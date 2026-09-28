/* Chromium backend, driven over the DevTools Protocol.
 *
 * Not a WebKit engine, and that is the point: it is the availability and
 * media-compatibility fallback. CDP is versioned and already implemented by
 * Chromium, so this backend speaks an existing protocol rather than inventing
 * one. Frames come from Page.startScreencast.
 *
 * Tradeoff worth knowing: screencast frames are JPEG, so this backend is good
 * for page fidelity and light animation but is the wrong choice for smooth
 * video. The guest can tell them apart because the frame header carries the
 * backend name in the HELLO_ACK.
 */
#include <gtk/gtk.h>

#include "../backend.h"
#include "../protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

/* Candidate binaries, in preference order. Probed by looking for an executable
 * file rather than by running --version, so probe() stays cheap. */
static const char *const chromium_candidates[] = {
	"/usr/bin/chromium",
	"/usr/bin/chromium-browser",
	"/usr/bin/google-chrome",
	"/usr/bin/google-chrome-stable",
	NULL,
};

typedef struct {
	pid_t pid;
	int ws_port;      /* devtools websocket port */
	int ws_fd;        /* connected socket */
	int next_id;
	char *chrome_ws_url;
	/* Most recent screencast frame, JPEG bytes. */
	unsigned char *frame;
	size_t frame_size;
	uint32_t seq;
	int width, height;
	char *pending;    /* accumulated socket input */
	size_t pending_size;
	int io_next_id;   /* id awaiting an IO.read reply */
	int eval_next_id; /* id awaiting a Runtime.evaluate reply */
	gboolean loaded;  /* set when Page.loadEventFired arrives */
} cr_impl;

/* Defined below cr_navigate, which uses it to confirm a frame is obtainable. */
static int cr_capture(cr_impl *self, char **error);
static void cr_handle_message(cr_impl *self, const char *json);

/* ws_pump takes a 3-argument callback; this adapts it to the 2-argument
 * handler. Every drain must go through this, not NULL: passing NULL makes
 * ws_pump discard the message, which silently threw away both the screencast
 * frames and the load event. */
static void cr_pump_handler(cr_impl *self, const char *json, void *user_data)
{
	(void)user_data;
	cr_handle_message(self, json);
}

/* Bounds every socket operation. Without this a read that the peer never
 * answers - Chromium's DevTools server keeps HTTP/1.1 connections open and
 * ignores "Connection: close" - blocks forever, and the caller hangs with no
 * diagnostic. */
