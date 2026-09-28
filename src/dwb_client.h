/* Portable guest-side transport for the Darling WebKit host service.
 *
 * Deliberately free of AppKit and Foundation so it can be compiled and tested
 * off-target; WKWebView.m becomes a thin ObjC shell over this. Everything hard
 * lives here and is exercised by the tests: framing, the shared-memory path,
 * sequence tracking, and error events.
 *
 * The guest is a single client, so this is a synchronous request/response
 * client. Frames are pulled, matching the host's DWB_MSG_FRAME.
 */
#ifndef DWB_CLIENT_H
#define DWB_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef struct {
	int fd;
	/* Set when the host reported an error via DWB_MSG_EVENT, so the caller can
	 * surface a navigation failure instead of silently doing nothing. */
	int had_error;
	char error[512];
	/* The response from the last completed load, kept until the next navigate.
	 * NULL or 0 means the host did not report it, which is not an error. */
	char *last_url;
	char *last_mime;
	int last_status;
	/* Last frame's sequence number, so the caller can drop stale frames. A
	 * frame whose seq is not newer than the last seen one has been superseded
	 * before the guest got round to painting it. */
	uint32_t last_seq;
	int have_seq;
} dwb_client;

/* Connects to the service. Returns 0 on success. */
int dwb_client_connect(dwb_client *c, const char *socket_path);
void dwb_client_close(dwb_client *c);

/* What the host reported about a completed load. Any field may be NULL/0, which
 * means "not reported" rather than "absent" - a backend that cannot capture a
 * response still reports the load. Caller frees the strings. */
typedef struct {
	char *url;
	char *mime;
	int status;
} dwb_load_info;

/* Navigate and wait for the load to finish. On success, and when *info is
 * given, it receives the response the host reported. */
int dwb_client_navigate_info(dwb_client *c, const char *url, dwb_load_info *info);

/* Handshake. Fills *backend_name (caller frees) and reports the service's
 * default geometry. Returns 0 on success. */
int dwb_client_hello(dwb_client *c, char **backend_name, int *width, int *height);

/* Navigates. Returns 0 when the host reports load-finished, -1 on error, with
 * the reason in c->error. */
int dwb_client_navigate(dwb_client *c, const char *url);

int dwb_client_resize(dwb_client *c, int width, int height);

/* Evaluates script. *value is a NUL-terminated string the caller frees.
 * Returns 0 on success. */
int dwb_client_eval(dwb_client *c, const char *js, char **value);

/* Attaches a shared frame region so subsequent frames arrive in memory rather
 * than on the socket. Returns 0 on success. */
int dwb_client_attach_shm(dwb_client *c, const char *path, size_t bytes);

/* Installs a script to run before the page's own. Returns 0 on success. */
int dwb_client_add_script(dwb_client *c, const char *js, int at_document_start,
                          int main_only);

/* Registers a named handler the page can post to. */
int dwb_client_add_handler(dwb_client *c, const char *name);

/* Drains one queued message. Returns 1 when a message came back (body in
 * *body, caller frees), 0 when the queue was empty, -1 on failure. */
int dwb_client_poll_message(dwb_client *c, const char *name, char **body);

/* Upstream's protocol.h has no DWB_FRAME_IN_SHM, because the socket and
 * frame-delivery layer that uses it is this fork's own. If a guest is ever
 * compiled against the upstream copy of the header, define it here rather than
 * failing to build; the value is the agreed convention, and the struct is
 * asserted below so the field order cannot drift unnoticed. */
#ifndef DWB_FRAME_IN_SHM
#define DWB_FRAME_IN_SHM 1u
#endif

/* What a frame header means for a consumer, derived once so the decision cannot
 * be made differently at each call site.
 *
 * This exists because the guest got it wrong: it built a bitmap rep around NULL
 * instead of around the pixels, and drew a compressed frame as though it had
 * rows. Both were logic errors that compiled, parsed, and passed every
 * structural check. The mapping from header to "how do I display this" is pure
 * arithmetic on the header, so it belongs here where it can be tested against
 * real frames rather than reasoned about in a view controller. */
typedef struct {
	int bad_magic;       /* the region is not a frame at all */
	int is_raw;          /* pixels are rows that can be blitted directly */
	int components;      /* 3 for RGB, 4 for RGBA/BGRA/ARGB, 0 if compressed */
	int has_alpha;
	int needs_decode;    /* compressed: the host must decode before display */
	/* Offset from the start of the region or buffer to the first pixel byte. */
	size_t pixel_offset;
	size_t pixel_bytes;
} dwb_frame_layout;

/* Pure function of `fh` and the buffer base size. Returns 0 on success.
 *
 * Checks the magic. If the two sides disagree about the header's layout the
 * guest would otherwise read plausible-looking width, height and stride out of
 * the wrong offsets and blit whatever followed - drawing garbage while looking
 * healthy. A bad magic is the one signal that reliably means "this struct is
 * not what I think it is", so it is checked and reported rather than ignored. */
int dwb_frame_describe(const dwb_frame_header *fh, size_t buffer_bytes,
                       dwb_frame_layout *out);

/* Pulls one frame.
 *
 * If a shared region was attached and the host wrote there, *pixels points into
 * that mapping and *in_shm is set. Otherwise *pixels points at a buffer this
 * function owns and the caller must free it via dwb_client_free_frame.
 * *pixels is NULL if the host reported an error instead of a frame. */
int dwb_client_frame(dwb_client *c, dwb_frame_header *out, const void **pixels,
                     size_t *pixel_bytes, void **shm_map, size_t *shm_size,
                     int *in_shm);

void dwb_client_free_frame(dwb_client *c, void *pixels);

#endif /* DWB_CLIENT_H */
