/* WebKitGTK 4.1 backend.
 *
 * Same engine as macOS WebKit (WebCore + JSC), so this is the fidelity backend:
 * WKWebView semantics, DOM, JS. Media goes through GStreamer, so H.264/AAC
 * depends on the host having the relevant gst plugins.
 *
 * Written against WebKitGTK 2.52, whose API lives under <webkit2/webkit2.h> and
 * pulls in <webkit/WebKitXxx.h>. Note WebKitLoadEvent has no FAILED member: a
 * failed load still reports WEBKIT_LOAD_FINISHED, so success is judged from the
 * snapshot rather than from the load event.
 */
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include "../backend.h"
#include "../protocol.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	GtkWidget *window;
	WebKitWebView *view;

	GdkPixbuf *snapshot; /* owned; valid until the next render() */
	uint32_t seq;
	/* Geometry the caller asked for, so render() can detect a WM that ignored
	 * the request instead of silently shipping an oversized frame. */
	int want_width;
	int want_height;

	gboolean load_finished;
	/* The main resource's response, captured at commit and empty until then. */
	char *response_mime;
	char *response_url;
	int response_status;
	/* Set when the main resource itself fails. WEBKIT_LOAD_FINISHED is reported
	 * for a failed load as well as a successful one - the enum has no FAILED
	 * member - so without this a DNS failure is indistinguishable from a load and
	 * the guest is told navigation succeeded. */
	gboolean load_failed;
	gboolean load_committed;
	/* Registered message handlers. The channel name is the signal *detail*
	 * rather than a parameter (script-message-received has n_params=1: just the
	 * WebKitJavascriptResult), so the name travels in this context. */
	GSList *handlers;
	/* Bodies queued by named handlers, drained by the guest polling. */
	char **queued;
	int queued_len;
	int queued_cap;
	char load_failure[256];

} wk_impl;

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

/* Runs the GTK main loop for `ms` milliseconds. WebKitGTK is main-loop bound, so
 * every wait in this backend goes through here rather than sleeping.
 *
 * iteration() is called non-blocking on purpose. With the blocking form it sits
 * until an event arrives, so a caller that is waiting on a deadline never gets
 * to look at that deadline - a page that loads nothing hangs the wait, and in
 * the guest that would hang the navigation call itself. */
static void pump(int ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < deadline) {
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(1000); /* nothing pending; let the loop breathe */
	}
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
	(void)view;
	wk_impl *self = user_data;
	if (event == WEBKIT_LOAD_COMMITTED)
		self->load_committed = TRUE;
	if (event == WEBKIT_LOAD_COMMITTED) {
		/* The main resource's response, captured here because
		 * WEBKIT_LOAD_COMMITTED is the last event that still carries it.
		 *
		 * Via the main resource: this WebKitGTK has no
		 * webkit_web_view_get_uri_response, which is what I tried first and it
		 * does not compile. get_main_resource is not deprecated, and
		 * webkit_web_resource_get_response hands back the URI response with
		 * get_uri, get_mime_type and get_status_code. */
		g_free(self->response_url);
		g_free(self->response_mime);
		self->response_url = NULL;
		self->response_mime = NULL;
		self->response_status = 0;
		WebKitWebResource *resource = webkit_web_view_get_main_resource(view);
		WebKitURIResponse *response = resource
			? webkit_web_resource_get_response(resource) : NULL;
		if (response) {
			self->response_mime = g_strdup(webkit_uri_response_get_mime_type(response));
			self->response_url = g_strdup(webkit_uri_response_get_uri(response));
			self->response_status = (int)webkit_uri_response_get_status_code(response);
		}
		self->load_committed = TRUE;
	}
	if (event == WEBKIT_LOAD_FINISHED) {
		/* FINISHED without a preceding COMMITTED means the navigation never
		 * reached the origin - DNS failure, refused connection, bad scheme. The
		 * enum has no FAILED member, so the absence of the commit is the signal
		 * that the load did not actually happen. */
		if (!self->load_committed)
			self->load_failed = TRUE;
		self->load_finished = TRUE;
	}
}

