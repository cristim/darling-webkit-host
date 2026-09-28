/* Host side of the guest protocol: serve one WebKit view over a Unix socket.
 *
 * This is the half that was deliberately left unimplemented in the service
 * while the guest-side spikes were unverified. Those spikes are now partly
 * answered by measurement rather than assumption: the engines navigate,
 * evaluate script and produce correctly-sized frames, so the transport is
 * worth building. Frame payload is framed over the socket rather than shared
 * memory for this first implementation - shared memory is a throughput
 * optimisation and can be layered in without changing the message set, whereas
 * getting the contract wrong now would be expensive.
 *
 * The frame plane is intentionally kept in the message set (DWB_MSG_FRAME with
 * a dwb_frame_header preamble) so the guest's decode path does not change when
 * the transport does.
 */
#include "backend.h"
#include "protocol.h"

#include <stddef.h>

typedef struct {
	const dwb_backend_ops *ops;
	dwb_backend *backend;
	int width;
	int height;
	/* Shared frame region the guest attached, or NULL when frames are still
	 * sent inline. Inline costs ~3MB per RGB frame, which is ~90MB/s at 30fps
	 * - fine for a still page, not for video. */
	void *shm;
	size_t shm_size;
} serve_ctx;


#include <glib.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Wire helpers. One header, then payload bytes. Both ends are the same machine,
 * so there is no byte-swap layer; the field order is protocol.h's.
 *
 * Sockets handed over by GSocketService are non-blocking, so a plain read()
 * returning EAGAIN is a normal "not ready", not a failure. Treating it as fatal
 * dropped the guest immediately after the handshake. These helpers therefore
 * wait on poll() and pump the GLib loop while waiting, so a slow guest never
 * stalls the web engine.
 */
static int wait_ready(int fd, int for_write)
{
	struct pollfd pfd = { .fd = fd, .events = (short)(for_write ? POLLOUT : POLLIN) };
	for (;;) {
		int r = poll(&pfd, 1, 50);
		if (r > 0)
			return 0;
		if (r < 0 && errno != EINTR)
			return -1;
		/* Keep the engine turning while the guest is quiet. */
		while (g_main_context_pending(NULL))
			g_main_context_iteration(NULL, FALSE);
	}
}

static int send_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (wait_ready(fd, 1) != 0)
					return -1;
				continue;
			}
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (wait_ready(fd, 0) != 0)
					return -1;
				continue;
			}
			return -1;
		}
		if (n == 0)
			return -1; /* peer closed mid-message */
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int send_msg(int fd, uint16_t type, const void *payload, uint32_t len)
{
	dwb_header h = { .magic = DWB_MAGIC, .version = DWB_PROTO_VERSION,
	                 .type = type, .length = len };
	if (send_all(fd, &h, sizeof(h)) != 0)
		return -1;
	if (len && send_all(fd, payload, len) != 0)
		return -1;
	return 0;
}

/* Pulls a minimal JSON string value out of a flat object, e.g. "url":"...".
 * Adequate for the guest's own requests and keeps a JSON parser out of the
 * service; anything with escapes is rejected rather than mis-parsed. */
/* Escapes for embedding in a JSON string literal. The mirror of the client's
 * escape and of the json_get_string decode below: a string that crosses the wire
 * has to survive the trip in both directions, and interpolating it raw truncated
 * it at the first quote inside it.
 *
 * This matters for the app specifically. YouLearn's injected script posts
 * window.webkit.messageHandlers.ylevent.postMessage with a stringified JSON body,
 * so every message it ever sends contains quotes and newlines. Pasting those in
 * raw produced malformed JSON on the way out and a truncated document on the way
 * in, and the error the host reported was a JavaScript syntax error naming the
 * page URL - which reads like a bug in the page and is nothing of the sort. */
