#ifndef TEDER_NOTCURSES_H
#define TEDER_NOTCURSES_H

/* Separate optional library. This header does not include notcurses headers. */
#include "teder.h"
#if defined(_WIN32)
#if defined(TEDER_NOTCURSES_STATIC)
#define TEDER_NOTCURSES_API
#elif defined(TEDER_NOTCURSES_BUILD)
#define TEDER_NOTCURSES_API __declspec(dllexport)
#else
#define TEDER_NOTCURSES_API __declspec(dllimport)
#endif
#else
#define TEDER_NOTCURSES_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct ncplane;
typedef struct teder_renderer teder_renderer;
enum { TEDER_RENDER_SPECTRUM = 1, TEDER_RENDER_LEVELS };
typedef struct teder_renderer_config {
	uint32_t struct_size, abi_version;
	uint32_t mode, foreground_rgb, background_rgb, peak_rgb;
	float floor_db;
	uint32_t reserved;
} teder_renderer_config;
typedef struct teder_rectangle { int32_t y, x; uint32_t rows, columns; } teder_rectangle;

/* Check the linked runtime's numeric version, then copy teder build information.
 * This does not certify fork identity or promise implemented drawing features.
 */
TEDER_NOTCURSES_API teder_code TEDER_CALL teder_renderer_get_build_info(teder_build_info *out);

/* Create/configure copy input. The renderer owns no engine, snapshot or plane.
 * All calls on one renderer are serialized. Configure failure preserves it.
 */
TEDER_NOTCURSES_API teder_code TEDER_CALL teder_renderer_create(
	const teder_renderer_config *config, teder_renderer **out);
TEDER_NOTCURSES_API teder_code TEDER_CALL teder_renderer_configure(
	teder_renderer *renderer, const teder_renderer_config *config);
/* Draw runs on the host's notcurses thread; borrow ends when draw returns.
 * NULL snapshot draws no-data. Full redraw of the clipped rectangle only.
 * Zero-area rectangles succeed. No notcurses_render, terminal init or teardown.
 * Plane and renderer must use the same loaded notcurses runtime/ABI.
 */
TEDER_NOTCURSES_API teder_code TEDER_CALL teder_renderer_draw(
	teder_renderer *renderer, const teder_snapshot *snapshot,
	struct ncplane *plane, const teder_rectangle *rectangle);
TEDER_NOTCURSES_API void TEDER_CALL teder_renderer_destroy(teder_renderer *renderer);
#ifdef __cplusplus
}
#endif
#endif
