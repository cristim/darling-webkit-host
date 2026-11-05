/* Host-side test for the chromium backend's script-message handlers. Includes
 * the backend source so its static parsing and registration paths run against
 * real-shaped CDP traffic, with a socketpair standing in for the DevTools
 * websocket. No browser is needed. */
#include "src/backends/chromium.c"

static int failures;

static void check(const char *label, int ok, const char *detail)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", label,
	       (!ok && detail) ? ": got " : "", (!ok && detail) ? detail : "");
	if (!ok)
		failures++;
}

/* Feeds a Runtime.bindingCalled event whose payload is the given raw page string,
 * JSON-escaped the way Chromium does on the wire. */
static void feed_binding(cr_impl *self, const char *name, const char *raw_payload)
{
	GString *esc = g_string_new(NULL);
	for (const char *p = raw_payload; *p; p++) {
		switch (*p) {
		case '"':  g_string_append(esc, "\\\""); break;
		case '\\': g_string_append(esc, "\\\\"); break;
		case '\n': g_string_append(esc, "\\n"); break;
		case '\t': g_string_append(esc, "\\t"); break;
		default:   g_string_append_c(esc, *p); break;
		}
	}
	char *msg = g_strdup_printf(
	    "{\"method\":\"Runtime.bindingCalled\",\"params\":{\"name\":\"%s\","
	    "\"payload\":\"%s\",\"executionContextId\":1}}", name, esc->str);
	cr_handle_message(self, msg);
	g_free(msg);
	g_string_free(esc, TRUE);
}

static void expect_body(cr_impl *self, const char *label, const char *name, const char *want)
{
	cr_handler *hc = NULL;
	for (GSList *it = self->handlers; it; it = it->next)
		if (strcmp(((cr_handler *)it->data)->name, name) == 0)
			hc = it->data;
	char *got = hc ? g_queue_pop_head(hc->bodies) : NULL;
	check(label, got && strcmp(got, want) == 0, got ? got : "(empty)");
	g_free(got);
}

static cr_handler *add_queue(cr_impl *self, const char *name)
{
	cr_handler *hc = g_new0(cr_handler, 1);
	hc->name = g_strdup(name);
	hc->bodies = g_queue_new();
	self->handlers = g_slist_append(self->handlers, hc);
	return hc;
}

/* Writes one unmasked server-to-client text frame, as Chromium sends them. */
static void server_send(int fd, const char *msg)
{
	size_t len = strlen(msg);
	unsigned char head[4] = { 0x81, 0, 0, 0 };
	size_t hl = 2;
	if (len < 126) {
		head[1] = (unsigned char)len;
	} else {
		head[1] = 126;
		head[2] = (unsigned char)(len >> 8);
		head[3] = (unsigned char)len;
		hl = 4;
	}
	if (write(fd, head, hl) != (ssize_t)hl || write(fd, msg, len) != (ssize_t)len)
		abort();
}

/* Runs cr_add_handler against a canned CDP reply for command id 100. */
static int add_with_reply(cr_impl *self, const char *name, const char *reply, char **err)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		abort();
	self->ws_fd = sv[0];
	self->next_id = 100;
	if (reply)
		server_send(sv[1], reply);
	dwb_backend backend = { .impl = self };
	int rc = cr_add_handler(&backend, name, err);
	close(sv[0]);
	close(sv[1]);
	self->ws_fd = -1;
	return rc;
}

static int has_handler(cr_impl *self, const char *name)
{
	for (GSList *it = self->handlers; it; it = it->next)
		if (strcmp(((cr_handler *)it->data)->name, name) == 0)
			return 1;
	return 0;
}

int main(void)
{
	cr_impl self;
	memset(&self, 0, sizeof(self));
	self.ws_fd = -1;
	add_queue(&self, "ylevent");

	feed_binding(&self, "ylevent", "{\"ev\":\"video\",\"state\":\"playing\"}");
	expect_body(&self, "body with quotes arrives intact", "ylevent",
	            "{\"ev\":\"video\",\"state\":\"playing\"}");
	feed_binding(&self, "ylevent", "line1\nline2\ttabbed");
	expect_body(&self, "newline and tab are decoded", "ylevent", "line1\nline2\ttabbed");
	feed_binding(&self, "ylevent", "back\\slash");
	expect_body(&self, "backslash is decoded", "ylevent", "back\\slash");

	cr_handle_message(&self,
	    "{\"method\":\"Runtime.bindingCalled\",\"params\":{\"name\":\"ylevent\","
	    "\"payload\":\"a\\/b \\u00e9 \\ud83d\\ude00 \\b\\f\\r\",\"executionContextId\":1}}");
	expect_body(&self, "slash, BMP and surrogate-pair escapes are decoded", "ylevent",
	            "a/b \xc3\xa9 \xf0\x9f\x98\x80 \b\f\r");

	char *err = NULL;
	int rc = add_with_reply(&self, "dw\"bProbe", "{\"id\":100,\"result\":{}}", &err);
	check("quoted channel name is rejected", rc != 0 && !has_handler(&self, "dw\"bProbe"),
	      has_handler(&self, "dw\"bProbe") ? "registered" : "returned success");
	check("rejection carries a reason", err && *err, err ? err : "(no error)");
	g_free(err);
	err = NULL;
	rc = add_with_reply(&self, "x\",\"k\":\"v", "{\"id\":100,\"result\":{}}", &err);
	check("param-injecting channel name is rejected", rc != 0, "returned success");
	g_free(err);
	err = NULL;

	rc = add_with_reply(&self, "okName", "{\"id\":100,\"result\":{}}", &err);
	check("successful addBinding registers the channel", rc == 0 && has_handler(&self, "okName"),
	      err ? err : "not registered");
	g_free(err);
	err = NULL;

	rc = add_with_reply(&self, "refused",
	    "{\"id\":100,\"error\":{\"code\":-32602,\"message\":\"Invalid parameters\"}}", &err);
	check("failed addBinding reply returns failure", rc != 0 && !has_handler(&self, "refused"),
	      rc == 0 ? "returned success" : "registered anyway");
	check("failure names the CDP message", err && strstr(err, "Invalid parameters"),
	      err ? err : "(no error)");
	g_free(err);
	err = NULL;

	rc = add_with_reply(&self, "silent", NULL, &err);
	check("missing addBinding reply returns failure", rc != 0 && !has_handler(&self, "silent"),
	      rc == 0 ? "returned success" : "registered anyway");
	g_free(err);

	return failures ? 1 : 0;
}
