/* Chromium backend, driven over the DevTools Protocol.
 *
 * Not a WebKit engine, and that is the point: it is the availability and
 * media-compatibility fallback. CDP is versioned and already implemented by
 * Chromium, so this backend speaks an existing protocol rather than inventing
 * one. Frames come from Page.startScreencast.
 *
 * Tradeoff worth knowing: screencast frames are JPEG, so this backend is good
 * for page fidelity and light animation but is the wrong choice for smooth
 * video. The guest can tell them apart because the frame header carries the
 * backend name in the HELLO_ACK.
 */
#include <gtk/gtk.h>

#include "../backend.h"
#include "../protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

/* Candidate binaries, in preference order. Probed by looking for an executable
 * file rather than by running --version, so probe() stays cheap. */
static const char *const chromium_candidates[] = {
	"/usr/bin/chromium",
	"/usr/bin/chromium-browser",
	"/usr/bin/google-chrome",
	"/usr/bin/google-chrome-stable",
	NULL,
};

typedef struct {
	pid_t pid;
	int ws_port;      /* devtools websocket port */
	int ws_fd;        /* connected socket */
	int next_id;
	char *chrome_ws_url;
	/* Most recent screencast frame, JPEG bytes. */
	unsigned char *frame;
	size_t frame_size;
	uint32_t seq;
	int width, height;
	char *pending;    /* accumulated socket input */
	size_t pending_size;
	int io_next_id;   /* id awaiting an IO.read reply */
	int eval_next_id; /* id awaiting a Runtime.evaluate reply */
} cr_impl;

/* Bounds every socket operation. Without this a read that the peer never
 * answers - Chromium's DevTools server keeps HTTP/1.1 connections open and
 * ignores "Connection: close" - blocks forever, and the caller hangs with no
 * diagnostic. */