static char *json_escape(const char *in)
{
	if (in == NULL)
		return NULL;
	size_t worst = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++)
		worst += (*p == '"' || *p == '\\' || *p < 0x20) ? 6 : 1;
	worst++;
	char *out = malloc(worst);
	if (out == NULL)
		return NULL;
	char *w = out;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
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
	*w = '\0';
	return out;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	const char *at = strstr(json, pat);
	if (!at)
		return -1;
	at += strlen(pat);
	size_t w = 0;
	while (*at != '"') {
		if (*at == '\0')
			return -1;          /* unterminated value */
		char c = *at;
		if (c != '\\') {
			at++;
		} else {
			/* Skip the backslash, then consume exactly one escape. Stepping over
			 * the escaped character is the whole job: the first version left the
			 * cursor sitting on it, so the next loop test saw the quote inside a
			 * \" sequence and stopped there. That truncated every script
			 * containing JSON at its first escaped quote. */
			at++;
			switch (*at) {
			case 'n':  c = '\n'; at++; break;
			case 'r':  c = '\r'; at++; break;
			case 't':  c = '\t'; at++; break;
			case 'b':  c = '\b'; at++; break;
			case 'f':  c = '\f'; at++; break;
			case 'u': {
				/* The BMP subset this protocol can carry, as UTF-8. A malformed
				 * or out-of-range \u is refused rather than guessed at, so a bad
				 * script is reported instead of becoming a different script. */
				if (at[1] && at[2] && at[3] && at[4]) {
					char hex[5] = { at[1], at[2], at[3], at[4], 0 };
					char *endp = NULL;
					long v = strtol(hex, &endp, 16);
					if (endp && *endp == 0 && v > 0 && v < 0x80) {
						if (w + 1 >= out_sz) return -1;
						out[w++] = (char)v; at += 5; break;
					}
					if (endp && *endp == 0 && v >= 0x80 && v < 0x800) {
						if (w + 2 >= out_sz) return -1;
						out[w++] = (char)(0xC0 | (v >> 6));
						out[w++] = (char)(0x80 | (v & 0x3F)); at += 5; break;
					}
					if (endp && *endp == 0 && v >= 0x800 && v < 0x10000) {
						if (w + 3 >= out_sz) return -1;
						out[w++] = (char)(0xE0 | (v >> 12));
						out[w++] = (char)(0x80 | ((v >> 6) & 0x3F));
						out[w++] = (char)(0x80 | (v & 0x3F)); at += 5; break;
					}
				}
				return -1;
			}
			default:
				c = *at;
				at++;
				break;
			}
		}
		if (w + 1 >= out_sz)
			return -1;
		out[w++] = c;
	}
	out[w] = '\0';
	return 0;
}

/* Builds {"name":"<event>","detail":"<detail>"} with the detail escaped. The
 * detail strings come from the backends, and the most useful ones quote the very
 * thing that went wrong - a handler name, a URL, a script fragment - so they
 * routinely contain quotes themselves. Pasting one in raw made the reply
 * unparseable, and the client then reported a generic failure in place of the
 * reason the host had just explained. */
static char *event_error(const char *name, const char *detail, size_t cap)
{
	char *out = calloc(1, cap);
	if (out == NULL)
		return NULL;
	char *ed = json_escape(detail ? detail : "unknown");
	snprintf(out, cap, "{\"name\":\"%s\",\"detail\":\"%s\"}",
	         name, ed ? ed : "unknown");
	free(ed);
	return out;
}

static int json_get_int(const char *json, const char *key, int *out)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *at = strstr(json, pat);
	if (!at)
		return -1;
	at += strlen(pat);
	while (*at == ' ')
		at++;
	if (*at < '0' || *at > '9')
		return -1;
	*out = (int)strtol(at, NULL, 10);
	return 0;
}

/* Handles one guest. Returns 0 when the guest disconnected cleanly and 1 on a
 * protocol violation, so the caller can drop that guest and keep serving. */