static void set_socket_timeouts(int fd, int seconds)
{
	struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void set_error(char **slot, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void set_error(char **slot, const char *fmt, ...)
{
	if (!slot)
		return;
	va_list ap;
	va_start(ap, fmt);
	char *msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_free(*slot);
	*slot = msg;
}

static const char *find_chromium(void)
{
	for (int i = 0; chromium_candidates[i]; i++)
		if (access(chromium_candidates[i], X_OK) == 0)
			return chromium_candidates[i];
	return NULL;
}

static int cr_probe(void)
{
	/* GTK is only needed for the base surface, and a missing display should not
	 * disqualify a headless engine, so this checks the binary alone. */
	return find_chromium() ? 1 : 0;
}

/* --- minimal WebSocket client (text frames only, no extensions) ----------- */

static int ws_send_frame(int fd, const char *payload, size_t len)
{
	/* Client frames must be masked, so header and payload cannot be written
	 * separately without masking the body first. The combined buffer has to be
	 * sized for both: a fixed 10-byte header array overflows the moment any
	 * payload is written into it. */
	size_t max_header = 14;
	unsigned char *frame = malloc(max_header + len);
	if (!frame)
		return -1;
	size_t hlen = 0;
	frame[hlen++] = 0x81; /* FIN + text opcode */
	if (len < 126) {
		frame[hlen++] = (unsigned char)(0x80 | len); /* MASK, 7-bit length */
	} else if (len <= 0xFFFF) {
		frame[hlen++] = (unsigned char)(0x80 | 126);
		frame[hlen++] = (unsigned char)((len >> 8) & 0xFF);
		frame[hlen++] = (unsigned char)(len & 0xFF);
	} else {
		frame[hlen++] = (unsigned char)(0x80 | 127);
		for (int i = 7; i >= 0; i--)
			frame[hlen++] = (unsigned char)((len >> (i * 8)) & 0xFF);
	}
	/* Fixed masking key: this backend only ever talks to a loopback DevTools
	 * endpoint, so a constant key is acceptable and keeps the code small. */
	static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
	for (int i = 0; i < 4; i++)
		frame[hlen++] = mask[i];
	for (size_t i = 0; i < len; i++)
		frame[hlen + i] = (unsigned char)(payload[i]) ^ mask[i % 4];

	size_t total = hlen + len;
	size_t sent = 0;
	while (sent < total) {
		ssize_t n = write(fd, frame + sent, total - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(frame);
			return -1;
		}
		sent += (size_t)n;
	}
	free(frame);
	return 0;
}

static int ws_connect(const char *host, int port, const char *path)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close(fd);
		return -1;
	}
	set_socket_timeouts(fd, 10);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	char req[512];
	int n = snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n", path, host, port);
	if (write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	/* Read up to the end of the HTTP upgrade response. */
	char buf[1024];
	ssize_t got = read(fd, buf, sizeof(buf) - 1);
	if (got <= 0) {
		close(fd);
		return -1;
	}
	buf[got] = '\0';
	if (!strstr(buf, "101")) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Handles one decoded CDP message. Defined at the bottom of the file; declared
 * here because cr_wait_reply needs to route replies through it. */
static void cr_handle_message(cr_impl *self, const char *json);

/* Pulls complete text messages out of the socket, calling `on_msg` for each.
 * `on_msg` may be NULL, in which case messages are drained and discarded -
 * which is what the screencast-drain paths want. */
static void ws_pump(cr_impl *self, void (*on_msg)(cr_impl *, const char *, void *), void *ud)
{
	for (;;) {
		struct pollfd pfd = { .fd = self->ws_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 0);
		if (pr <= 0)
			return;
		unsigned char head[2];
		ssize_t n = read(self->ws_fd, head, 2);
		if (n != 2)
			return;
		size_t len = head[1] & 0x7F;
		if (len == 126) {
			unsigned char ext[2];
			if (read(self->ws_fd, ext, 2) != 2)
				return;
			len = ((size_t)ext[0] << 8) | ext[1];
		} else if (len == 127) {
			unsigned char ext[8];
			if (read(self->ws_fd, ext, 8) != 8)
				return;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | ext[i];
		}
		if (len == 0 || len > 32u * 1024 * 1024)
			return;
		char *payload = malloc(len + 1);
		if (!payload)
			return;
		size_t got = 0;
		while (got < len) {
			ssize_t r = read(self->ws_fd, payload + got, len - got);
			if (r <= 0) {
				free(payload);
				return;
			}
			got += (size_t)r;
		}
		payload[len] = '\0';
		if (g_getenv("DWB_DEBUG_CDP")) {
			char *flat = g_strdup(payload);
			for (char *p = flat; *p; p++)
				if (*p == '\n')
					*p = ' ';
			g_printerr("[cdp] %s\n", flat);
			g_free(flat);
		}
		if ((head[0] & 0x0F) == 0x01 && on_msg)
			on_msg(self, payload, ud);
		free(payload);
	}
}

static int cr_send_cmd_raw(cr_impl *self, int id, const char *method, const char *params);

static int cr_send_cmd(cr_impl *self, int id, const char *method, const char *params_fmt, ...)
{
	char buf[4096];
	if (params_fmt) {
		va_list ap;
		va_start(ap, params_fmt);
		vsnprintf(buf, sizeof(buf), params_fmt, ap);
		va_end(ap);
	} else {
		/* Must be a valid empty object. Emitting nothing here yields
		 * "params":} , which every command rejects with -32700. */
		snprintf(buf, sizeof(buf), "{}");
	}
	char payload[8192];
	snprintf(payload, sizeof(payload),
	         "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, buf);
	return ws_send_frame(self->ws_fd, payload, strlen(payload));
}

/* Same, with the params object already built - used where the params contain a
 * string too long or too awkward for a format argument, which is the case for
 * every JavaScript expression. */
static int cr_send_cmd_raw(cr_impl *self, int id, const char *method, const char *params)
{
	size_t need = strlen(params) + strlen(method) + 64;
	char *payload = g_malloc0(need);
	snprintf(payload, need, "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
	         id, method, params);
	int rc = ws_send_frame(self->ws_fd, payload, strlen(payload));
	g_free(payload);
	return rc;
}

/* Waits for the reply carrying `id`, running the message handler for everything
 * else (screencast frames, events) on the way. */
/* True when the payload is a reply for exactly this id. Requires the digits
 * after the colon to be followed by a non-digit, so 5 does not match 50. */
static int cr_payload_has_id(const char *payload, int id)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"id\":");
	const char *at = strstr(payload, pat);
	while (at) {
		const char *digits = at + strlen(pat);
		char *endp = NULL;
		long v = strtol(digits, &endp, 10);
		/* A JSON id is a bare number: something non-digit, or the end, follows. */
		if (endp && endp != digits && (!*endp || (*endp >= '0' && *endp <= '9') == 0)) {
			if (v == (long)id)
				return 1;
		}
		at = strstr(at + 1, pat);
	}
	return 0;
}

static char *cr_wait_reply_until(cr_impl *self, int id, int timeout_ms, gint64 deadline);

static char *cr_wait_reply(cr_impl *self, int id, int timeout_ms)
{
	return cr_wait_reply_until(self, id, timeout_ms, g_get_monotonic_time() + timeout_ms * 1000);
}

/* cr_wait_reply discards every message that is not the reply it is waiting for,
 * and on chromium a screencast frame arrives every few milliseconds for as long
 * as the page is visible. The recursion passed the full timeout down each time,
 * so a page that never stopped producing frames - which is every page with a
 * video on it, i.e. the app this proxy exists for - reached the recursion tail
 * before the deadline and evaluate() reported:
 *
 *     no reply to Runtime.evaluate
 *
 * for a command the browser had answered immediately. The deadline is now
 * absolute and passed down, so discarding frames cannot extend the wait.
 * Bounded as well: an unbounded tail call on a chatty socket is a stack risk
 * independent of the timeout. */
static char *cr_wait_reply_until(cr_impl *self, int id, int timeout_ms, gint64 deadline)
{
	int remaining = (int)((deadline - g_get_monotonic_time()) / 1000);
	if (remaining <= 0)
		return NULL;
	struct pollfd pfd = { .fd = self->ws_fd, .events = POLLIN };
	int pr = poll(&pfd, 1, remaining);
	if (pr <= 0)
		return NULL;
	unsigned char head[2];
	if (read(self->ws_fd, head, 2) != 2)
		return NULL;
	size_t len = head[1] & 0x7F;
	if (len == 126) {
		unsigned char ext[2];
		if (read(self->ws_fd, ext, 2) != 2)
			return NULL;
		len = ((size_t)ext[0] << 8) | ext[1];
	} else if (len == 127) {
		unsigned char ext[8];
		if (read(self->ws_fd, ext, 8) != 8)
			return NULL;
		len = 0;
		for (int i = 0; i < 8; i++)
			len = (len << 8) | ext[i];
	}
	if (len == 0 || len > 8u * 1024 * 1024)
		return NULL;
	char *payload = malloc(len + 1);
	size_t got = 0;
	while (got < len) {
		ssize_t r = read(self->ws_fd, payload + got, len - got);
		if (r <= 0) {
			free(payload);
			return NULL;
		}
		got += (size_t)r;
	}
	payload[len] = '\0';
	cr_handle_message(self, payload);

	/* Only the reply carrying our id is the answer. Anything else on the socket
	 * - a screencast frame, or a reply to an earlier or later command - must be
	 * discarded, or evaluate() hands the guest a stale message.
	 *
	 * Matching is on the id as a complete number, not a substring. strstr("\"id\":5")
	 * also matches \"id\":50 and \"id\":53, so waiting for reply 5 happily
	 * consumed reply 50 instead, and the guest got another command's result:
	 *
	 *     1+1  rc=0 [description]
	 *     document.querySelectorAll('p').length  rc=0 [description]
	 *
	 * - a successful call returning the wrong value, which is worse than an
	 * error because nothing downstream can tell. Chromium's ids climb into the
	 * hundreds on a page with a video, so the odds of hitting a neighbour are
	 * high, exactly when it matters most. */
	if (!cr_payload_has_id(payload, id)) {
		free(payload);
		return cr_wait_reply_until(self, id, timeout_ms, deadline);
	}
	/* Ownership passes to the caller, which frees it. Freeing here and
	 * returning the pointer gave callers a dangling buffer, and their free
	 * turned that into a double free. */
	return payload;
}

/* Issues a GET to the loopback DevTools HTTP endpoint and returns the whole
 * body. A single read() is not enough: /json/list is a few KB, so the key we
 * want routinely lands in a later TCP segment than the first one. */
static char *devtools_http_get(int port, const char *path)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	set_socket_timeouts(fd, 2);
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}
	char req[256];
	int rn = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
	                                     "Connection: close\r\n\r\n", path, port);
	if (write(fd, req, (size_t)rn) != rn) {
		close(fd);
		return NULL;
	}

	size_t cap = 16384, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(fd);
		return NULL;
	}
	/* Read until the body is complete, the peer closes, or the socket times
	 * out. Chromium may hold the connection open, so a timeout is a normal
	 * stopping condition here, not an error. */
	for (;;) {
		if (len + 4096 > cap) {
			char *bigger = realloc(buf, cap * 2);
			if (!bigger) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = bigger;
			cap *= 2;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n <= 0)
			break; /* EOF, timeout, or error: all end the read loop */
		len += (size_t)n;
		/* The JSON array is complete once its closing bracket has arrived. */
		char *body = strstr(buf, "\r\n\r\n");
		if (body) {
			char *end = strrchr(body + 4, ']');
			if (end) {
				len = (size_t)(end - buf) + 1;
				break;
			}
		}
	}
	buf[len] = '\0';
	close(fd);

	/* Skip the HTTP headers. */
	char *body = strstr(buf, "\r\n\r\n");
	if (!body) {
		free(buf);
		return NULL;
	}
	char *result = g_strdup(body + 4);
	free(buf);
	return result;
}

