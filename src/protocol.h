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

/* Extensions live in a reserved range, never appended to the enum above.
 *
 * The enum above is upstream's, verbatim, because the guest will eventually be
 * compiled against upstream's protocol.h rather than this copy. Appending here
 * renumbers what follows: ADD_SCRIPT became 10, which is upstream's PING. A
 * guest built against upstream's header would then send a document-start script
 * where a ping was expected, and nothing would complain - no compile error, no
 * failed test, just a host that silently misreads the stream.
 *
 * DWB_MSG_PING must stay 10. The static assert below is what keeps that true.
 */
#define DWB_MSG_EXT_BASE 0x1000
enum dwb_msg_ext_type {
	DWB_MSG_ADD_SCRIPT     = DWB_MSG_EXT_BASE + 0, /* {"js":"...","at":"document-start","main":true} */
	DWB_MSG_ADD_HANDLER    = DWB_MSG_EXT_BASE + 1, /* {"name":"...","key":"..."} */
	DWB_MSG_POLL_MESSAGE   = DWB_MSG_EXT_BASE + 2, /* drain a handler's queue */
	DWB_MSG_SCRIPT_MESSAGE = DWB_MSG_EXT_BASE + 3, /* reply to POLL: {"name","body"} or {"empty":true} */
};

/* The invariant that matters: a guest compiled against upstream's header must
 * agree with this host on every message upstream defines. */
typedef char dwb_proto_ping_must_be_10[(DWB_MSG_PING == 10) ? 1 : -1];
typedef char dwb_proto_ext_must_not_collide[(DWB_MSG_ADD_SCRIPT > 0x0FFF) ? 1 : -1];

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
	/* DWB_FRAME_IN_SHM when the pixels were written into the shared region the
	 * guest attached with DWB_MSG_SHM_ATTACH instead of being sent inline. The
	 * guest reads them from the region at the offset this header describes and
	 * expects no pixel bytes on the socket. A flag bit in the message type
	 * would have been shorter, but it would overload the type field the guest
	 * already switches on. */
	uint32_t reserved;
} dwb_frame_header;

/* The guest reads width, height, stride, format and size out of this struct by
 * offset. If the field order ever changed, both ends would still compile and
 * the guest would blit from the wrong offsets - healthy-looking garbage. */
typedef char dwb_frame_header_is_32_bytes[(sizeof(dwb_frame_header) == 32) ? 1 : -1];

#define DWB_FRAME_IN_SHM 1u

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
