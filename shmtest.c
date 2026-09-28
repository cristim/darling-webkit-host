/* Shared-memory frame transport test.
 *
 * Inline frames cost ~3MB each for a 1280x800 RGB view, which is ~90MB/s at
 * 30fps - unusable for video. With a shared region attached the host copies the
 * frame once and sends only the header. This drives that path and verifies the
 * guest-visible contract: the header arrives with DWB_FRAME_IN_SHM set, no
 * pixel bytes follow it on the socket, and the pixels are readable in the
 * region at the documented offset.
 */
#define _GNU_SOURCE
#include "protocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok, const char *detail)
{
	printf("%-52s %s%s%s\n", what, ok ? "PASS" : "FAIL",
	       detail && *detail ? "  " : "", detail ? detail : "");
	if (!ok)
		failures++;
}

static int send_msg(int fd, uint16_t type, const void *payload, uint32_t len)
{
	dwb_header h = { .magic = DWB_MAGIC, .version = DWB_PROTO_VERSION,
	                 .type = type, .length = len };
	if (write(fd, &h, sizeof(h)) != (ssize_t)sizeof(h))
		return -1;
	if (len && write(fd, payload, len) != (ssize_t)len)
		return -1;
	return 0;
}

static int recv_msg(int fd, dwb_header *h, void *payload, size_t cap)
{
	if (read(fd, h, sizeof(*h)) != (ssize_t)sizeof(*h))
		return -1;
	if (h->length > cap)
		return -1;
	if (h->length && read(fd, payload, h->length) != (ssize_t)h->length)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/dwb-shm.sock";
	const char *url = argc > 2 ? argv[2] : "https://example.com";
	const size_t region = 16u * 1024 * 1024;

	/* A file-backed region stands in for what a guest would get from memfd or
	 * shm_open; the host only ever sees a path and maps it. */
	char shm_path[256];
	snprintf(shm_path, sizeof(shm_path), "/home/cristi/.local/share/agent-tmp/youlearn/frame-shm-%d", (int)getpid());
	int sfd = open(shm_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (sfd < 0) {
		perror("create shm file");
		return 2;
	}
	if (ftruncate(sfd, (off_t)region) != 0) {
		perror("ftruncate");
		close(sfd);
		return 2;
	}
	unsigned char *map = mmap(NULL, region, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
	close(sfd);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(map, 0, region);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		return 2;
	}
	printf("connected, region %s (%zu bytes)\n\n", shm_path, region);

	char buf[65536];
	dwb_header h;
	send_msg(fd, DWB_MSG_HELLO, "{\"proto\":1}", 11);
	recv_msg(fd, &h, buf, sizeof(buf));
	check("handshake", h.type == DWB_MSG_HELLO_ACK, buf);

	/* Baseline: with no region attached, the frame must arrive inline. */
	char nav[4096];
	snprintf(nav, sizeof(nav), "{\"url\":\"%s\"}", url);
	send_msg(fd, DWB_MSG_NAVIGATE, nav, (uint32_t)strlen(nav));
	recv_msg(fd, &h, buf, sizeof(buf));
	check("navigate", h.type == DWB_MSG_EVENT, buf);

	send_msg(fd, DWB_MSG_FRAME, NULL, 0);
	int ok = recv_msg(fd, &h, buf, sizeof(buf)) == 0 && h.type == DWB_MSG_FRAME;
	dwb_frame_header inline_fh;
	memcpy(&inline_fh, buf, sizeof(inline_fh));
	check("inline frame: header only, flag clear", ok && inline_fh.reserved != DWB_FRAME_IN_SHM, "");
	printf("    inline geometry %ux%u stride=%u format=%u bytes=%u\n",
	       inline_fh.width, inline_fh.height, inline_fh.stride, inline_fh.format, inline_fh.size);
	/* Drain the inline pixel bytes with a plain read. recv_msg() would
	 * reinterpret 3MB of pixels as a message header and desynchronise the
	 * stream, which is exactly what happened the first time round. */
	if (ok && inline_fh.size) {
		uint32_t left = inline_fh.size;
		unsigned char sink[65536];
		while (left) {
			size_t want = left < sizeof(sink) ? left : sizeof(sink);
			ssize_t n = read(fd, sink, want);
			if (n <= 0)
				break;
			left -= (uint32_t)n;
		}
	}

	/* Now attach a region and confirm frames move into it. */
	memset(map, 0, region);
	char attach[1024];
	snprintf(attach, sizeof(attach), "{\"path\":\"%s\",\"size\":%zu}", shm_path, region);
	send_msg(fd, DWB_MSG_SHM_ATTACH, attach, (uint32_t)strlen(attach));
	ok = recv_msg(fd, &h, buf, sizeof(buf)) == 0;
	check("shm attach: acknowledged", ok, buf);

	send_msg(fd, DWB_MSG_FRAME, NULL, 0);
	ok = recv_msg(fd, &h, buf, sizeof(buf)) == 0 && h.type == DWB_MSG_FRAME;
	dwb_frame_header shm_fh;
	memcpy(&shm_fh, buf, sizeof(shm_fh));
	check("shm frame: header received", ok, "");
	check("shm frame: DWB_FRAME_IN_SHM set", ok && shm_fh.reserved == DWB_FRAME_IN_SHM, "");
	check("shm frame: no pixels on the socket",
	      ok && h.length == sizeof(dwb_frame_header), "");
	printf("    shm geometry %ux%u stride=%u format=%u bytes=%u\n",
	       shm_fh.width, shm_fh.height, shm_fh.stride, shm_fh.format, shm_fh.size);

	/* The region must hold the same frame the socket would have carried. */
	int nonzero = 0;
	for (uint32_t i = 0; i < shm_fh.size; i += 1013)
		if (map[sizeof(dwb_frame_header) + i])
			nonzero = 1;
	check("shm frame: pixels readable and not all zero", nonzero, "");
	check("shm frame: geometry unchanged from inline",
	      shm_fh.width == inline_fh.width && shm_fh.height == inline_fh.height, "");
	check("shm frame: fits the attached region",
	      sizeof(dwb_frame_header) + shm_fh.size <= region, "");

	send_msg(fd, DWB_MSG_BYE, NULL, 0);
	close(fd);
	munmap(map, region);
	unlink(shm_path);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