/* Pulls the value of a "key": "value" pair, starting the search at `from`. */
/* Value of a JSON string field, or NULL.
 *
 * The previous version searched for the key, skipped to the next colon, and took
 * whatever string appeared after that. For a Runtime.evaluate reply:
 *
 *     {"id":7,"result":{"result":{"type":"number","value":2,"description":"2"}}}
 *
 * it never looked at the fact that "value" here is the number 2. It walked past
 * the colon to "description" and returned the string "2" - and for a numeric
 * result it returned the description of some other field entirely, which is how
 * 1+1 came back as "description":
 *
 *     1+1  rc=0 [description]
 *
 * A successful call with the wrong value, which is worse than an error because
 * nothing downstream can detect it. The value has to be the one belonging to the
 * key that was asked for, and only a string value can be returned here - a
 * numeric or boolean result is not a JSON string, so it is left to the caller
 * rather than fabricated.
 */
static char *json_string_value(const char *from, const char *key)
{
	const char *k = strstr(from, key);
	if (!k)
		return NULL;
	k += strlen(key);
	/* Skip whitespace to the value. */
	while (*k == ' ' || *k == '\t')
		k++;
	if (*k != ':')
		return NULL;
	k++;
	while (*k == ' ' || *k == '\t')
		k++;
	/* A string value is quoted. Anything else is not a string, and guessing at
	 * it is what produced the wrong answers. */
	if (*k != '"')
		return NULL;
	k++;
	const char *end = k;
	while (*end && *end != '"') {
		if (*end == '\\' && end[1])
			end++;
		end++;
	}
	if (*end != '"')
		return NULL;

	/* The raw slice still carries CDP's JSON escapes. Decoded here so a caller
	 * sees the text the engine actually wrote: a thrown message with a newline
	 * arrived as a literal backslash-n, which is the CDP encoding leaking
	 * through into an error a human reads. A string result is affected too -
	 * a document title containing a quote or a newline came back escaped. */
	gsize raw = (gsize)(end - k);
	char *raw_out = g_strndup(k, raw);
	char *out = g_malloc0(raw + 1);
	size_t w = 0;
	for (size_t i = 0; i < raw; i++) {
		if (raw_out[i] != '\\' || i + 1 >= raw) {
			out[w++] = raw_out[i];
			continue;
		}
		i++;
		switch (raw_out[i]) {
		case 'n': out[w++] = '\n'; break;
		case 'r': out[w++] = '\r'; break;
		case 't': out[w++] = '\t'; break;
		case 'b': out[w++] = '\b'; break;
		case 'f': out[w++] = '\f'; break;
		case 'u': {
			if (i + 4 >= raw) { out[w++] = raw_out[i]; break; }
			char hex[5] = { raw_out[i+1], raw_out[i+2], raw_out[i+3], raw_out[i+4], 0 };
			char *endp = NULL;
			long v = strtol(hex, &endp, 16);
			if (endp && *endp == 0 && v > 0 && v < 0x80) {
				out[w++] = (char)v; i += 4; break;
			}
			if (endp && *endp == 0 && v >= 0x80 && v < 0x800) {
				out[w++] = (char)(0xC0 | (v >> 6));
				out[w++] = (char)(0x80 | (v & 0x3F)); i += 4; break;
			}
			if (endp && *endp == 0 && v >= 0x800 && v < 0x10000) {
				out[w++] = (char)(0xE0 | (v >> 12));
				out[w++] = (char)(0x80 | ((v >> 6) & 0x3F));
				out[w++] = (char)(0x80 | (v & 0x3F)); i += 4; break;
			}
			out[w++] = raw_out[i];
			break;
		}
		default: out[w++] = raw_out[i]; break;
		}
	}
	out[w] = '\0';
	g_free(raw_out);
	return out;
}