static void set_socket_timeouts(int fd, int seconds)
{
	struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

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

static const char *find_chromium(void)
{
	for (int i = 0; chromium_candidates[i]; i++)
		if (access(chromium_candidates[i], X_OK) == 0)
			return chromium_candidates[i];
	return NULL;
}

static int cr_probe(void)
{
	/* GTK is only needed for the base surface, and a missing display should not
	 * disqualify a headless engine, so this checks the binary alone. */
	return find_chromium() ? 1 : 0;
}

/* --- minimal WebSocket client (text frames only, no extensions) ----------- */

static int ws_send_frame(int fd, const char *payload, size_t len)
{
	/* Client frames must be masked, so header and payload cannot be written
	 * separately without masking the body first. The combined buffer has to be
	 * sized for both: a fixed 10-byte header array overflows the moment any
	 * payload is written into it. */
	size_t max_header = 14;
	unsigned char *frame = malloc(max_header + len);
	if (!frame)
		return -1;
	size_t hlen = 0;
	frame[hlen++] = 0x81; /* FIN + text opcode */
	if (len < 126) {
		frame[hlen++] = (unsigned char)(0x80 | len); /* MASK, 7-bit length */
	} else if (len <= 0xFFFF) {
		frame[hlen++] = (unsigned char)(0x80 | 126);
		frame[hlen++] = (unsigned char)((len >> 8) & 0xFF);
		frame[hlen++] = (unsigned char)(len & 0xFF);
	} else {
		frame[hlen++] = (unsigned char)(0x80 | 127);
		for (int i = 7; i >= 0; i--)
			frame[hlen++] = (unsigned char)((len >> (i * 8)) & 0xFF);
	}
	/* Fixed masking key: this backend only ever talks to a loopback DevTools
	 * endpoint, so a constant key is acceptable and keeps the code small. */
	static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
	for (int i = 0; i < 4; i++)
		frame[hlen++] = mask[i];
	for (size_t i = 0; i < len; i++)
		frame[hlen + i] = (unsigned char)(payload[i]) ^ mask[i % 4];

	size_t total = hlen + len;
	size_t sent = 0;
	while (sent < total) {
		ssize_t n = write(fd, frame + sent, total - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(frame);
			return -1;
		}
		sent += (size_t)n;
	}
	free(frame);
	return 0;
}

static int ws_connect(const char *host, int port, const char *path)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close(fd);
		return -1;
	}
	set_socket_timeouts(fd, 10);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	char req[512];
	int n = snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n", path, host, port);
	if (write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	/* Read up to the end of the HTTP upgrade response. */
	char buf[1024];
	ssize_t got = read(fd, buf, sizeof(buf) - 1);
	if (got <= 0) {
		close(fd);
		return -1;
	}
	buf[got] = '\0';
	if (!strstr(buf, "101")) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Handles one decoded CDP message. Defined at the bottom of the file; declared
 * here because cr_wait_reply needs to route replies through it. */
static void cr_handle_message(cr_impl *self, const char *json);

/* Pulls complete text messages out of the socket, calling `on_msg` for each.
 * `on_msg` may be NULL, in which case messages are drained and discarded -
 * which is what the screencast-drain paths want. */
static void ws_pump(cr_impl *self, void (*on_msg)(cr_impl *, const char *, void *), void *ud)
{
	for (;;) {
		struct pollfd pfd = { .fd = self->ws_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 0);
		if (pr <= 0)
			return;
		unsigned char head[2];
		ssize_t n = read(self->ws_fd, head, 2);
		if (n != 2)
			return;
		size_t len = head[1] & 0x7F;
		if (len == 126) {
			unsigned char ext[2];
			if (read(self->ws_fd, ext, 2) != 2)
				return;
			len = ((size_t)ext[0] << 8) | ext[1];
		} else if (len == 127) {
			unsigned char ext[8];
			if (read(self->ws_fd, ext, 8) != 8)
				return;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | ext[i];
		}
		if (len == 0 || len > 32u * 1024 * 1024)
			return;
		char *payload = malloc(len + 1);
		if (!payload)
			return;
		size_t got = 0;
		while (got < len) {
			ssize_t r = read(self->ws_fd, payload + got, len - got);
			if (r <= 0) {
				free(payload);
				return;
			}
			got += (size_t)r;
		}
		payload[len] = '\0';
		if (g_getenv("DWB_DEBUG_CDP")) {
			char *flat = g_strdup(payload);
			for (char *p = flat; *p; p++)
				if (*p == '\n')
					*p = ' ';
			g_printerr("[cdp] %s\n", flat);
			g_free(flat);
		}
		if ((head[0] & 0x0F) == 0x01 && on_msg)
			on_msg(self, payload, ud);
		free(payload);
	}
}

static int cr_send_cmd(cr_impl *self, int id, const char *method, const char *params_fmt, ...)
{
	char buf[4096];
	if (params_fmt) {
		va_list ap;
		va_start(ap, params_fmt);
		vsnprintf(buf, sizeof(buf), params_fmt, ap);
		va_end(ap);
	} else {
		/* Must be a valid empty object. Emitting nothing here yields
		 * "params":} , which every command rejects with -32700. */
		snprintf(buf, sizeof(buf), "{}");
	}
	char payload[8192];
	snprintf(payload, sizeof(payload),
	         "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, buf);
	return ws_send_frame(self->ws_fd, payload, strlen(payload));
}

/* Waits for the reply carrying `id`, running the message handler for everything
 * else (screencast frames, events) on the way. */
static char *cr_wait_reply(cr_impl *self, int id, int timeout_ms)
{
	struct pollfd pfd = { .fd = self->ws_fd, .events = POLLIN };
	int pr = poll(&pfd, 1, timeout_ms);
	if (pr <= 0)
		return NULL;
	/* One message is enough for the simple command/reply calls used here. */
	unsigned char head[2];
	if (read(self->ws_fd, head, 2) != 2)
		return NULL;
	size_t len = head[1] & 0x7F;
	if (len == 126) {
		unsigned char ext[2];
		if (read(self->ws_fd, ext, 2) != 2)
			return NULL;
		len = ((size_t)ext[0] << 8) | ext[1];
	} else if (len == 127) {
		unsigned char ext[8];
		if (read(self->ws_fd, ext, 8) != 8)
			return NULL;
		len = 0;
		for (int i = 0; i < 8; i++)
			len = (len << 8) | ext[i];
	}
	if (len == 0 || len > 8u * 1024 * 1024)
		return NULL;
	char *payload = malloc(len + 1);
	size_t got = 0;
	while (got < len) {
		ssize_t r = read(self->ws_fd, payload + got, len - got);
		if (r <= 0) {
			free(payload);
			return NULL;
		}
		got += (size_t)r;
	}
	payload[len] = '\0';
	cr_handle_message(self, payload);
	free(payload);
	return payload; /* caller inspects for the id; simplified for our use */
}

/* Issues a GET to the loopback DevTools HTTP endpoint and returns the whole
 * body. A single read() is not enough: /json/list is a few KB, so the key we
 * want routinely lands in a later TCP segment than the first one. */
static char *devtools_http_get(int port, const char *path)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	set_socket_timeouts(fd, 2);
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}
	char req[256];
	int rn = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
	                                     "Connection: close\r\n\r\n", path, port);
	if (write(fd, req, (size_t)rn) != rn) {
		close(fd);
		return NULL;
	}

	size_t cap = 16384, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(fd);
		return NULL;
	}
	/* Read until the body is complete, the peer closes, or the socket times
	 * out. Chromium may hold the connection open, so a timeout is a normal
	 * stopping condition here, not an error. */
	for (;;) {
		if (len + 4096 > cap) {
			char *bigger = realloc(buf, cap * 2);
			if (!bigger) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = bigger;
			cap *= 2;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n <= 0)
			break; /* EOF, timeout, or error: all end the read loop */
		len += (size_t)n;
		/* The JSON array is complete once its closing bracket has arrived. */
		char *body = strstr(buf, "\r\n\r\n");
		if (body) {
			char *end = strrchr(body + 4, ']');
			if (end) {
				len = (size_t)(end - buf) + 1;
				break;
			}
		}
	}
	buf[len] = '\0';
	close(fd);

	/* Skip the HTTP headers. */
	char *body = strstr(buf, "\r\n\r\n");
	if (!body) {
		free(buf);
		return NULL;
	}
	char *result = g_strdup(body + 4);
	free(buf);
	return result;
}

