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

	/* Returns 0 on success, non-zero on failure with *error set (caller frees). */
	int (*navigate)(dwb_backend *backend, const char *url, char **error);
	void (*resize)(dwb_backend *backend, int width, int height);

	/* Renders the current state into `out`. The frame stays valid until the
	 * next call on this backend. */
	int (*render)(dwb_backend *backend, dwb_frame *out, char **error);

	/* Runs JS. On success *value receives a NUL-terminated heap string the
	 * caller frees. On failure *error is set. Either may be NULL if the caller
	 * does not want it. */
	int (*evaluate)(dwb_backend *backend, const char *js, char **value, char **error);
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