static int serve_client(int client, const dwb_backend_ops *ops, dwb_backend *backend,
                        int *width, int *height, void *shm_ctx)
{

	static uint32_t frames_sent;
	/* 256K: an injected document-start script containing JSON escapes to
	 * several times its own length, and 64K refused scripts that are
	 * perfectly ordinary. The client caps what it will send. */
	char payload[262144];
	for (;;) {
		dwb_header h;
		if (recv_all(client, &h, sizeof(h)) != 0) {
			fprintf(stderr, "guest closed\n");
			return 0;
		}
		if (h.magic != DWB_MAGIC) {
			fprintf(stderr, "bad magic %#x, dropping guest\n", h.magic);
			return 1;
		}
		if (h.version != DWB_PROTO_VERSION) {
			/* Refuse loudly rather than misparse: a version skew here would
			 * corrupt frames silently. */
			fprintf(stderr, "protocol version %u != %u, dropping guest\n",
			        h.version, DWB_PROTO_VERSION);
			return 1;
		}
		if (h.length > sizeof(payload)) {
			fprintf(stderr, "oversized payload %u, dropping guest\n", h.length);
			return 1;
		}
		/* `>=`, not `>`: the terminator below writes at index h.length, so a
		 * payload exactly filling this buffer wrote one byte past the end of a
		 * 256 KB stack allocation. */
		if (h.length >= sizeof(payload)) {
			fprintf(stderr, "oversized payload %u, dropping guest\n", h.length);
			return 1;
		}
		if (h.length && recv_all(client, payload, h.length) != 0)
			return 0;
		payload[h.length] = '\0';

		switch (h.type) {
		case DWB_MSG_HELLO: {
			char reply[256];
			snprintf(reply, sizeof(reply),
			         "{\"backend\":\"%s\",\"caps\":%d,\"width\":%d,\"height\":%d}",
			         ops->name, 1, *width, *height);
			send_msg(client, DWB_MSG_HELLO_ACK, reply, (uint32_t)strlen(reply));
			fprintf(stderr, "hello from guest, backend=%s\n", ops->name);
			break;
		}
		case DWB_MSG_NAVIGATE: {
			char url[2048];
			if (json_get_string(payload, "url", url, sizeof(url)) != 0) {
				send_msg(client, DWB_MSG_EVENT, "{\"name\":\"error\",\"detail\":\"bad url\"}", 38);
				break;
			}
			char *nav_err = NULL;
			dwb_response response = { NULL, NULL, 0 };
			int rc = ops->navigate(backend, url, &nav_err, &response);
			if (rc != 0) {
				char ev[4096];
				char *eu = json_escape(url);
				char *ed = json_escape(nav_err ? nav_err : "unknown");
				snprintf(ev, sizeof(ev), "{\"name\":\"error\",\"url\":\"%s\",\"detail\":\"%s\"}",
				         eu ? eu : "", ed ? ed : "unknown");
				free(eu);
				free(ed);
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
				fprintf(stderr, "navigate failed: %s\n", nav_err ? nav_err : "unknown");
				g_free(nav_err);
				break;
			}
			/* The response travels with the event. A guest's delegate is asked
			 * webView:decidePolicyForNavigationResponse: after the load, and that
			 * callback can only inspect what the host reports; without these
			 * fields the guest has nothing to show it and the callback cannot
			 * work at all.
			 *
			 * Optional by design: a backend that reports nothing - chromium -
			 * still sends load-finished, with empty fields, and the guest reads
			 * that as unknown rather than as a failure. */
			{
				char *ru = json_escape(response.url ? response.url : "");
				char *rm = json_escape(response.mime ? response.mime : "");
				char ev[4096];
				snprintf(ev, sizeof(ev),
				         "{\"name\":\"load-finished\",\"url\":\"%s\","
				         "\"mime\":\"%s\",\"status\":%d}",
				         ru ? ru : "", rm ? rm : "", response.status);
				free(ru);
				free(rm);
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
			}
			free(response.url);
			free(response.mime);
			break;
		}
		case DWB_MSG_RESIZE: {
			int w = *width, hh = *height;
			json_get_int(payload, "w", &w);
			json_get_int(payload, "h", &hh);
			if (w > 0 && hh > 0) {
				ops->resize(backend, w, hh);
				*width = w;
				*height = hh;
			}
			break;
		}
		case DWB_MSG_EVAL: {
			char js[32768];
			if (json_get_string(payload, "js", js, sizeof(js)) != 0) {
				send_msg(client, DWB_MSG_EVAL_RESULT, "{\"error\":\"bad js\"}", 18);
				break;
			}
			char *value = NULL, *eval_err = NULL;
			if (ops->evaluate(backend, js, &value, &eval_err) != 0) {
				char r[4096];
				char *eev = json_escape(eval_err ? eval_err : "unknown");
				snprintf(r, sizeof(r), "{\"error\":\"%s\"}", eev ? eev : "unknown");
				free(eev);
				send_msg(client, DWB_MSG_EVAL_RESULT, r, (uint32_t)strlen(r));
				g_free(eval_err);
			} else {
				char r[65536];
				char *ev = json_escape(value ? value : "");
				snprintf(r, sizeof(r), "{\"value\":\"%s\"}", ev ? ev : "");
				free(ev);
				send_msg(client, DWB_MSG_EVAL_RESULT, r, (uint32_t)strlen(r));
				g_free(value);
			}
			break;
		}
		case DWB_MSG_FRAME: {
			dwb_frame f;
			char *render_err = NULL;
			if (ops->render(backend, &f, &render_err) != 0) {
				char *ev = event_error("error", render_err ? render_err : "render failed", 4096);
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
				free(ev);
				g_free(render_err);
				break;
			}
			dwb_frame_header fh = { .magic = DWB_FRAME_MAGIC, .seq = ++frames_sent,
				                .width = f.width, .height = f.height,
				                .stride = f.stride, .format = f.format, .size = f.size };
			serve_ctx *ctx = shm_ctx;
			size_t need = sizeof(fh) + f.size;
			if (ctx && ctx->shm && ctx->shm_size >= need) {
				/* Shared region attached: copy once, tell the guest to read it
				 * from there, and send only the header on the socket. */
				memcpy(ctx->shm, &fh, sizeof(fh));
				if (f.size)
					memcpy((char *)ctx->shm + sizeof(fh), f.pixels, f.size);
				fh.reserved = DWB_FRAME_IN_SHM;
				send_msg(client, DWB_MSG_FRAME, &fh, (uint32_t)sizeof(fh));
			} else {
				send_msg(client, DWB_MSG_FRAME, &fh, (uint32_t)sizeof(fh));
				if (f.size)
					send_all(client, f.pixels, f.size);
			}
			break;
		}
		case DWB_MSG_SHM_ATTACH: {
			serve_ctx *ctx = shm_ctx;
			char path[512];
			size_t bytes = 0;
			if (json_get_string(payload, "path", path, sizeof(path)) != 0) {
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"bad shm path\"}", 40);
				break;
			}
			json_get_int(payload, "size", (int *)&bytes);
			if (bytes == 0)
				bytes = 32u * 1024 * 1024; /* room for 1080p RGBA */
			if (ctx->shm) {
				munmap(ctx->shm, ctx->shm_size);
				ctx->shm = NULL;
				ctx->shm_size = 0;
			}
			int sfd = open(path, O_RDWR);
			if (sfd < 0) {
				char ev[600];
				snprintf(ev, sizeof(ev),
				         "{\"name\":\"error\",\"detail\":\"shm open failed: %s\"}",
				         strerror(errno));
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
				break;
			}
			/* Map only what the file actually is. The guest names the size, and
			 * mmap happily maps past the end of the file: the mapping succeeds
			 * and the first access beyond it raises SIGBUS, which kills the host
			 * rather than the guest. A guest asking for more than the file holds
			 * is a bug in the guest, and the fix is to refuse it, not to crash.
			 *
			 * st_size can legitimately be smaller than what the guest asked for
			 * if the file was created shorter, so the smaller of the two is what
			 * both sides must agree on, and the guest is told that size. */
			{
				struct stat st;
				if (fstat(sfd, &st) != 0) {
					close(sfd);
					send_msg(client, DWB_MSG_EVENT,
					         "{\"name\":\"error\",\"detail\":\"shm fstat failed\"}", 45);
					break;
				}
				if ((size_t)st.st_size < bytes)
					bytes = (size_t)st.st_size;
				if (bytes == 0) {
					close(sfd);
					send_msg(client, DWB_MSG_EVENT,
					         "{\"name\":\"error\",\"detail\":\"shm file is empty\"}", 44);
					break;
				}
			}
			void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
			close(sfd);
			if (map == MAP_FAILED) {
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"shm mmap failed\"}", 42);
				break;
			}
			ctx->shm = map;
			ctx->shm_size = bytes;
			char ok[128];
			snprintf(ok, sizeof(ok), "{\"name\":\"shm-attached\",\"size\":%zu}", bytes);
			send_msg(client, DWB_MSG_EVENT, ok, (uint32_t)strlen(ok));
			fprintf(stderr, "shared frame region attached: %s (%zu bytes)\n", path, bytes);
			break;
		}
		case DWB_MSG_ADD_SCRIPT: {
			char js[32768];
			char when[32] = "document-start";
			if (json_get_string(payload, "js", js, sizeof(js)) != 0) {
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"bad js\"}", 34);
				break;
			}
			json_get_string(payload, "at", when, sizeof(when));
			int main_only = 0;
			json_get_int(payload, "main", &main_only);
			char *ierr = NULL;
			if (!ops->add_script || ops->add_script(backend, js,
			                                        strcmp(when, "document-start") == 0,
			                                        main_only, &ierr) != 0) {
				char *ev = event_error("error", ierr ? ierr : "add_script unsupported", 8192);
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
				free(ev);
				g_free(ierr);
				break;
			}
			send_msg(client, DWB_MSG_EVENT,
			         "{\"name\":\"script-added\"}", 24);
			break;
		}
		case DWB_MSG_ADD_HANDLER: {
			char name[256];
			if (json_get_string(payload, "name", name, sizeof(name)) != 0) {
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"bad handler name\"}", 40);
				break;
			}
			char *herr = NULL;
			if (!ops->add_handler || ops->add_handler(backend, name, &herr) != 0) {
				char *ev = event_error("error", herr ? herr : "add_handler unsupported", 4096);
				send_msg(client, DWB_MSG_EVENT, ev, (uint32_t)strlen(ev));
				free(ev);
				g_free(herr);
				break;
			}
			send_msg(client, DWB_MSG_EVENT, "{\"name\":\"handler-added\"}", 26);
			break;
		}
		case DWB_MSG_POLL_MESSAGE: {
			/* The guest drains the queue. An unsolicited host->guest message
			 * would arrive where the guest expects a reply and desynchronise
			 * the stream, so the callback path is pull-based by design. */
			char name[256];
			if (json_get_string(payload, "name", name, sizeof(name)) != 0) {
				/* The return value was ignored, so a request with no usable
				 * name polled an uninitialised stack buffer - an arbitrary
				 * channel name built from whatever was on the stack. Refuse
				 * instead. */
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"poll needs a channel name\"}", 53);
				break;
			}
			char *body = NULL, *perr = NULL;
			if (!ops->poll_message || ops->poll_message(backend, name, &body, &perr) != 0) {
				send_msg(client, DWB_MSG_EVENT,
				         "{\"name\":\"error\",\"detail\":\"poll unsupported\"}", 42);
				g_free(perr);
				break;
			}
			/* The body is normally a stringified object, so it is full of quotes
			 * and newlines, and pasting it in raw truncated the reply at the
			 * first quote inside it - the app then parsed half a JSON object.
			 * The name is a channel the page chose, so it gets the same. */
			char reply[65536];
			if (body) {
				char *en = json_escape(name);
				char *eb = json_escape(body);
				snprintf(reply, sizeof(reply), "{\"name\":\"%s\",\"body\":\"%s\"}",
				         en ? en : "", eb ? eb : "");
				free(en);
				free(eb);
			} else {
				snprintf(reply, sizeof(reply), "{\"empty\":true}");
			}
			send_msg(client, DWB_MSG_SCRIPT_MESSAGE, reply, (uint32_t)strlen(reply));
			g_free(body);
			break;
		}
		case DWB_MSG_PING:
			send_msg(client, DWB_MSG_PONG, NULL, 0);
			break;
		case DWB_MSG_BYE:
			fprintf(stderr, "guest said bye\n");
			return 0;
		default:
			fprintf(stderr, "unknown message type %u\n", h.type);
			break;
		}
	}
}