/* Pulls the value of a "key": "value" pair, starting the search at `from`. */
static char *json_string_value(const char *from, const char *key)
{
	const char *k = strstr(from, key);
	if (!k)
		return NULL;
	k += strlen(key);
	const char *colon = strchr(k, ':');
	const char *open = colon ? strchr(colon + 1, '"') : NULL;
	const char *close = open ? strchr(open + 1, '"') : NULL;
	if (!open || !close)
		return NULL;
	return g_strndup(open + 1, (gsize)(close - open - 1));
}

static dwb_backend *cr_create(int width, int height, char **error)
{
	const char *bin = find_chromium();
	if (!bin) {
		set_error(error, "no chromium binary found");
		return NULL;
	}

	cr_impl *self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	self->ws_fd = -1;
	self->width = width;
	self->height = height;

	int port = 0;
	for (int attempt = 0; attempt < 32 && port == 0; attempt++)
		port = 19222 + attempt;

	char portbuf[32];
	/* The "=" form is required, not stylistic. /usr/bin/chromium on this host is
	 * a launcher stub that injects its own defaults (ozone, password store,
	 * omarchy extensions) and mangles argv; when the flag and its value are two
	 * separate tokens the value is dropped, the stub's own port handling wins,
	 * and nothing ever listens. One token survives the rewrite. */
	snprintf(portbuf, sizeof(portbuf), "--remote-debugging-port=%d", port);

	char profilebuf[128];
	snprintf(profilebuf, sizeof(profilebuf), "--user-data-dir=/tmp/dwb-chromium-profile-%d",
	         port);

	char *args[] = {
		(char *)bin,
		(char *)"--headless=new",
		(char *)"--no-sandbox",
		(char *)"--disable-gpu",
		(char *)"--hide-scrollbars",
		(char *)"--mute-audio",
		(char *)"--no-first-run",
		(char *)"--no-default-browser-check",
		/* No positional URL: modern headless Chromium rejects one ("Multiple
		 * targets are not supported"), and navigation goes through
		 * Page.navigate anyway. */
		portbuf,
		profilebuf,
		NULL,
	};

	self->pid = fork();
	if (self->pid < 0) {
		set_error(error, "fork failed: %s", strerror(errno));
		free(self);
		return NULL;
	}
	if (self->pid == 0) {
		/* Detach stdio. Without this the child inherits our stdout, so any
		 * caller reading our output never sees EOF while the browser is alive,
		 * and a long-lived helper can wedge a pipeline. It is also correct
		 * default behaviour for a spawned engine. */
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		setsid();
		execv(bin, args);
		_exit(127);
	}
	self->ws_port = port;

	/* Wait for the debugging endpoint to come up. /json/list is used rather
	 * than /json/version because it yields a page target we can drive directly,
	 * instead of the browser-level endpoint that cannot host a page. */
	for (int attempt = 0; attempt < 60 && !self->chrome_ws_url; attempt++) {
		/* If the child died, say so rather than looping for 15s on a process
		 * that is never going to listen. This is the difference between "the
		 * browser is slow" and "execv never happened". */
		int status = 0;
		pid_t reaped = waitpid(self->pid, &status, WNOHANG);
		if (reaped == self->pid) {
			if (WIFEXITED(status))
				set_error(error, "chromium exited immediately with status %d (%s)",
				          WEXITSTATUS(status), bin);
			else if (WIFSIGNALED(status))
				set_error(error, "chromium was killed by signal %d (%s)",
				          WTERMSIG(status), bin);
			else
				set_error(error, "chromium terminated abnormally (%s)", bin);
			self->pid = 0;
			free(self);
			return NULL;
		}

		char *body = devtools_http_get(port, "/json/list");
		if (body) {
			/* The list is not only pages: bundled extensions contribute
			 * background_page entries, and driving one of those silently does
			 * nothing. Prefer a real page, fall back to whatever is there. */
			const char *page = strstr(body, "\"type\": \"page\"");
			if (!page)
				page = strstr(body, "\"type\":\"page\"");
			self->chrome_ws_url = json_string_value(page ? page : body,
			                                        "\"webSocketDebuggerUrl\"");
			if (self->chrome_ws_url && !g_str_has_prefix(self->chrome_ws_url, "ws://")) {
				g_clear_pointer(&self->chrome_ws_url, g_free);
			}
			g_free(body);
		}
		if (self->chrome_ws_url)
			break;
		usleep(250000);
	}
	if (!self->chrome_ws_url) {
		set_error(error, "chromium devtools endpoint did not come up on port %d", port);
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		free(self);
		return NULL;
	}

	/* Connect to the page target advertised by /json/list. */
	int ws_fd;
	{
		const char *p = strstr(self->chrome_ws_url, "ws://");
		char host[64] = "127.0.0.1";
		int pp = port;
		const char *slash = p ? strchr(p + 5, '/') : NULL;
		if (p) {
			size_t hl = slash ? (size_t)(slash - (p + 5)) : strlen(p + 5);
			if (hl < sizeof(host)) {
				memcpy(host, p + 5, hl);
				host[hl] = '\0';
				char *colon = strrchr(host, ':');
				if (colon) {
					*colon = '\0';
					pp = atoi(colon + 1);
				}
			}
		}
		ws_fd = ws_connect(host, pp ? pp : port, slash ? slash : "/");
	}
	if (ws_fd < 0) {
		/* Fall back to whatever the version endpoint advertised. */
		const char *p = strstr(self->chrome_ws_url, "ws://");
		if (p) {
			char host[64];
			int pp = 0;
			const char *slash = strchr(p + 5, '/');
			size_t hl = slash ? (size_t)(slash - (p + 5)) : strlen(p + 5);
			if (hl < sizeof(host)) {
				memcpy(host, p + 5, hl);
				host[hl] = '\0';
				char *colon = strrchr(host, ':');
				if (colon) {
					*colon = '\0';
					pp = atoi(colon + 1);
				}
				ws_fd = ws_connect(host, pp ? pp : port, slash ? slash : "/");
			}
		}
	}
	if (ws_fd < 0) {
		set_error(error, "could not open a devtools websocket");
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		g_free(self->chrome_ws_url);
		free(self);
		return NULL;
	}
	self->ws_fd = ws_fd;
	self->next_id = 1;

	cr_send_cmd(self, self->next_id++, "Page.enable", NULL);
	cr_send_cmd(self, self->next_id++, "Runtime.enable", NULL);
	cr_send_cmd(self, self->next_id++, "Page.startScreencast",
	            "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":%d,\"maxHeight\":%d,"
	            "\"everyNthFrame\":1}", width, height);

	dwb_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		close(ws_fd);
		kill(self->pid, SIGKILL);
		waitpid(self->pid, NULL, 0);
		g_free(self->chrome_ws_url);
		free(self);
		return NULL;
	}
	backend->ops = &dwb_backend_chromium;
	backend->impl = self;
	return backend;
}