/* Pins the two GDK settings that are only read during initialisation. */
static void pin_gdk_env(void)
{
	static gsize once = 0;
	if (!g_once_init_enter(&once))
		return;

	/* The host display is HiDPI, so a 640x480 logical view became a 1280x960
	 * device-pixel surface - correct behaviour for GTK, but double the frame
	 * bytes a guest has to receive for no benefit. */
	g_setenv("GDK_SCALE", "1", TRUE);
	g_setenv("GDK_DPI_SCALE", "1", TRUE);

	/* A desktop session exports GDK_BACKEND=wayland,x11,* to mean "whatever you
	 * prefer", and GTK then picks Wayland and puts this service's window on the
	 * logged-in user's desktop, ignoring the DISPLAY it was handed. Measured with
	 * DISPLAY=:77 and the session's value: the toplevel's GdkWindow was not an
	 * X11 window at all (GDK_IS_X11_WINDOW false, so gdk_x11_window_get_xid
	 * asserted), render() returned twice the requested size, and every sampled
	 * frame was byte-identical during playback. With GDK_BACKEND=x11 the same
	 * code returns exactly 640x480 and 29 distinct frames over 8s of playback.
	 *
	 * A preference list or a wildcard is a session's choice, not an instruction
	 * to this service, so it is replaced. A caller who deliberately wants one
	 * backend keeps it. */
	const char *backend = g_getenv("GDK_BACKEND");
	gboolean explicit_backend = backend && *backend && !strchr(backend, ',') &&
	                           !strchr(backend, '*');
	if (!explicit_backend)
		g_setenv("GDK_BACKEND", "x11", TRUE);

	g_once_init_leave(&once, 1);
}

static int wk_probe(void)
{
	pin_gdk_env();
	/* This backend is only compiled when pkg-config found webkit2gtk-4.1, so
	 * reaching this function already proves the library exists. What can still
	 * be missing at runtime is a display: WebKitGTK will not composite into a
	 * surface that is not attached to a GdkWindow, so a headless host needs
	 * Xvfb. */
	return gtk_init_check(NULL, NULL) ? 1 : 0;
}

static dwb_backend *wk_create(int width, int height, char **error)
{
	pin_gdk_env();
	if (!gtk_init_check(NULL, NULL)) {
		set_error(error, "gtk_init_check failed (no display; try Xvfb)");
		return NULL;
	}

	wk_impl *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(self->window), width, height);
	/* The window must not be growable. set_default_size is only a request, and
	 * the WM was free to hand back a 1242x1500 view for a 640x480 request,
	 * which made every frame 7-15MB. A non-resizable window plus a hard size
	 * request on the view makes the requested geometry the actual allocation,
	 * which is what the guest will be told to display. */
	/* Resizable, because a non-resizable window ignores gtk_window_resize and a
	 * guest resize is then silently dropped. The view's size request is what
	 * pins the geometry, and wk_resize lowers it on demand. */
	gtk_window_set_resizable(GTK_WINDOW(self->window), TRUE);
	self->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
	self->want_width = width;
	self->want_height = height;
	gtk_widget_set_size_request(GTK_WIDGET(self->view), width, height);

	WebKitSettings *settings = webkit_web_view_get_settings(self->view);
	if (settings) {
		/* A proxy never sees a user gesture, so without this YouTube refuses
		 * to start playback and nothing in the guest could ever play a video. */
		webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
		webkit_settings_set_hardware_acceleration_policy(
			settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
		webkit_settings_set_enable_webgl(settings, TRUE);
	}

	g_signal_connect(self->view, "load-changed", G_CALLBACK(on_load_changed), self);
	/* script-message-received is connected per channel in wk_add_handler, with
	 * the channel as the signal detail and a handler context as user_data. A
	 * blanket connection here would pass wk_impl where a handler context is
	 * expected. */
	gtk_container_add(GTK_CONTAINER(self->window), GTK_WIDGET(self->view));
	gtk_widget_show_all(self->window);
	/* Re-assert after mapping: the first allocation can still be pre-resize. */
	gtk_window_resize(GTK_WINDOW(self->window), width, height);
	gtk_window_move(GTK_WINDOW(self->window), 0, 0);
	pump(120);

	dwb_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		g_free(self);
		return NULL;
	}
	backend->ops = &dwb_backend_webkitgtk;
	backend->impl = self;
	return backend;
}

