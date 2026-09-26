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

	gboolean load_finished;

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
	if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED)
		self->load_finished = TRUE;
}

static int wk_probe(void)
{
	/* This backend is only compiled when pkg-config found webkit2gtk-4.1, so
	 * reaching this function already proves the library exists. What can still
	 * be missing at runtime is a display: WebKitGTK will not composite into a
	 * surface that is not attached to a GdkWindow, so a headless host needs
	 * Xvfb. */
	return gtk_init_check(NULL, NULL) ? 1 : 0;
}

static dwb_backend *wk_create(int width, int height, char **error)
{
	if (!gtk_init_check(NULL, NULL)) {
		set_error(error, "gtk_init_check failed (no display; try Xvfb)");
		return NULL;
	}

	wk_impl *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(self->window), width, height);
	self->view = WEBKIT_WEB_VIEW(webkit_web_view_new());

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
	gtk_container_add(GTK_CONTAINER(self->window), GTK_WIDGET(self->view));
	gtk_widget_show_all(self->window);

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
	if (self->window) {
		gtk_widget_destroy(self->window);
		/* Leave the main loop clean for a later backend's create(). */
		while (g_main_context_pending(NULL))
			g_main_context_iteration(NULL, FALSE);
	}
	g_free(self);
	free(backend);
}

static int wk_navigate(dwb_backend *backend, const char *url, char **error)
{
	wk_impl *self = backend->impl;
	self->load_finished = FALSE;
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
	/* Let deferred layout and first paint settle so frame 0 is not blank. */
	pump(300);
	return 0;
}

static void wk_resize(dwb_backend *backend, int width, int height)
{
	wk_impl *self = backend->impl;
	gtk_window_resize(GTK_WINDOW(self->window), width, height);
	pump(150);
}

typedef struct {
	char *value;
	char *error;
	gboolean done;
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
		char *str = value ? jsc_value_to_string(value) : NULL;
		ctx->value = g_strdup(str ? str : "");
		g_free(str);
		g_clear_object(&value);
	}
	ctx->done = TRUE;
}

static int wk_evaluate(dwb_backend *backend, const char *js, char **value, char **error)
{
	wk_impl *self = backend->impl;
	eval_ctx ctx = { .done = FALSE };

	webkit_web_view_evaluate_javascript(self->view, js, -1, NULL, NULL, NULL,
	                                    on_eval_finished, &ctx);

	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
	while (!ctx.done && g_get_monotonic_time() < deadline)
		pump(50);

	if (!ctx.done) {
		set_error(error, "javascript evaluation timed out");
		return -1;
	}
	if (ctx.error) {
		set_error(error, "%s", ctx.error);
		g_free(ctx.error);
		g_free(ctx.value);
		return -1;
	}
	if (value)
		*value = ctx.value;
	else
		g_free(ctx.value);
	return 0;
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
};