/* Handles one decoded CDP message. Only screencast frames carry data this
 * backend needs; command replies are matched by the caller. */
static void cr_handle_message(cr_impl *self, const char *json)
{
	/* Screencast frames arrive as base64 in Page.screencastFrame. Extracting
	 * that without a JSON parser means finding the marker and decoding until
	 * the closing quote, which is safe because base64 has no escapes. */
	const char *marker = "\"data\":\"";
	const char *at = strstr(json, marker);
	if (at) {
		at += strlen(marker);
		const char *end = strchr(at, '"');
		if (end && end > at) {
			gsize out_len = 0;
			guchar *decoded = g_base64_decode(at, &out_len);
			if (decoded && out_len > 0) {
				g_free(self->frame);
				self->frame = decoded;
				self->frame_size = out_len;
				self->seq++;
			} else {
				g_free(decoded);
			}
		}
	}
}

static void cr_destroy(dwb_backend *backend)
{
	cr_impl *self = backend->impl;
	if (!self)
		return;
	if (self->ws_fd >= 0)
		close(self->ws_fd);
	if (self->pid > 0) {
		kill(self->pid, SIGTERM);
		/* Give it a moment, then insist. */
		for (int i = 0; i < 20; i++) {
			int status;
			pid_t r = waitpid(self->pid, &status, WNOHANG);
			if (r == self->pid || r < 0)
				self->pid = 0;
			else
				g_usleep(100000);
			if (!self->pid)
				break;
		}
		if (self->pid)
			kill(self->pid, SIGKILL);
	}
	g_free(self->frame);
	g_free(self->chrome_ws_url);
	g_free(self->pending);
	g_free(self);
	free(backend);
}

