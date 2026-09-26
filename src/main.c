/* darling-webkit-host: host-side web engine service for Darling's WebKit.
 *
 * Two roles in one binary:
 *
 *   1. `--render` - standalone, no protocol, no guest. Loads a URL in the best
 *      available backend and writes a PNG. This is the verification path: it
 *      answers "does engine X actually render and play Y" on the host, which is
 *      the question the Darling guest work depends on.
 *
 *   2. default - serve the protocol from protocol.h on a Unix socket for the
 *      guest's WebKit.framework to connect to.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "protocol.h"

/* --- --list-backends ---------------------------------------------------- */

static int cmd_list_backends(void)
{
	printf("%-12s %-8s %s\n", "BACKEND", "AVAILABLE", "DESCRIPTION");
	for (const dwb_backend_ops *const *b = dwb_backend_table(); *b; b++) {
		printf("%-12s %-8s %s\n", (*b)->name, (*b)->probe() ? "yes" : "no",
		       (*b)->description);
	}
	return 0;
}

/* --- --render ----------------------------------------------------------- */

static uint32_t hash_frame(const dwb_frame *f)
{
	/* FNV-1a over the pixels, so "did the frame change" is answerable without
	 * keeping every frame around. */
	uint32_t h = 2166136261u;
	const unsigned char *p = f->pixels;
	for (uint32_t i = 0; i < f->size; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static int cmd_render(const char *url, const char *out_path, const char *backend_name,
                      int width, int height, int watch_ms, const char *eval_js,
                      int eval_wait_ms, const char *eval2_js)
{
	char *error = NULL;
	const dwb_backend_ops *ops = dwb_backend_select(backend_name, &error);
	if (!ops) {
		fprintf(stderr, "no backend: %s\n", error ? error : "unknown");
		g_free(error);
		return 2;
	}
	printf("backend=%s (%s)\n", ops->name, ops->description);
	fflush(stdout);

	dwb_backend *backend = ops->create(width, height, &error);
	if (!backend) {
		fprintf(stderr, "create failed: %s\n", error ? error : "unknown");
		g_free(error);
		return 3;
	}

	if (ops->navigate(backend, url, &error) != 0) {
		fprintf(stderr, "navigate failed: %s\n", error ? error : "unknown");
		g_free(error);
		ops->destroy(backend);
		return 4;
	}
	printf("navigated=%s\n", url);

	/* Settle time before probing, so slow-starting players (YouTube especially)
	 * have a chance to reach a playing state before anything is measured. */
	if (eval_wait_ms > 0) {
		g_usleep((guint64)eval_wait_ms * 1000);
		printf("settled_ms=%d\n", eval_wait_ms);
	}

	/* Evaluating the page is far stronger evidence than hashing pixels: it
	 * reports the media element's own currentTime, paused and readyState, so
	 * "the frames moved" can be attributed to actual decoding. */
	if (eval_js && *eval_js) {
		char *value = NULL;
		if (ops->evaluate(backend, eval_js, &value, &error) == 0) {
			printf("eval=%s\n", value ? value : "");
			g_free(value);
		} else {
			printf("eval_error=%s\n", error ? error : "unknown");
			g_free(error);
			error = NULL;
		}
	}

	dwb_frame frame;
	if (ops->render(backend, &frame, &error) != 0) {
		fprintf(stderr, "render failed: %s\n", error ? error : "unknown");
		g_free(error);
		ops->destroy(backend);
		return 5;
	}
	printf("frame0=%ux%u stride=%u format=%u bytes=%u\n", frame.width, frame.height,
	       frame.stride, frame.format, frame.size);

	/* A single frame proves rendering. Watching for changes is what proves
	 * animation or video is actually running rather than a static poster. */
	uint32_t prev = hash_frame(&frame);
	int distinct = 1;
	if (watch_ms > 0) {
		int elapsed = 0;
		while (elapsed < watch_ms) {
			g_usleep(250000);
			elapsed += 250;
			if (ops->render(backend, &frame, &error) != 0)
				break;
			uint32_t h = hash_frame(&frame);
			if (h != prev) {
				prev = h;
				distinct++;
			}
		}
	}
	printf("distinct_frames=%d over %dms\n", distinct, watch_ms);

	/* Second probe, after the watch window. Needed for playback tests: the
	 * first eval observes the state YouTube leaves the player in (usually
	 * paused), the action happens, and only this one can show currentTime
	 * actually advancing. */
	if (eval2_js && *eval2_js) {
		char *value = NULL;
		if (ops->evaluate(backend, eval2_js, &value, &error) == 0) {
			printf("eval2=%s\n", value ? value : "");
			g_free(value);
		} else {
			printf("eval2_error=%s\n", error ? error : "unknown");
			g_free(error);
			error = NULL;
		}
	}

	/* Dump the last frame so a human can look at it. Raw formats go out as
	 * PPM via cairo when GTK is around; for the JPEG backend write the bytes
	 * we were handed. */
	if (out_path && *out_path) {
		if (frame.format == DWB_PIXEL_JPEG) {
			FILE *f = fopen(out_path, "wb");
			if (f) {
				fwrite(frame.pixels, 1, frame.size, f);
				fclose(f);
				printf("wrote=%s (jpeg)\n", out_path);
			}
		} else if (frame.format == DWB_PIXEL_BGRA && frame.stride > 0) {
			cairo_surface_t *surface = cairo_image_surface_create_for_data(
				frame.pixels, CAIRO_FORMAT_ARGB32, (int)frame.width, (int)frame.height,
				(int)frame.stride);
			cairo_surface_write_to_png(surface, out_path);
			cairo_surface_destroy(surface);
			printf("wrote=%s (png)\n", out_path);
		}
	}

	ops->destroy(backend);
	return distinct > 1 ? 0 : 6;
}

/* --- protocol service --------------------------------------------------- */

static int cmd_serve(const char *socket_path, const char *backend_name, int width, int height)
{
	char *error = NULL;
	const dwb_backend_ops *ops = dwb_backend_select(backend_name, &error);
	if (!ops) {
		fprintf(stderr, "no backend: %s\n", error ? error : "unknown");
		g_free(error);
		return 2;
	}
	printf("backend=%s socket=%s\n", ops->name, socket_path);
	fflush(stdout);

	/* The socket half is intentionally not implemented yet: the guest side of
	 * the proxy is blocked on spikes 0.1-0.3 (AF_UNIX reachability, shared
	 * memory, and guest-side compositing). Until those pass, shipping a
	 * half-wired server would be code no caller can exercise. */
	fprintf(stderr,
	        "serve mode is not wired up yet; run the guest-side spikes first\n");
	return 7;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "usage:\n"
	        "  %s --list-backends\n"
	        "  %s [--backend NAME] [--size WxH] [--watch MS] [--settle MS] [--eval JS]\n"
	        "        --render URL [OUT.png]\n"
	        "  %s [--backend NAME] --serve SOCKET\n", argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	const char *backend_name = g_getenv("DWB_BACKEND");
	const char *url = NULL;
	const char *out = NULL;
	const char *socket_path = NULL;
	const char *eval_js = NULL;
	const char *eval2_js = NULL;
	int width = 1280, height = 800, watch_ms = 0, eval_wait_ms = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list-backends") == 0)
			return cmd_list_backends();
		else if (strcmp(argv[i], "--render") == 0 && i + 1 < argc)
			url = argv[++i];
		else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc)
			socket_path = argv[++i];
		else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
			backend_name = argv[++i];
		else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc)
			watch_ms = atoi(argv[++i]);
		else if (strcmp(argv[i], "--eval") == 0 && i + 1 < argc)
			eval_js = argv[++i];
		else if (strcmp(argv[i], "--eval2") == 0 && i + 1 < argc)
			eval2_js = argv[++i];
		else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc)
			eval_wait_ms = atoi(argv[++i]);
		else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
				fprintf(stderr, "bad --size\n");
				return 1;
			}
		} else if (argv[i][0] != '-' && !url) {
			url = argv[i];
		} else if (argv[i][0] != '-' && url && !out) {
			out = argv[i];
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (url)
		return cmd_render(url, out, backend_name, width, height, watch_ms, eval_js,
		                  eval_wait_ms, eval2_js);
	if (socket_path)
		return cmd_serve(socket_path, backend_name, width, height);

	usage(argv[0]);
	return 1;
}