static void wk_destroy(dwb_backend *backend)
{
	wk_impl *self = backend->impl;
	if (!self)
		return;
	if (self->snapshot)
		g_object_unref(self->snapshot);
	for (int i = 0; i < self->queued_len; i++)
		g_free(self->queued[i]);
	g_free(self->queued);
	if (self->window) {
		gtk_widget_destroy(self->window);
		/* Leave the main loop clean for a later backend's create(). */
		while (g_main_context_pending(NULL))
			g_main_context_iteration(NULL, FALSE);
	}
	g_free(self);
	free(backend);
}

static int wk_navigate(dwb_backend *backend, const char *url, char **error,
                      dwb_response *out)
{
	wk_impl *self = backend->impl;
	self->load_finished = FALSE;
	self->load_failed = FALSE;
	self->load_committed = FALSE;
	self->load_failure[0] = '\0';
	if (g_getenv("DWB_DEBUG_WK"))
		g_printerr("[wk] load_uri: %s\n", url);
	webkit_web_view_load_uri(self->view, url);
	if (g_getenv("DWB_DEBUG_WK"))
		g_printerr("[wk] load_uri returned\n");

	/* Bounded: a page that never reports a load event must not wedge the
	 * guest's navigation call. */
	gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	while (!self->load_finished && g_get_monotonic_time() < deadline)
		pump(50);
	if (g_getenv("DWB_DEBUG_WK"))
		g_printerr("[wk] wait loop done, load_finished=%d\n", self->load_finished);

	if (!self->load_finished) {
		set_error(error, "timed out waiting for %s to load", url);
		return -1;
	}
	if (self->load_failed) {
		set_error(error, "%s: %s", url,
		          self->load_failure[0] ? self->load_failure
		                               : "navigation did not commit (host unreachable?)");
		return -1;
	}
	/* Hand the response back. Copied out of the impl so the caller owns it and
	 * the impl can be reused for the next navigation without invalidating
	 * anything already handed out. */
	if (out) {
		out->url = self->response_url ? g_strdup(self->response_url) : NULL;
		out->mime = self->response_mime ? g_strdup(self->response_mime) : NULL;
		out->status = self->response_status;
	}
	/* Let deferred layout and first paint settle so frame 0 is not blank. */
	pump(300);
	return 0;
}

typedef struct {
	wk_impl *impl;
	char *name;
	GQueue *bodies; /* char*, drained by the guest polling */
} handler_ctx;

static void handler_ctx_free(gpointer data)
{
	handler_ctx *hc = data;
	if (!hc)
		return;
	g_free(hc->name);
	if (hc->bodies)
		g_queue_free_full(hc->bodies, g_free);
	g_free(hc);
}

/* Three arguments, not four. The channel name is the signal detail, so a
 * four-argument callback receives user_data where it expects the name and then
 * dereferences whatever follows it as user_data - which segfaulted on the first
 * message received. */
static void on_script_message(WebKitUserContentManager *manager,
                              WebKitJavascriptResult *js_result, gpointer user_data)
{
	(void)manager;
	handler_ctx *hc = user_data;
	if (!hc || !hc->bodies)
		return;
	JSCValue *value = webkit_javascript_result_get_js_value(js_result);
	/* Borrowed reference: the result object owns it, so unreffing here trips
	 * "g_object_unref: assertion 'G_IS_OBJECT (object)' failed". */
	char *body = value ? jsc_value_to_string(value) : NULL;
	if (body)
		g_queue_push_tail(hc->bodies, body);
}

