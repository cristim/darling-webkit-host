/* Backend registry and selection. */
#include "backend.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

/* Priority order. WebKit first because it is the same engine as macOS WebKit,
 * so it is the fidelity backend: a guest app sees WKWebView's own DOM and JS
 * semantics. Chromium is the fallback, carried for media compatibility.
 *
 * Adding an engine here is the whole integration: implement dwb_backend_ops in
 * a new file, guard it in CMake, and append it to this table. Nothing in the
 * protocol, the registry logic, or the guest changes. */
static const dwb_backend_ops *const backends[] = {
	&dwb_backend_webkitgtk,
	&dwb_backend_chromium,
	NULL,
};

const dwb_backend_ops *const *dwb_backend_table(void)
{
	return backends;
}

const dwb_backend_ops *dwb_backend_select(const char *forced, char **error)
{
	if (forced && *forced) {
		for (int i = 0; backends[i]; i++) {
			if (strcmp(backends[i]->name, forced) != 0)
				continue;
			if (!backends[i]->probe()) {
				if (error) {
					char *msg = g_strdup_printf(
						"backend '%s' was requested but its probe() failed", forced);
					*error = msg;
				}
				return NULL;
			}
			return backends[i];
		}
		if (error) {
			char *known = g_strjoinv(", ", (char **)backends);
			char *msg = g_strdup_printf(
				"unknown backend '%s' (this build has: %s)", forced, known);
			*error = msg;
			g_free(known);
		}
		return NULL;
	}

	for (int i = 0; backends[i]; i++) {
		if (backends[i]->probe())
			return backends[i];
	}

	if (error) {
		char *msg = g_strdup(
			"no usable web engine: this build has no backend whose probe() succeeded");
		*error = msg;
	}
	return NULL;
}