/* A JSON value that is not a string: a number, true, false or null. Returns a
 * malloc'd rendering, or NULL when the field holds a string or is absent. */
static char *json_scalar_value(const char *from, const char *key)
{
	const char *k = strstr(from, key);
	if (!k)
		return NULL;
	k += strlen(key);
	while (*k == ' ' || *k == '\t')
		k++;
	if (*k != ':')
		return NULL;
	k++;
	while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r')
		k++;
	if (*k == '"' || *k == '{' || *k == '[' || *k == '\0')
		return NULL;              /* not a scalar, or absent */
	/* A scalar runs to the next comma, brace or whitespace. */
	const char *end = k;
	while (*end && *end != ',' && *end != '}' && *end != ' ' && *end != '\n' &&
	       *end != '\r' && *end != ']')
		end++;
	if (end == k)
		return NULL;
	return g_strndup(k, (gsize)(end - k));
}

static dwb_backend *cr_create(int width, int height, char **error)
{
	const char *bin = find_chromium();
	if (!bin) {
		set_error(error, "no chromium binary found");
		return NULL;
	}

	cr_impl *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	self->ws_fd = -1;
	self->width = width;
	self->height = height;

	int port = 0;
	for (int attempt = 0; attempt < 32 && port == 0; attempt++)
		port = 19222 + attempt;

	char portbuf[32];
	/* The "=" form is required, not stylistic. /usr/bin/chromium on this host is
	 * a launcher stub that injects its own defaults (ozone, password store,
	 * omarchy extensions) and mangles argv; when the flag and its value are two
	 * separate tokens the value is dropped, the stub's own port handling wins,
	 * and nothing ever listens. One token survives the rewrite. */
	snprintf(portbuf, sizeof(portbuf), "--remote-debugging-port=%d", port);

	char profilebuf[128];
	snprintf(profilebuf, sizeof(profilebuf), "--user-data-dir=/tmp/dwb-chromium-profile-%d",
	         port);

	char *args[] = {
		(char *)bin,
		(char *)"--headless=new",
		(char *)"--no-sandbox",
		(char *)"--disable-gpu",
		(char *)"--hide-scrollbars",
		(char *)"--mute-audio",
		(char *)"--no-first-run",
		(char *)"--no-default-browser-check",
		/* No positional URL: modern headless Chromium rejects one ("Multiple
		 * targets are not supported"), and navigation goes through
		 * Page.navigate anyway. */
		portbuf,
		profilebuf,
		NULL,
	};

	self->pid = fork();
	if (self->pid < 0) {
		set_error(error, "fork failed: %s", strerror(errno));
		free(self);
		return NULL;
	}
	if (self->pid == 0) {
		/* Detach stdio. Without this the child inherits our stdout, so any
		 * caller reading our output never sees EOF while the browser is alive,
		 * and a long-lived helper can wedge a pipeline. It is also correct
		 * default behaviour for a spawned engine. */
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		setsid();
		execv(bin, args);
		_exit(127);
	}
	self->ws_port = port;

	/* Wait for the debugging endpoint to come up. /json/list is used rather
	 * than /json/version because it yields a page target we can drive directly,
	 * instead of the browser-level endpoint that cannot host a page. */
	for (int attempt = 0; attempt < 60 && !self->chrome_ws_url; attempt++) {
		/* If the child died, say so rather than looping for 15s on a process
		 * that is never going to listen. This is the difference between "the
		 * browser is slow" and "execv never happened". */
		int status = 0;
		pid_t reaped = waitpid(self->pid, &status, WNOHANG);
		if (reaped == self->pid) {
			if (WIFEXITED(status))
				set_error(error, "chromium exited immediately with status %d (%s)",
				          WEXITSTATUS(status), bin);
			else if (WIFSIGNALED(status))
				set_error(error, "chromium was killed by signal %d (%s)",
				          WTERMSIG(status), bin);
			else
				set_error(error, "chromium terminated abnormally (%s)", bin);
			self->pid = 0;
			free(self);
			return NULL;
		}

		char *body = devtools_http_get(port, "/json/list");
		if (body) {
			/* The list is not only pages: bundled extensions contribute
			 * background_page entries, and driving one of those silently does
			 * nothing. Prefer a real page, fall back to whatever is there. */
			const char *page = strstr(body, "\"type\": \"page\"");
			if (!page)
				page = strstr(body, "\"type\":\"page\"");
			self->chrome_ws_url = json_string_value(page ? page : body,
			                                        "\"webSocketDebuggerUrl\"");
			if (self->chrome_ws_url && !g_str_has_prefix(self->chrome_ws_url, "ws://")) {
				g_clear_pointer(&self->chrome_ws_url, g_free);
			}
			g_free(body);
		}
		if (self->chrome_ws_url)
			break;
		usleep(250000);
	}
	if (!self->chrome_ws_url) {
		set_error(error, "chromium devtools endpoint did not come up on port %d", port);
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		free(self);
		return NULL;
	}

	/* Connect to the page target advertised by /json/list. */
	int ws_fd;
	{
		const char *p = strstr(self->chrome_ws_url, "ws://");
		char host[64] = "127.0.0.1";
		int pp = port;
		const char *slash = p ? strchr(p + 5, '/') : NULL;
		if (p) {
			size_t hl = slash ? (size_t)(slash - (p + 5)) : strlen(p + 5);
			if (hl < sizeof(host)) {
				memcpy(host, p + 5, hl);
				host[hl] = '\0';
				char *colon = strrchr(host, ':');
				if (colon) {
					*colon = '\0';
					pp = atoi(colon + 1);
				}
			}
		}
		ws_fd = ws_connect(host, pp ? pp : port, slash ? slash : "/");
	}
	if (ws_fd < 0) {
		/* Fall back to whatever the version endpoint advertised. */
		const char *p = strstr(self->chrome_ws_url, "ws://");
		if (p) {
			char host[64];
			int pp = 0;
			const char *slash = strchr(p + 5, '/');
			size_t hl = slash ? (size_t)(slash - (p + 5)) : strlen(p + 5);
			if (hl < sizeof(host)) {
				memcpy(host, p + 5, hl);
				host[hl] = '\0';
				char *colon = strrchr(host, ':');
				if (colon) {
					*colon = '\0';
					pp = atoi(colon + 1);
				}
				ws_fd = ws_connect(host, pp ? pp : port, slash ? slash : "/");
			}
		}
	}
	if (ws_fd < 0) {
		set_error(error, "could not open a devtools websocket");
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		g_free(self->chrome_ws_url);
		free(self);
		return NULL;
	}
	self->ws_fd = ws_fd;
	self->next_id = 1;

	cr_send_cmd(self, self->next_id++, "Page.enable", NULL);
	cr_send_cmd(self, self->next_id++, "Runtime.enable", NULL);
	cr_send_cmd(self, self->next_id++, "Page.startScreencast",
	            "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":%d,\"maxHeight\":%d,"
	            "\"everyNthFrame\":1}", width, height);

	dwb_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		close(ws_fd);
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		g_free(self->chrome_ws_url);
		free(self);
		return NULL;
	}
	backend->ops = &dwb_backend_chromium;
	backend->impl = self;
	return backend;
}