static int wk_add_handler(dwb_backend *backend, const char *name, char **error)
{
	wk_impl *self = backend->impl;
	WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(self->view);
	if (!manager) {
		set_error(error, "view has no user content manager");
		return -1;
	}
	/* Registering a channel twice succeeds, and is what a real
	 * WKUserContentController does - addScriptMessageHandler: twice for one name
	 * replaces the handler, it does not fail. WebKit refuses the second
	 * registration outright, and reporting that as an error meant a guest that
	 * legitimately re-applied its configuration - which the app does whenever the
	 * configuration is applied again - saw:
	 *
	 *     could not register message handler 'ylevent'
	 *
	 * for a handler that was already there and working. The existing context is
	 * reused, so the queue is not thrown away and messages already queued are
	 * still deliverable.
	 */
	for (GSList *it = self->handlers; it; it = it->next) {
		handler_ctx *existing = it->data;
		if (existing->name && strcmp(existing->name, name) == 0)
			return 0;
	}

	gchar *owned = g_strdup(name);
	gboolean ok = webkit_user_content_manager_register_script_message_handler(manager, owned);
	g_free(owned);
	if (!ok) {
		set_error(error, "could not register message handler '%s'", name);
		return -1;
	}
	handler_ctx *hc = g_new0(handler_ctx, 1);
	hc->impl = self;
	hc->name = g_strdup(name);
	hc->bodies = g_queue_new();
	self->handlers = g_slist_append(self->handlers, hc);

	/* Connect with the channel as the detail, so the callback's user_data
	 * carries the name the signal does not. */
	char *detail = g_strdup_printf("script-message-received::%s", name);
	g_signal_connect(manager, detail, G_CALLBACK(on_script_message), hc);
	g_free(detail);
	return 0;
}

static int wk_poll_message(dwb_backend *backend, const char *name, char **body,
                           char **error)
{
	wk_impl *self = backend->impl;
	for (GSList *it = self->handlers; it; it = it->next) {
		handler_ctx *hc = it->data;
		if (strcmp(hc->name, name) != 0)
			continue;
		g_free(*body);
		*body = g_queue_pop_head(hc->bodies); /* NULL when empty */
		return 0;
	}
	set_error(error, "no such message handler '%s'", name);
	*body = NULL;
	return -1;
}

static int wk_add_script(dwb_backend *backend, const char *js, int at_document_start,
                         int main_only, char **error)
{
	wk_impl *self = backend->impl;
	WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(self->view);
	if (!manager) {
		set_error(error, "view has no user content manager");
		return -1;
	}
	/* A WKUserScript's forMainFrameOnly maps to the injected-frames enum, and
	 * its injection time maps to the inject-at enum. */
	WebKitUserContentInjectedFrames frames = main_only
		? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
		: WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES;
	WebKitUserScriptInjectionTime when = at_document_start
		? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
		: WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
	WebKitUserScript *script = webkit_user_script_new(js, frames, when, NULL, NULL);
	if (!script) {
		set_error(error, "could not build user script");
		return -1;
	}
	webkit_user_content_manager_add_script(manager, script);
	webkit_user_script_unref(script);
	return 0;
}

static void wk_resize(dwb_backend *backend, int width, int height)
{
	wk_impl *self = backend->impl;
	/* The view carries a size request set at create(). Without lowering it
	 * here the window clamps to that minimum and the guest's resize is silently
	 * ignored - the frame keeps coming back at the original size. */
	gtk_widget_set_size_request(GTK_WIDGET(self->view), width, height);
	gtk_window_resize(GTK_WINDOW(self->window), width, height);
	self->want_width = width;
	self->want_height = height;
	/* The reallocation happens on the next layout pass, so pump long enough
	 * for it to land before the caller samples a frame. */
	pump(300);
}

typedef struct {
	char *value;
	char *error;
	gboolean done;
	/* Set by the waiter when it gives up. The callback may still fire after
	 * that, so it must be able to tell that nobody is listening any more. */
	gboolean abandoned;
} eval_ctx;