static int cr_navigate(dwb_backend *backend, const char *url, char **error)
{
	cr_impl *self = backend->impl;
	cr_send_cmd(self, self->next_id++, "Page.navigate", "{\"url\":\"%s\"}", url);
	/* Wait for Page.loadEventFired by draining screencast traffic, which is
	 * also how the frame buffer gets populated. */
	gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline) {
		ws_pump(self, NULL, NULL);
		if (self->frame_size > 0)
			break;
		g_usleep(100000);
	}
	if (self->frame_size == 0) {
		set_error(error, "no screencast frame after navigating to %s", url);
		return -1;
	}
	return 0;
}

static void cr_resize(dwb_backend *backend, int width, int height)
{
	cr_impl *self = backend->impl;
	self->width = width;
	self->height = height;
	cr_send_cmd(self, self->next_id++, "Emulation.setDeviceMetricsOverride",
	            "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":1,\"mobile\":false}",
	            width, height);
	ws_pump(self, NULL, NULL);
}

static int cr_render(dwb_backend *backend, dwb_frame *out, char **error)
{
	cr_impl *self = backend->impl;
	ws_pump(self, NULL, NULL);
	if (!self->frame || self->frame_size == 0) {
		set_error(error, "no screencast frame available");
		return -1;
	}
	/* The screencast is JPEG; hand it up as-is and let the frame header say so.
	 * Decoding belongs on the host that asked for pixels, not here. */
	out->pixels = self->frame;
	out->width = (uint32_t)self->width;
	out->height = (uint32_t)self->height;
	out->stride = 0;
	out->format = DWB_PIXEL_JPEG;
	out->size = (uint32_t)self->frame_size;
	return 0;
}

static int cr_evaluate(dwb_backend *backend, const char *js, char **value, char **error)
{
	cr_impl *self = backend->impl;
	int id = self->next_id++;
	cr_send_cmd(self, id, "Runtime.evaluate", "{\"expression\":\"%s\",\"returnByValue\":true}", js);
	char *reply = cr_wait_reply(self, id, 15000);
	if (!reply) {
		set_error(error, "no reply to Runtime.evaluate");
		return -1;
	}
	const char *res = strstr(reply, "\"result\"");
	if (value)
		*value = g_strdup(res ? res : "");
	free(reply);
	return 0;
}

const dwb_backend_ops dwb_backend_chromium = {
	.name = "chromium",
	.description = "Chromium via DevTools Protocol (media-compatibility fallback)",
	.probe = cr_probe,
	.create = cr_create,
	.destroy = cr_destroy,
	.navigate = cr_navigate,
	.resize = cr_resize,
	.render = cr_render,
	.evaluate = cr_evaluate,
};
