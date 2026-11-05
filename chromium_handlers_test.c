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

	return failures ? 1 : 0;
}