/* Handles one decoded CDP message. Only screencast frames carry data this
 * backend needs; command replies are matched by the caller. */
static void cr_handle_message(cr_impl *self, const char *json)
{
	/* Screencast frames arrive as base64 in Page.screencastFrame. Extracting
	 * that without a JSON parser means finding the marker and decoding until
	 * the closing quote, which is safe because base64 has no escapes. */
	if (strstr(json, "Page.loadEventFired"))
		self->loaded = TRUE;

	const char *marker = "\"data\":\"";
	const char *at = strstr(json, marker);
	if (at) {
		at += strlen(marker);
		const char *end = strchr(at, '"');
		if (end && end > at) {
			gsize out_len = 0;
			guchar *decoded = g_base64_decode(at, &out_len);
			if (decoded && out_len > 0) {
				g_free(self->frame);
				self->frame = decoded;
				self->frame_size = out_len;
				self->seq++;
			} else {
				g_free(decoded);
			}
		}
	}
}

static void cr_destroy(dwb_backend *backend)
{
	cr_impl *self = backend->impl;
	if (!self)
		return;
	if (self->ws_fd >= 0)
		close(self->ws_fd);
	if (self->pid > 0) {
		kill(self->pid, SIGTERM);
		/* Give it a moment, then insist. */
		for (int i = 0; i < 20; i++) {
			int status;
			pid_t r = waitpid(self->pid, &status, WNOHANG);
			if (r == self->pid || r < 0)
				self->pid = 0;
			else
				g_usleep(100000);
			if (!self->pid)
				break;
		}
		if (self->pid)
			kill(self->pid, SIGKILL);
	}
	g_free(self->frame);
	g_free(self->chrome_ws_url);
	g_free(self->pending);
	g_free(self);
	free(backend);
}

