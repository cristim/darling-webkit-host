/* Exercises the portable guest transport against the live host service,
 * including the paths the shell client did not cover: error events, resize
 * taking effect, sequence monotonicity, and the shared-memory path.
 */
#define _GNU_SOURCE
#include "dwb_client.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok, const char *detail)
{
	printf("%-54s %s%s%s\n", what, ok ? "PASS" : "FAIL",
	       detail && *detail ? "  " : "", detail ? detail : "");
	if (!ok)
		failures++;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/dwb.sock";
	const char *url = argc > 2 ? argv[2] : "https://example.com";
	const size_t region = 16u * 1024 * 1024;

	dwb_client c;
	if (dwb_client_connect(&c, path) != 0) {
		perror("connect");
		return 2;
	}
	printf("connected to %s\n\n", path);

	char *backend = NULL;
	int w = 0, h = 0;
	int supports_handlers = 1;
	int ok = dwb_client_hello(&c, &backend, &w, &h) == 0;
	if (ok && backend && strcmp(backend, "webkitgtk") != 0)
		supports_handlers = 0;
	check("hello: succeeds", ok, "");
	check("hello: names a backend", ok && backend && *backend, backend ? backend : "");
	check("hello: reports geometry", ok && w > 0 && h > 0, NULL);
	free(backend);

	check("navigate: succeeds", dwb_client_navigate(&c, url) == 0, c.error);

	char *val = NULL;
	ok = dwb_client_eval(&c, "document.title", &val) == 0;
	check("eval: succeeds", ok, c.error);
	check("eval: returns the title", ok && val && *val, val ? val : "");
	/* Chromium used to answer with a raw slice of the CDP reply; the guest
	 * must get a clean string from either backend. */
	check("eval: value is not a CDP fragment",
	      ok && val && !strstr(val, "\"result\""), val ? val : "");
	free(val);

	/* A number is a legitimate result and the app reads numbers back, so it is
	 * asserted exactly. Chromium used to answer a numeric evaluate with a
	 * different field of the same reply - "description" - because the extraction
	 * walked past the value without checking that it was a string. No test caught
	 * that, because every eval test asked for a string. */
	ok = dwb_client_eval(&c, "1+1", &val) == 0;
	check("eval: a numeric result is its own value, not another CDP field",
	      ok && val && strcmp(val, "2") == 0, val ? val : "");
	free(val);
	val = NULL;
	ok = dwb_client_eval(&c, "40+2", &val) == 0;
	check("eval: a multi-digit result is not confused with a field name",
	      ok && val && strcmp(val, "42") == 0, val ? val : "");
	free(val);
	val = NULL;

	/* A dotted expression whose first segment starts with a keyword. "document"
	 * begins "do" and "function.length" begins "function"; plain prefix matching
	 * classified both as statements, never captured the value, and returned
	 * empty for a title that is present - a successful call with a wrong answer
	 * again, and the most likely shape the app evaluates.
	 *
	 * Plus a script that throws, which must surface as a failure with the
	 * message intact rather than as a successful call whose value happens to
	 * start with "error:". */
	ok = dwb_client_eval(&c, "document.title", &val) == 0;
	check("eval: an expression starting with a keyword is still evaluated",
	      ok && val && *val, val ? val : c.error);
	free(val);
	val = NULL;
	ok = dwb_client_eval(&c, "function.length", &val) == 0;
	check("eval: a property access on a keyword-named global evaluates",
	      ok && val && strcmp(val, "1") == 0, val ? val : c.error);
	free(val);
	val = NULL;

	{
	int threw = dwb_client_eval(&c, "throw new Error('boom \"quoted\"')", &val);
	check("eval: a thrown script is a failure, not a successful 'error:' value",
	      threw != 0 && strstr(c.error, "boom \"quoted\"") != NULL,
	      c.error[0] ? c.error : (val ? val : "no error, no value"));
	free(val);
	val = NULL;
	}

	/* Script injection: install a document-start script that sets a marker, then
	 * prove it ran by reading the marker back out of the loaded page. This is
	 * the path a WKUserContentController client depends on. */
	ok = dwb_client_add_script(&c,
	        "window.__dwbProbe = 'injected-ok';"
	        "document.documentElement.setAttribute('data-dwb','1');", 1, 1) == 0;
	check("script: document-start injection accepted", ok, c.error);
	ok = dwb_client_navigate(&c, url) == 0;
	check("navigate after injection", ok, c.error);
	val = NULL;
	ok = dwb_client_eval(&c, "window.__dwbProbe || 'not-injected'", &val) == 0;
	check("script: marker present in the loaded page",
	      ok && val && strstr(val, "injected-ok"), val ? val : "");
	free(val);

	/* Named message handler: the page posts into it and the guest drains the
	 * queue by polling. Checked as a real round trip, not by trusting the
	 * registration acknowledgement. */
	ok = dwb_client_add_handler(&c, "dwbTest") == 0;
	if (!supports_handlers) {
		/* Refusing cleanly is the correct behaviour for a backend that does not
		 * implement handlers, and is asserted rather than skipped. */
		check("handler: unsupported backend refuses cleanly", !ok, c.error);
		check("handler: refusal carries a reason", !ok && c.error[0] != '\0', c.error);
	} else {
	check("handler: registered", ok, c.error);
	ok = dwb_client_eval(&c,
	        "(function(){try{webkit.messageHandlers.dwbTest.postMessage('ping-from-page');"
	        "return 'posted';}catch(e){return 'ERR:'+e.message;}})()", &val) == 0;
	check("handler: page can post", ok && val && strstr(val, "posted"),
	      val ? val : c.error);
	free(val);
	char *body = NULL;
	int got = 0;
	for (int i = 0; i < 12; i++) {
		got = dwb_client_poll_message(&c, "dwbTest", &body);
		if (got == 1)
			break;
		usleep(250000);
	}
	check("handler: guest drains the message", got == 1, body ? body : "queue empty");
	check("handler: body survived the round trip",
	      got == 1 && body && strstr(body, "ping-from-page"), body ? body : "");
	free(body);
	body = NULL;
	got = dwb_client_poll_message(&c, "dwbTest", &body);
	check("handler: queue empty once drained", got == 0, NULL);
	free(body);
	ok = dwb_client_poll_message(&c, "noSuchHandler", &body) != 1;
	check("handler: unknown channel is refused", ok, body ? body : "");
	}

	/* The app's own message, end to end: the channel name and the script shape
	 * both read out of the YouLearn binary. Every earlier handler test posted a
	 * bare string with no quotes, so the entire escaping bug - which truncated
	 * the script at the first escaped quote and reported it as a page syntax
	 * error - stayed invisible. This is the test that has a JSON body in it.
	 *
	 * Scoped to backends with handler support, because these five checks all
	 * depend on registering one. Chromium refuses handlers by design, and its
	 * refusal is covered by the capability-scope tests above; failing these
	 * there would only assert that a deliberate refusal happens. */
	if (supports_handlers) {
		int rc = dwb_client_add_handler(&c, "ylevent");
		check("app channel: ylevent registers", rc == 0, c.error);
		rc = dwb_client_navigate(&c, "https://example.com");
		check("app channel: page loaded", rc == 0, c.error);
		dwb_client_resize(&c, 800, 600);
		char *v = NULL;
		/* Exactly the call the app's injected script makes. */
		rc = dwb_client_eval(&c,
			"window.webkit.messageHandlers.ylevent.postMessage('{\"ev\":\"video\",\"state\":\"playing\"}')",
			&v);
		/* A successful postMessage returns no value. Reported as a failure
		 * before, because WebKit cannot marshal the result back. */
		check("app channel: postMessage is not an error", rc == 0, c.error);
		free(v);
		v = NULL;

		/* The body must arrive whole. Compared as an exact string, not by
		 * prefix, because the failure mode is truncation and a prefix test
		 * passes on a truncated body. */
		char *body = NULL;
		int got = 0;
		for (int i = 0; i < 12; i++) {
			got = dwb_client_poll_message(&c, "ylevent", &body);
			if (got != 0)
				break;
			usleep(250000);
		}
		check("app channel: body delivered", got == 1, body ? body : "queue empty");
		const char *want = "{\"ev\":\"video\",\"state\":\"playing\"}";
		check("app channel: body intact, not truncated at a quote",
		      body != NULL && strcmp(body, want) == 0, body ? body : "(null)");
		if (body) {
			printf("    got %zu bytes, expected %zu\n",
			       strlen(body), strlen(want));
			free(body);
		}

		/* A multiline body is the case a JSON envelope is most likely to
		 * mangle, and the one a real page sends. */
		v = NULL;
		rc = dwb_client_eval(&c,
			"window.webkit.messageHandlers.ylevent.postMessage('line1\\nline2\\ttabbed')",
			&v);
		check("app channel: multiline post is not an error", rc == 0, c.error);
		free(v);
		body = NULL;
		for (int i = 0; i < 12; i++) {
			got = dwb_client_poll_message(&c, "ylevent", &body);
			if (got != 0)
				break;
			usleep(250000);
		}
		check("app channel: escapes decoded, newlines preserved",
		      body != NULL && strcmp(body, "line1\nline2\ttabbed") == 0,
		      body ? body : "(null)");
		free(body);
	}

	/* Envelope round trip for the fields that were still interpolated raw: a
	 * URL, a channel name and the shared-region path. Each is read back by the
	 * host's decoder, so a quote in any of them was truncated exactly as a
	 * script was. The channel name is the interesting one - it is echoed in
	 * every reply, so a truncated registration would have made the guest poll a
	 * name the host never registered. */
	if (supports_handlers) {
		const char *quoteName = "dw\"bProbe";
		int rc = dwb_client_add_handler(&c, quoteName);
		check("envelope: a channel name with a quote registers", rc == 0, c.error);
		char *v = NULL;
		rc = dwb_client_eval(&c,
		        "try{webkit.messageHandlers[\"dw\\\"bProbe\"]"
		        ".postMessage('a\"b\\nc');return 'posted';}catch(e){return 'ERR:'+e.message;}",
		        &v);
		check("envelope: posting on a quoted channel name works",
		      rc == 0 && v && strstr(v, "posted"), v ? v : c.error);
		free(v);
		v = NULL;
		char *body = NULL;
		int got = 0;
		for (int i = 0; i < 12; i++) {
			got = dwb_client_poll_message(&c, quoteName, &body);
			if (got != 0)
				break;
			usleep(250000);
		}
		/* Exact comparison: the failure mode is truncation, and a substring
		 * test passes on a truncated body. */
		check("envelope: quoted channel body round-trips intact",
		      got == 1 && body && strcmp(body, "a\"b\nc") == 0,
		      body ? body : "queue empty");
		free(body);
	}

	/* A URL carrying a quote in its query string reaches the host's url field
	 * and back out again in the error event. */
	{
		char *v = NULL;
		int rc = dwb_client_navigate(&c,
		        "https://nonexistent.invalid/x?q=\"quoted\"&r=back\\slash");
		/* Expected to fail: the host does not exist. The point is that the
		 * failure is a navigation error, not a malformed request. */
		check("envelope: a URL with quotes is accepted and fails honestly",
		      rc != 0 && c.error[0] != '\0', c.error);
		free(v);
		/* This leaves the engine on an error page, so anything evaluating
		 * afterwards has to navigate back. Not doing so made the two checks
		 * below report a stale navigation error, and one of them failed for
		 * that reason alone - a test failure caused by the test's own ordering
		 * rather than by the code under test. */
		ok = dwb_client_navigate(&c, url) == 0;
		check("envelope: the engine recovers for the next test", ok, c.error);
	}

	/* Error text carrying quotes must survive the trip intact.
	 *
	 * The useful reasons quote the thing that went wrong, and pasted in raw the
	 * reply was unparseable, so the client replaced a specific explanation with a
	 * generic failure. A thrown Error is the honest probe: the message is ours to
	 * choose, so it can be made to contain a quote, a newline and a backslash.
	 *
	 * The first attempt at this used a syntax error, which asserted that the
	 * engine's own message contained a quote. It does not - WebKit reports
	 * "SyntaxError: Unexpected EOF" and quotes nothing - so the test was wrong
	 * about the world rather than the code being wrong, and passing a real
	 * truncation is worth more than a green assertion that proves nothing. */
	{
		char *v = NULL;
		int rc = dwb_client_eval(&c,
		        "throw new Error('he said \"hi\"\\nsecond\\ttab')", &v);
		check("error text: quotes and escapes in a thrown message survive",
		      rc != 0 && strstr(c.error, "he said \"hi\"") != NULL, c.error);
		check("error text: the newline in a thrown message survives",
		      rc != 0 && strchr(c.error, '\n') != NULL, c.error);
		free(v);
	}

	/* The app's real sequence, end to end, using only the selectors the YouLearn
	 * binary provably references.
	 *
	 * Every other check here probes one capability in isolation. This one runs
	 * them in the order the app runs them - construct, install a document-start
	 * script, register the handler channel, navigate, then post an event and read
	 * it back - because that ordering is what the app depends on, and a set of
	 * independent capability checks cannot show it working as a whole. The bug
	 * this session spent longest on, a message truncated at its first escaped
	 * quote, only showed up once the real sequence ran in one go.
	 *
	 * Which selectors are confirmed in the binary and which are only part of the
	 * API this guest stands in for is recorded in KNOWN-ISSUES.md. */
	if (supports_handlers) {
		ok = dwb_client_add_script(&c,
		        "window.__yle = 'start'; var __t = document.title;", 1, 1) == 0;
		check("app sequence: document-start script accepted", ok, c.error);
		ok = dwb_client_add_handler(&c, "ylevent") == 0;
		check("app sequence: the app's channel accepted", ok, c.error);
		ok = dwb_client_navigate(&c, url) == 0;
		check("app sequence: navigate", ok, c.error);
		dwb_client_resize(&c, 800, 600);
		usleep(2500000);

		val = NULL;
		ok = dwb_client_eval(&c, "window.__yle", &val) == 0;
		check("app sequence: the injected script actually ran",
		      ok && val && strcmp(val, "start") == 0, val ? val : c.error);
		free(val);
		val = NULL;

		ok = dwb_client_eval(&c,
		        "window.webkit.messageHandlers.ylevent.postMessage('{\"ev\":\"video\"}')",
		        &val) == 0;
		check("app sequence: the app's postMessage is not an error", ok, c.error);
		free(val);
		val = NULL;

		char *seqBody = NULL;
		int seqGot = 0;
		for (int i = 0; i < 12; i++) {
			seqGot = dwb_client_poll_message(&c, "ylevent", &seqBody);
			if (seqGot != 0)
				break;
			usleep(250000);
		}
		/* Exact: the failure is truncation, and a substring test passes on a
		 * truncated body. */
		check("app sequence: the app's event reaches the guest intact",
		      seqGot == 1 && seqBody && strcmp(seqBody, "{\"ev\":\"video\"}") == 0,
		      seqBody ? seqBody : "queue empty");
		free(seqBody);
	}

	/* The response the host reports for a completed load. Needed by a guest's
	 * webView:decidePolicyForNavigationResponse:, which has nothing to inspect
	 * without it, so it is asserted like any other part of the contract: the URL
	 * is the one asked for, the MIME type is present, and the status is a real
	 * HTTP status. */
	{
		dwb_load_info info;
		memset(&info, 0, sizeof info);
		ok = dwb_client_navigate_info(&c, url, &info) == 0;
		check("response: a completed load reports a response", ok, c.error);
		/* What a backend reports is a property of the backend, not of the
		 * contract. webkitgtk captures the main resource; chromium does not, and
		 * reports nothing. Both are correct, so the detail checks are scoped to
		 * a backend that actually reports, and the absent case is asserted to be
		 * harmless - that is the half that matters, because the guest reads a
		 * missing response as unknown rather than as a failed load. */
		if (info.url != NULL) {
			check("response: the URL is the one that was loaded",
			      strstr(info.url, "example.com") != NULL, info.url);
			check("response: a MIME type comes with the URL",
			      info.mime != NULL && *info.mime != '\0', info.mime);
			check("response: the status is a real HTTP status",
			      info.status >= 200 && info.status < 400, "not reported");
			printf("    url=%s mime=%s status=%d\n",
			       info.url, info.mime ? info.mime : "(null)", info.status);
		} else {
			check("response: a backend that reports none still reports the load",
			      ok, "navigate failed instead");
			check("response: an absent response is not an error",
			      c.error[0] == '\0', c.error);
			printf("    no response reported by this backend; load still succeeded\n");
		}
		free(info.url);
		free(info.mime);
	}

	/* A URL the host cannot load must surface as an error, not as success. */
	int rc = dwb_client_navigate(&c, "https://nonexistent.invalid./x");
	check("navigate: bad URL reports an error", rc != 0, c.error);

	/* Back to a good page, then resize and confirm geometry follows. */
	dwb_client_navigate(&c, url);
	int nw = 480, nh = 360;
	dwb_client_resize(&c, nw, nh);
	dwb_frame_header fh;
	const void *px = NULL;
	size_t pbytes = 0;
	void *map = NULL;
	size_t mapsz = 0;
	int in_shm = 0;
	ok = dwb_client_frame(&c, &fh, &px, &pbytes, &map, &mapsz, &in_shm) == 0;
	check("frame after resize: received", ok, c.error);
	check("frame after resize: geometry honoured",
	      ok && (int)fh.width == nw && (int)fh.height == nh, NULL);
	printf("    after resize: %ux%u stride=%u format=%u bytes=%u\n",
	       fh.width, fh.height, fh.stride, fh.format, fh.size);
	dwb_client_free_frame(&c, (void *)px);

	/* Sequence must advance so the guest can drop stale frames. */
	uint32_t prev = 0;
	int seq_ok = 1;
	for (int i = 0; i < 3; i++) {
		ok = dwb_client_frame(&c, &fh, &px, &pbytes, &map, &mapsz, &in_shm) == 0;
		if (!ok || fh.seq <= prev)
			seq_ok = 0;
		prev = fh.seq;
		dwb_client_free_frame(&c, (void *)px);
	}
	check("frame: sequence increases across pulls", seq_ok, NULL);

	/* Frame layout: the header-to-display decision, tested directly. This is
	 * the logic that was wrong twice - a rep built around NULL, and a
	 * compressed frame drawn as raw. */
	{
		dwb_frame_layout lay;
		ok = dwb_client_frame(&c, &fh, &px, &pbytes, &map, &mapsz, &in_shm) == 0;
		check("layout: real frame describable", ok, c.error);
		size_t have = in_shm ? (mapsz ? mapsz : 0) : (sizeof(dwb_frame_header) + pbytes);
		int rc = dwb_frame_describe(&fh, have, &lay);
		check("layout: real frame accepted", rc == 0, "");
		printf("    format=%u -> is_raw=%d comps=%d decode=%d offset=%zu bytes=%zu\n",
		       fh.format, lay.is_raw, lay.components, lay.needs_decode,
		       lay.pixel_offset, lay.pixel_bytes);
		if (fh.format == DWB_PIXEL_JPEG)
			check("layout: compressed frame flagged for decode, not raw",
			      !lay.is_raw && lay.needs_decode, "");
		else
			check("layout: raw frame gives components",
			      lay.is_raw && lay.components >= 3, "");
		check("layout: offset skips the header", lay.pixel_offset == sizeof(dwb_frame_header), "");
		dwb_client_free_frame(&c, (void *)px);

		/* A header that claims more bytes than were delivered must be
		 * rejected, not turned into an out-of-bounds read. */
		dwb_frame_header bad = fh;
		bad.size = 0xfffffff0u;
		check("layout: oversized frame rejected",
		      dwb_frame_describe(&bad, 4096, &lay) != 0, "");
		bad = fh;
		bad.stride = 1; bad.width = 64; bad.height = 64;
		bad.format = DWB_PIXEL_RGBA;
		check("layout: stride narrower than the row rejected",
		      dwb_frame_describe(&bad, 1u << 20, &lay) != 0, "");
		bad = fh;
		bad.format = DWB_PIXEL_RGBA; bad.width = 0;
		check("layout: zero-width frame rejected",
		      dwb_frame_describe(&bad, 1u << 20, &lay) != 0, "");

		/* The silent-failure case: if the two ends ever disagree about the
		 * header layout, the guest reads plausible width/height/stride from the
		 * wrong offsets and blits whatever follows. A wrong magic is the one
		 * reliable signal, so it must be caught rather than drawn. */
		bad = fh;
		bad.magic = 0xDEADBEEFu;
		lay.is_raw = 99;
		check("layout: wrong magic rejected",
		      dwb_frame_describe(&bad, 1u << 20, &lay) != 0, "");
		check("layout: wrong magic is reported, not guessed at",
		      lay.bad_magic == 1, "");
	}

	/* Shared-memory path, driven through the transport rather than raw
	 * sockets this time. */
	char shm_path[256];
	snprintf(shm_path, sizeof(shm_path),
	         "/home/cristi/.local/share/agent-tmp/youlearn/frame-shm-c%zu", region);
	int sfd = open(shm_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (sfd < 0 || ftruncate(sfd, (off_t)region) != 0) {
		perror("shm");
		return 2;
	}
	unsigned char *shm = mmap(NULL, region, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
	close(sfd);
	memset(shm, 0, region);

	ok = dwb_client_attach_shm(&c, shm_path, region) == 0;
	check("shm: attach accepted", ok, c.error);
	ok = dwb_client_frame(&c, &fh, &px, &pbytes, &map, &mapsz, &in_shm) == 0;
	check("shm: frame received", ok, c.error);
	check("shm: flagged as in-memory", ok && in_shm, NULL);
	check("shm: no socket buffer handed to caller", ok && px == NULL, NULL);
	int nonzero = 0;
	if (ok)
		for (uint32_t i = 0; i < fh.size; i += 1013)
			if (shm[sizeof(dwb_frame_header) + i])
				nonzero = 1;
	check("shm: pixels readable in the region", nonzero, NULL);

	munmap(shm, region);
	unlink(shm_path);
	dwb_client_close(&c);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
