/* Backend interface: one engine behind a uniform surface.
 *
 * The guest's WebKit.framework only ever talks to whichever backend the host
 * service selected, so adding an engine means adding one file that fills in
 * dwb_backend_ops and appending it to the table in registry.c. Nothing in the
 * protocol or the guest changes.
 */
#ifndef DWB_BACKEND_H
#define DWB_BACKEND_H

#include <stdint.h>

typedef struct dwb_backend dwb_backend;

/* A decoded frame the host wants to hand to the guest. Pixels are owned by the
 * backend and stay valid until the next call on that backend. */
/* The main resource's response, filled in by navigate. Every field is optional:
 * a backend that cannot report one leaves them NULL/0, and the guest treats that
 * as unknown rather than as a failed load. Separate from dwb_frame because a
 * response is about the load and a frame is about the pixels. */
typedef struct {
	char *url;      /* the final URL, after redirects; caller frees */
	char *mime;     /* Content-Type, or NULL */
	int status;     /* HTTP status, or 0 if not reported */
} dwb_response;

typedef struct {
	void *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride; /* bytes per row */
	uint32_t format; /* DWB_PIXEL_* from protocol.h */
	uint32_t size;   /* byte length of `pixels` */
} dwb_frame;

typedef struct {
	const char *name;
	/* Short human description, used by `--list-backends`. */
	const char *description;

	/* Runtime detection. Must not crash or be slow; this runs once at startup
	 * for every registered backend, including ones that are not installed. */
	int (*probe)(void);

	dwb_backend *(*create)(int width, int height, char **error);
	void (*destroy)(dwb_backend *backend);

	/* The main resource's response, filled in by navigate. Every field is

	/* Returns 0 on success, non-zero on failure with *error set (caller frees).
	 * `out`, when given, receives the response for the load just performed. */
	int (*navigate)(dwb_backend *backend, const char *url, char **error,
	                dwb_response *out);
	void (*resize)(dwb_backend *backend, int width, int height);

	/* Renders the current state into `out`. The frame stays valid until the
	 * next call on this backend. */
	int (*render)(dwb_backend *backend, dwb_frame *out, char **error);

	/* Runs JS. On success *value receives a NUL-terminated heap string the
	 * caller frees. On failure *error is set. Either may be NULL if the caller
	 * does not want it. */
	int (*evaluate)(dwb_backend *backend, const char *js, char **value, char **error);

	/* Gives the page `ms` of wall time. A backend whose engine is main-loop
	 * bound has to iterate that loop here rather than sleep: WebKitGTK cannot
	 * open a GStreamer pipeline, answer IPC or run a script while its loop is
	 * not turning, so a plain sleep hands the page no time at all. A backend
	 * whose page lives in another process can just sleep. */
	void (*wait)(dwb_backend *backend, int ms);

	/* Installs a script to run before the page's own scripts. Needed by any
	 * client that uses a WKUserContentController, which is how a webview is
	 * normally instrumented. `main_only` mirrors
	 * WKUserScript's forMainFrameOnly. */
	int (*add_script)(dwb_backend *backend, const char *js, int at_document_start,
	                  int main_only, char **error);

	/* Named message handler, the other half of what a
	 * WKUserContentController client installs. The page calls into the host
	 * through it; the host queues what arrives and the guest drains the queue by
	 * polling. Polling rather than pushing because the protocol is strict
	 * request/response, and an unsolicited host->guest message would land where
	 * the guest expects a reply and desynchronise the stream. */
	int (*add_handler)(dwb_backend *backend, const char *name, char **error);
	int (*poll_message)(dwb_backend *backend, const char *name, char **body,
	                    char **error);
} dwb_backend_ops;

struct dwb_backend {
	const dwb_backend_ops *ops;
	void *impl;
};

/* Backends provided by the build. Each is compiled in only when its
 * dependencies were found, so the table reflects what is actually usable. */
extern const dwb_backend_ops dwb_backend_webkitgtk;
extern const dwb_backend_ops dwb_backend_chromium;

/* NULL-terminated table of every backend compiled into this build. */
const dwb_backend_ops *const *dwb_backend_table(void);

/* Walks the table in priority order and returns the first backend whose probe()
 * succeeds. `forced` overrides selection when non-NULL (DWB_BACKEND env var or
 * --backend). Returns NULL and sets *error if nothing is available or the forced
 * name is unknown. */
const dwb_backend_ops *dwb_backend_select(const char *forced, char **error);

#endif /* DWB_BACKEND_H */