static int cr_navigate(dwb_backend *backend, const char *url, char **error,
                      dwb_response *out)
{
	/* A response is not captured on this backend. It is left empty rather than
	 * refused: the guest treats an absent response as unknown, and this backend
	 * refuses message handlers outright, so a delegate that needs the response
	 * has nothing to do here anyway. Filling it in with a guess would be worse
	 * than reporting nothing. */
	if (out) {
		out->url = NULL;
		out->mime = NULL;
		out->status = 0;
	}
	cr_impl *self = backend->impl;
	self->loaded = FALSE;
	int nav_id = self->next_id++;
	cr_send_cmd(self, nav_id, "Page.navigate", "{\"url\":\"%s\"}", url);

	/* Page.navigate answers with errorText when the navigation itself fails
	 * (net::ERR_NAME_NOT_RESOLVED and friends). Chromium still paints an error
	 * page and still fires loadEventFired, so without reading this reply an
	 * unreachable host looks exactly like a successful load - the same defect
	 * the webkitgtk backend had, where WEBKIT_LOAD_FINISHED covers both cases. */
	char *nav_reply = cr_wait_reply(self, nav_id, 15000);
	if (nav_reply) {
		const char *err = strstr(nav_reply, "\"errorText\"");
		if (err) {
			const char *open = strchr(err, ':');
			char detail[256] = "navigation failed";
			if (open) {
				const char *v = strchr(open + 1, '"');
				const char *close = v ? strchr(v + 1, '"') : NULL;
				if (v && close && (size_t)(close - v - 1) < sizeof(detail)) {
					memcpy(detail, v + 1, (size_t)(close - v - 1));
					detail[close - v - 1] = '\0';
				}
			}
			set_error(error, "%s: %s", url, detail);
			free(nav_reply);
			return -1;
		}
		free(nav_reply);
	}

	gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	while (!self->loaded && g_get_monotonic_time() < deadline) {
		ws_pump(self, cr_pump_handler, NULL);
		g_usleep(50000);
	}
	if (!self->loaded) {
		set_error(error, "timed out waiting for %s to finish loading", url);
		return -1;
	}
	/* Confirm the page actually produces pixels before reporting success, so a
	 * blank or unsupported target is caught here rather than by the guest. */
	return cr_capture(self, error);
}