static void on_eval_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
	eval_ctx *ctx = user_data;
	GError *err = NULL;
	JSCValue *value =
		webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &err);
	if (err) {
		ctx->error = g_strdup(err->message);
		g_error_free(err);
	} else {
		/* A script that returns nothing is not an error. evaluateJavaScript
		 * is called all over the place for its effect - postMessage, a DOM
		 * assignment, a click - and postMessage in particular returns undefined
		 * by design. jsc_value_to_string refuses a non-primitive, and treating
		 * that refusal as a failure reported:
		 *
		 *     post error: Unsupported result type
		 *
		 * for a call that did exactly what it was asked. An object result is
		 * stringified; undefined means no value, which is reported as an empty
		 * string, matching what a real WKWebView hands back. */
		if (!value) {
			ctx->value = g_strdup("");
		} else if (jsc_value_is_undefined(value) || jsc_value_is_null(value)) {
			ctx->value = g_strdup("");
		} else if (jsc_value_is_string(value)) {
			char *str = jsc_value_to_string(value);
			ctx->value = g_strdup(str ? str : "");
			g_free(str);
		} else if (jsc_value_is_number(value) || jsc_value_is_boolean(value)) {
			/* A number or boolean is not a string, and jsc_value_to_string
			 * refuses it. Stringifying it here rather than routing it to the
			 * structured branch below is what makes "1+1" come back as "2":
			 * JSON-encoding a number yields "2", but JSON-encoding a *string*
			 * yields "\"hello\"", and a string is by far the most common
			 * result, so checking it first is what keeps the common case
			 * correct. */
			char *str = jsc_value_to_string(value);
			ctx->value = g_strdup(str ? str : "");
			g_free(str);
		} else {
			/* Object, function, symbol: the structured form is the faithful
			 * rendering and keeps the caller informed, where stringifying
			 * would hand back "[object Object]". jsc_value_to_json is the API
			 * for this; the older JSValueToStringCopy path is deprecated. */
			/* indent 0 is a compact rendering; indent 2 would be pretty
			 * printed, which is not what a caller stringifying a value wants. */
			char *json = jsc_value_to_json(value, 0);
			ctx->value = g_strdup(json ? json : "");
			g_free(json);
		}
		g_clear_object(&value);
	}
	ctx->done = TRUE;
	/* If the waiter already returned, the result is dropped and the context is
	 * freed here. The context cannot be freed on abandonment instead: this
	 * callback is what still writes into it. */
	if (ctx->abandoned) {
		g_free(ctx->value);
		g_free(ctx->error);
		g_free(ctx);
	}
}

