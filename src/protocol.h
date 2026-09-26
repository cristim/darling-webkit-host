/* Wire protocol between the Darling guest and the host-side WebKit service.
 *
 * Design note: the control plane is a stream of length-prefixed messages; the
 * frame plane is a shared-memory ring so video does not have to cross a socket.
 * Both are defined here so the guest-side WebKit.framework and the host service
 * cannot drift.
 */
#ifndef DWB_PROTOCOL_H
#define DWB_PROTOCOL_H

#include <stdint.h>

#define DWB_PROTO_VERSION 1u
#define DWB_MAGIC 0x4B425744u /* "DWBK" */

/* Control message header. Wire order is host-endian; both ends are the same
 * machine, so there is no byte-swap layer. */
typedef struct {
	uint32_t magic;   /* DWB_MAGIC */
	uint16_t version; /* DWB_PROTO_VERSION */
	uint16_t type;
	uint32_t length; /* payload bytes following this header */
} dwb_header;

enum dwb_msg_type {
	DWB_MSG_HELLO = 1,     /* guest -> host: {"proto":1,"backend":"?"} */
	DWB_MSG_HELLO_ACK,     /* host -> guest: {"backend":"webkitgtk","caps":N} */
	DWB_MSG_NAVIGATE,      /* guest -> host: {"url":"..."} */
	DWB_MSG_RESIZE,        /* guest -> host: {"w":N,"h":N} */
	DWB_MSG_FRAME,         /* host -> guest: shm index of a new frame */
	DWB_MSG_EVAL,          /* guest -> host: {"js":"..."} */
	DWB_MSG_EVAL_RESULT,   /* host -> guest: {"value":"...","error":"?"} */
	DWB_MSG_EVENT,         /* host -> guest: {"name":"load-changed","state":"finished"} */
	DWB_MSG_SHM_ATTACH,    /* guest -> host: {"path":"/tmp/...","size":N} */
	DWB_MSG_PING,
	DWB_MSG_PONG,
	DWB_MSG_BYE,
};

/* Header of a frame inside the shared-memory region. `size` is the byte length
 * of pixel data that follows this header, at `stride` bytes per row. */
#define DWB_FRAME_MAGIC 0x4B465744u /* "DWFK" */

typedef struct {
	uint32_t magic; /* DWB_FRAME_MAGIC */
	uint32_t seq;   /* monotonically increasing; lets the guest drop stale frames */
	uint32_t width;
	uint32_t height;
	uint32_t stride;  /* bytes per row, >= width * 4 */
	uint32_t format;  /* DWB_PIXEL_* */
	uint32_t size;    /* bytes of pixel data after this header */
	uint32_t reserved;
} dwb_frame_header;

enum dwb_pixel_format {
	DWB_PIXEL_BGRA = 0, /* most common little-endian layout for cairo/GdkPixbuf */
	DWB_PIXEL_RGBA = 1,
	DWB_PIXEL_ARGB = 2,
	DWB_PIXEL_RGB = 4,  /* no alpha channel, 3 bytes per pixel */
	/* Compressed. The host sends JPEG as-is and the guest decodes; used by the
	 * Chromium screencast backend, which cannot cheaply hand over raw pixels. */
	DWB_PIXEL_JPEG = 3,
};

#endif /* DWB_PROTOCOL_H */