static void cr_resize(dwb_backend *backend, int width, int height)
{
	cr_impl *self = backend->impl;
	self->width = width;
	self->height = height;
	cr_send_cmd(self, self->next_id++, "Emulation.setDeviceMetricsOverride",
	            "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":false}",
	            width, height);
	ws_pump(self, cr_pump_handler, NULL);
}

/* Requests one frame via Page.captureScreenshot and waits for it.
 *
 * This is the reliable capture path. Page.startScreencast turned out to be
 * unusable here: the launcher stub injects a conflicting --ozone-platform, and
 * the cast produced no frames at all even once the handshake and navigation
 * were verified working. captureScreenshot is a request/reply, so it either
 * returns an image or an error - it cannot silently do nothing, which is
 * exactly the property a proxy needs. */
static int cr_capture(cr_impl *self, char **error)
{
	uint32_t before = self->seq;
	int id = self->next_id++;
	cr_send_cmd(self, id, "Page.captureScreenshot",
	            "{\"format\":\"jpeg\",\"quality\":85,\"captureBeyondViewport\":false}");

	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	while (self->seq == before && g_get_monotonic_time() < deadline) {
		struct pollfd pfd = { .fd = self->ws_fd, .events = POLLIN };
		if (poll(&pfd, 1, 250) <= 0)
			continue;
		char *reply = cr_wait_reply(self, id, 250);
		if (!reply)
			continue;
		if (strstr(reply, "\"error\"")) {
			set_error(error, "Page.captureScreenshot failed: %s", reply);
			free(reply);
			return -1;
		}
		free(reply);
	}

	if (self->seq == before || !self->frame || self->frame_size == 0) {
		set_error(error, "no frame from Page.captureScreenshot");
		return -1;
	}
	return 0;
}

static int cr_render(dwb_backend *backend, dwb_frame *out, char **error)
{
	cr_impl *self = backend->impl;
	ws_pump(self, cr_pump_handler, NULL);

	/* Prefer a screencast frame if one happens to be buffered, since it is
	 * cheaper; otherwise pull a fresh screenshot. */
	if (!self->frame || self->frame_size == 0) {
		if (cr_capture(self, error) != 0)
			return -1;
	}

	/* Compressed: hand it up as-is and let the frame header say so. Decoding
	 * belongs to whoever asked for pixels, not here. */
	out->pixels = self->frame;
	out->width = (uint32_t)self->width;
	out->height = (uint32_t)self->height;
	out->stride = 0;
	out->format = DWB_PIXEL_JPEG;
	out->size = (uint32_t)self->frame_size;
	return 0;
}