static int wk_evaluate(dwb_backend *backend, const char *js, char **value, char **error)
{
	wk_impl *self = backend->impl;
	/* Heap, not stack. The callback is owned by WebKit and fires whenever it
	 * fires; on a page that takes longer than the deadline (a media page
	 * blocked in GStreamer will) the stack frame below is long gone, and the
	 * callback writing a gboolean and two pointers into it corrupted whatever
	 * the caller had by then. */
	eval_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		set_error(error, "out of memory");
		return -1;
	}

	/* Wrapped as a function body, not an expression, because a caller's script
	 * is frequently statements rather than an expression - "var x=5;" is a
	 * statement, and parenthesising it produced:
	 *
	 *     SyntaxError: Unexpected keyword 'var'
	 *
	 * which is a regression this wrapper introduced, not a pre-existing fault.
	 * A body takes both: statements run, and the last expression is the value.
	 * __r stays declared on the wrapped function's own scope, so nothing leaks
	 * into the page.
	 *
	 * The completion value is always a string WebKit can marshal back.
	 *
	 * evaluate_javascript_finish fails the whole call with "Unsupported result
	 * type" when the script's result cannot be marshalled, even though the script
	 * ran and had its effect. YouLearn's own first message triggers it:
	 *
	 *     window.webkit.messageHandlers.ylevent.postMessage('{"ev":"video"}')
	 *
	 * The handler is invoked, the event is queued, and the guest drains a
	 * complete 32-byte body - and the call is then reported as a failure.
	 *
	 * So the script is wrapped in an IIFE that renders its result as a string.
	 * The catch rethrows rather than returning a string. Returning
	 * "error: ..." turned a script that threw into a *successful* evaluation
	 * whose value happened to be that text, so the caller's own exception never
	 * reached it:
	 *
	 *     rc=0 val=[error: Error: he said "hi"] err=[]
	 *
	 * The try is only there so the capture logic itself is never skipped.
	 *
	 * The rendering matches what the unwrapped path already produced for
	 * primitives and objects, and undefined or null becomes the empty string
	 * rather than an error, which is what a real WKWebView hands back. Verified
	 * against the values that worked before, so nothing regressed to buy this:
	 * 1+1 -> 2, 'hello' -> hello, ({a:1}) -> {"a":1}, typeof x -> object,
	 * undefined and null -> empty. */
	/* Does the last non-blank line of the script look like a bare expression?
	 * Keywords that introduce a statement, and anything ending a statement, say
	 * no. Deliberately conservative: a false negative costs the caller its
	 * return value, a false positive produces a syntax error. */
	const char *last = NULL;
	{
		const char *scan = js;
		while (*scan) {
			const char *nl = strchr(scan, '\n');
			const char *end = nl ? nl : scan + strlen(scan);
			const char *b = scan;
			while (b < end && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n'))
				b++;
			if (b < end)
				last = b;
			if (!nl)
				break;
			scan = nl + 1;
		}
	}
	size_t last_len = last ? strlen(last) : 0;
	int looks_like_expression = 0;
	if (last && last_len) {
		/* A keyword only counts when it is a whole word. Plain prefix matching
		 * made "document.title" look like the "do" statement, and
		 * "function.length" like a function declaration, so both were treated
		 * as statements, never captured, and came back empty:
		 *
		 *     try 0: rc=0 val=[] err=[-]
		 *
		 * for a title that is present and non-empty. The test is "the keyword,
		 * then a non-identifier character", which is what distinguishes a
		 * statement keyword from the start of an identifier. */
		static const char *keywords[] = {
			"var", "let", "const", "if", "for", "while", "do", "return",
			"function", "try", "throw", "switch", "debugger", "class",
			"delete", "import", "export", "with", NULL
		};
		int is_statement = 0;
		for (int k = 0; keywords[k] && !is_statement; k++) {
			size_t kwlen = strlen(keywords[k]);
			if (strncmp(last, keywords[k], kwlen) != 0)
				continue;
			char after = last[kwlen];
			int is_word = (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
			              (after >= '0' && after <= '9') || after == '_' || after == '$';
			/* A keyword followed by whitespace introduces a statement. A
			 * keyword followed by '.' or '(' does not: `function.length` is a
			 * property read on the global `function` object, not a declaration,
			 * and treating it as a declaration made the wrapper splice it in as a
			 * function body:
			 *
			 *     SyntaxError: Unexpected token '.'
			 *
			 * The word-boundary test alone was not enough, because '.' is not an
			 * identifier character and so looked like a keyword boundary. */
			if (!is_word && after != '.' && after != '(')
				is_statement = 1;
		}
		/* window.print is a call, not a statement; listed so it is not
		 * mistaken for one, and checked exactly because of the same reason. */
		if (!is_statement && strcmp(last, "window.print") == 0)
			is_statement = 1;
		if (!is_statement) {
			char tail = last[last_len - 1];
			if (tail != ';' && tail != '}')
				looks_like_expression = 1;
		}
	}

	/* The caller's script is spliced in unchanged, so it needs a terminator of
	 * its own before anything follows it. A statement that already ends in ; or
	 * is fine as-is; a bare `throw new Error(...)` is not, and appending the
	 * capture logic straight after it produced:
	 *
	 *     ... __r; throw new Error('x') if (__r === undefined ...
	 *     SyntaxError: Unexpected keyword 'if'
	 *
	 * So a semicolon is added when the last line does not already end in one.
	 * Harmless for a block and for an expression alike. */
	int needs_semicolon = 0;
	if (last && last_len) {
		char tail = last[last_len - 1];
		needs_semicolon = (tail != ';' && tail != '}');
	}

	char *body = NULL;
	if (looks_like_expression) {
		/* Copy everything before the last line, then assign the last line. */
		size_t head_len = (size_t)(last - js);
		const char *assign = "__r = ";
		body = g_malloc0(head_len + last_len + strlen(assign) + 8);
		memcpy(body, js, head_len);
		body[head_len] = '\0';
		strcat(body, assign);
		strcat(body, last);
		strcat(body, ";\n");
	} else {
		size_t total = strlen(js) + 4;
		body = g_malloc0(total);
		strcpy(body, js);
		if (needs_semicolon)
			strcat(body, ";\n");
		else
			strcat(body, "\n");
	}

	char *wrapped = g_strdup_printf(
		"(function(){ try { var __r; %s"
		" if (__r === undefined || __r === null) return '';"
		" if (typeof __r === 'object') {"
		"   var __j = JSON.stringify(__r); return __j === undefined ? '' : __j; }"
		" return String(__r); } catch (e) { throw e; } })()",
		body);
	g_free(body);
	webkit_web_view_evaluate_javascript(self->view, wrapped, -1, NULL, NULL, NULL,
	                                    on_eval_finished, ctx);
	g_free(wrapped);

	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	while (!ctx->done && g_get_monotonic_time() < deadline)
		pump(50);

	if (!ctx->done) {
		set_error(error, "javascript evaluation timed out");
		ctx->abandoned = TRUE;
		return -1;
	}
	if (ctx->error) {
		set_error(error, "%s", ctx->error);
		g_free(ctx->error);
		g_free(ctx->value);
		g_free(ctx);
		return -1;
	}
	if (value)
		*value = ctx->value;
	else
		g_free(ctx->value);
	g_free(ctx);
	return 0;
}

static void wk_wait(dwb_backend *backend, int ms)
{
	(void)backend;
	pump(ms);
}

static int wk_render(dwb_backend *backend, dwb_frame *out, char **error)
{
	wk_impl *self = backend->impl;

	/* Frames are read straight off the view's GdkWindow rather than through
	 * webkit_web_view_get_snapshot(). Two reasons, both observed:
	 *
	 *  - The snapshot API never calls back on a page containing a <video>,
	 *    so the request sat pending until it timed out. Reading the window has
	 *    no such dependency.
	 *  - The snapshot ignored the requested window size and returned the page's
	 *    full content size (2512x1500, 15MB) even at --size 640x480, which OOM'd
	 *    a 15GB host. The window is by definition the size we asked for, which
	 *    is also what the guest will display.
	 *
	 * It is on-screen pixels either way, so this is not a lower-fidelity
	 * source - it is the same surface, minus the API that would not settle. */
	GdkWindow *gdk_window = gtk_widget_get_window(GTK_WIDGET(self->view));
	if (!gdk_window) {
		set_error(error, "view has no GdkWindow (not realised?)");
		return -1;
	}
	GtkAllocation alloc;
	gtk_widget_get_allocation(GTK_WIDGET(self->view), &alloc);
	if (alloc.width <= 0 || alloc.height <= 0) {
		set_error(error, "view has a zero-sized allocation (%dx%d)", alloc.width,
		          alloc.height);
		return -1;
	}

	/* Let pending paints and video presents land before sampling, otherwise the
	 * window still holds the previous frame. Without this a playing video
	 * reported identical pixels across every sample. */
	pump(40);

	GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(
		gdk_window, 0, 0, alloc.width, alloc.height);
	if (!pixbuf) {
		set_error(error, "gdk_pixbuf_get_from_window failed for %dx%d", alloc.width,
		          alloc.height);
		return -1;
	}

	int channels = gdk_pixbuf_get_n_channels(pixbuf);
	int width = gdk_pixbuf_get_width(pixbuf);
	int height = gdk_pixbuf_get_height(pixbuf);
	int stride = gdk_pixbuf_get_rowstride(pixbuf);

	if (self->snapshot)
		g_object_unref(self->snapshot);
	self->snapshot = GDK_PIXBUF(pixbuf);

	out->pixels = gdk_pixbuf_get_pixels(self->snapshot);
	out->width = (uint32_t)width;
	out->height = (uint32_t)height;
	out->stride = (uint32_t)stride;
	/* GdkPixbuf stores bytes in R,G,B,A order on this host, and the alpha
	 * channel is only present when the window has one. */
	out->format = channels == 4 ? DWB_PIXEL_RGBA : DWB_PIXEL_RGB;
	out->size = (uint32_t)(stride * height);
	self->seq++;
	return 0;
}

const dwb_backend_ops dwb_backend_webkitgtk = {
	.name = "webkitgtk",
	.description = "WebKitGTK 4.1 (WebCore + JSC, GStreamer media)",
	.probe = wk_probe,
	.create = wk_create,
	.destroy = wk_destroy,
	.navigate = wk_navigate,
	.resize = wk_resize,
	.render = wk_render,
	.evaluate = wk_evaluate,
	.wait = wk_wait,
	.add_script = wk_add_script,
	.add_handler = wk_add_handler,
	.poll_message = wk_poll_message,
};