/* One guest at a time. The proxy serves a single WKWebView, so handling a
 * connection synchronously here is sufficient; the main loop still turns
 * between connections, which is what keeps the view responsive while idle. */
static void on_incoming(GSocketService *service, GSocketConnection *connection,
                        GObject *source, gpointer user_data)
{
	(void)service;
	(void)source;
	serve_ctx *ctx = user_data;
	/* The message layer works on the raw fd. */
	int cfd = g_socket_get_fd(g_socket_connection_get_socket(connection));
	if (cfd < 0)
		return;
	fprintf(stderr, "guest connected\n");
	fflush(stderr);
	serve_client(cfd, ctx->ops, ctx->backend, &ctx->width, &ctx->height, ctx);
	fprintf(stderr, "guest disconnected\n");
	fflush(stderr);
}

int dwb_serve(const char *socket_path, const char *backend_name, int width, int height)
{
	char *error = NULL;
	const dwb_backend_ops *ops = dwb_backend_select(backend_name, &error);
	if (!ops) {
		fprintf(stderr, "no backend: %s\n", error ? error : "unknown");
		g_free(error);
		return 2;
	}
	dwb_backend *backend = ops->create(width, height, &error);
	if (!backend) {
		fprintf(stderr, "create failed: %s\n", error ? error : "unknown");
		g_free(error);
		return 3;
	}
	unlink(socket_path);
	int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		ops->destroy(backend);
		return 4;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "socket path too long\n");
		close(lfd);
		ops->destroy(backend);
		return 4;
	}
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	/* Serve on the main loop rather than blocking in accept(). A blocking
	 * accept() stops the loop, the view's window stops responding to the window
	 * manager, and the process gets flagged as hung. GSocketService dispatches
	 * incoming connections from the loop, so the engine keeps pumping the whole
	 * time the service is idle. */
	GSocketService *service = g_socket_service_new();
	GInetAddress *dummy = NULL; (void)dummy;
	GError *gerr = NULL;
	GSocketAddress *gaddr = g_unix_socket_address_new(addr.sun_path);
	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), gaddr,
	                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
	                                   NULL, NULL, &gerr)) {
		g_object_unref(gaddr);
		fprintf(stderr, "listen failed: %s\n", gerr ? gerr->message : "unknown");
		if (gerr)
			g_error_free(gerr);
		g_object_unref(service);
		close(lfd);
		unlink(socket_path);
		ops->destroy(backend);
		return 4;
	}
	g_object_unref(gaddr);

	serve_ctx ctx = { .ops = ops, .backend = backend, .width = width, .height = height,
	                  .shm = NULL, .shm_size = 0 };
	g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), &ctx);
	g_socket_service_start(service);

	fprintf(stderr, "serving backend=%s on %s\n", ops->name, socket_path);
	fflush(stderr);

	/* Block in the loop and dispatch when work arrives. The previous version
	 * spun on g_main_context_pending() without ever calling iteration(), which
	 * busy-waited and never dispatched an incoming connection. */
	for (;;)
		g_main_context_iteration(NULL, TRUE);

	if (ctx.shm)
		munmap(ctx.shm, ctx.shm_size);
	close(lfd);
	g_socket_service_stop(service);
	g_object_unref(service);
	unlink(socket_path);
	ops->destroy(backend);
	return 0;
}