static int cr_evaluate(dwb_backend *backend, const char *js, char **value, char **error)
{
	cr_impl *self = backend->impl;
	int id = self->next_id++;
	/* The expression is escaped before it goes into the CDP command. It was
	 * interpolated raw, so any script containing a quote - that is, every
	 * script containing JSON, and every string literal with an apostrophe in it
	 * - produced malformed JSON, and the browser rejected the command:
	 *
	 *     no reply to Runtime.evaluate
	 *
	 * for a script that was never sent. The error names the transport, so it
	 * reads like a hung browser rather than a quoting problem.
	 *
	 * The buffer is raised to 256K for the same reason as the client's: an
	 * escaped expression is several times the length of the original. */
	{
		size_t worst = 0;
		for (const unsigned char *p = (const unsigned char *)js; *p; p++)
			worst += (*p == '"' || *p == '\\' || *p < 0x20) ? 6 : 1;
		if (worst + 128 >= 262144) {
			set_error(error, "expression too large to send");
			return -1;
		}
		char *params = g_malloc0(worst + 128);
		char *w = params;
		w += sprintf(w, "{\"expression\":\"");
		for (const unsigned char *p = (const unsigned char *)js; *p; p++) {
			switch (*p) {
			case '"':  memcpy(w, "\\\"", 2); w += 2; break;
			case '\\': memcpy(w, "\\\\", 2); w += 2; break;
			case '\n': memcpy(w, "\\n", 2); w += 2; break;
			case '\r': memcpy(w, "\\r", 2); w += 2; break;
			case '\t': memcpy(w, "\\t", 2); w += 2; break;
			case '\b': memcpy(w, "\\b", 2); w += 2; break;
			case '\f': memcpy(w, "\\f", 2); w += 2; break;
			default:
				if (*p < 0x20)
					w += sprintf(w, "\\u%04x", *p);
				else
					*w++ = (char)*p;
				break;
			}
		}
		w += sprintf(w, "\",\"returnByValue\":true}");
		cr_send_cmd_raw(self, id, "Runtime.evaluate", params);
		g_free(params);
	}
	char *reply = cr_wait_reply(self, id, 15000);
	if (!reply) {
		set_error(error, "no reply to Runtime.evaluate");
		return -1;
	}
	/* Extract the JS value, not a fragment of the CDP reply. The webkitgtk
	 * backend returns a clean string for the same call, and a guest decoding
	 * the protocol cannot special-case one backend's shape - it would have to
	 * parse nested CDP JSON on one path and not the other.
	 * Runtime.evaluate with returnByValue answers one of:
	 *   {"id":N,"result":{"result":{"type":"string","value":"..."}}}
	 *   {"id":N,"result":{"result":{"type":"number","value":2,"description":"2"}}}
	 * so a string value is extracted when there is one, and a bare number is
	 * read from the unquoted value otherwise.
	 *
	 * Returning "" for a numeric result - which is what happened once the
	 * extraction stopped guessing - made every numeric evaluation look like it
	 * produced nothing, and the app reads numbers back. So the number is read
	 * properly rather than treated as a missing value. */
	/* A thrown script answers with an exceptionDetails object and no value:
	 *
	 *   {"id":N,"result":{"result":{...},"exceptionDetails":{...}}}
	 *
	 * Reporting that as a successful empty string is a lie - a call that threw
	 * looked like a call that returned nothing - so the exception is checked
	 * first and its description becomes the error. */
	if (strstr(reply, "\"exceptionDetails\"")) {
		char *desc = json_string_value(reply, "\"description\"");
		if (!desc)
			desc = json_string_value(reply, "\"value\"");
		set_error(error, "%s", desc ? desc : "javascript threw");
		free(desc);
		free(reply);
		return -1;
	}

	char *js_value = json_string_value(reply, "\"value\"");
	if (!js_value) {
		char *vnum = json_scalar_value(reply, "\"value\"");
		js_value = vnum;
	}
	if (value)
		*value = js_value ? js_value : g_strdup("");
	else
		g_free(js_value);
	free(reply);
	return 0;
}

static void cr_wait(dwb_backend *backend, int ms)
{
	/* The page lives in the browser process, which has its own loop, so here
	 * waiting really is just waiting. */
	(void)backend;
	g_usleep((guint64)ms * 1000);
}

static int cr_add_script(dwb_backend *backend, const char *js, int at_document_start,
                         int main_only, char **error)
{
	cr_impl *self = backend->impl;
	/* Page.addScriptToEvaluateOnNewDocument is the document-start injection.
	 * There is no equivalent of a document-end injection over CDP, so that case
	 * is reported rather than silently doing the wrong thing. */
	if (!at_document_start) {
		set_error(error, "chromium backend only supports document-start injection");
		return -1;
	}
	(void)main_only;
	cr_send_cmd(self, self->next_id++, "Page.addScriptToEvaluateOnNewDocument",
	            "{\"source\":\"%s\"}", js);
	return 0;
}

const dwb_backend_ops dwb_backend_chromium = {
	.name = "chromium",
	.description = "Chromium via DevTools Protocol (media-compatibility fallback)",
	.probe = cr_probe,
	.create = cr_create,
	.destroy = cr_destroy,
	.navigate = cr_navigate,
	.resize = cr_resize,
	.render = cr_render,
	.evaluate = cr_evaluate,
	.wait = cr_wait,
	.add_script = cr_add_script,
};
