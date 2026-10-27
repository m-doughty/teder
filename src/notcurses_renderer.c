#include "renderer.h"
#include "handle.h"
#include <notcurses/notcurses.h>
#include <limits.h>
#include <math.h>

struct teder_renderer {
	td_handle header;
	td_arena *arena;
	teder_renderer_config config;
};

static teder_code resolve(const teder_renderer_config *input, teder_renderer_config *out)
{
	teder_code code = td_prefix(input, sizeof(*input));
	if (code != TEDER_OK)
		return code;
	*out = *input;
	if (!out->mode)
		out->mode = TEDER_RENDER_SPECTRUM;
	if (!out->floor_db)
		out->floor_db = -90;
	if ((out->mode != TEDER_RENDER_SPECTRUM && out->mode != TEDER_RENDER_LEVELS) || out->reserved ||
	    !isfinite(out->floor_db) || out->floor_db >= 0 ||
	    ((out->foreground_rgb | out->background_rgb | out->peak_rgb) & UINT32_C(0xff000000)))
		return TEDER_INVALID;
	return TEDER_OK;
}

teder_code td_renderer_create_with(const teder_renderer_config *input,
                                   td_allocator allocator,
                                   teder_renderer **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	teder_renderer_config c;
	teder_code code = resolve(input, &c);
	teder_build_info build = {.struct_size = sizeof(build), .abi_version = TEDER_ABI_VERSION};
	if (code == TEDER_OK)
		code = teder_renderer_get_build_info(&build);
	if (code != TEDER_OK)
		return code;
	td_arena *arena = td_arena_create(allocator, sizeof(teder_renderer));
	if (!arena)
		return TEDER_NO_MEMORY;
	teder_renderer *r = td_arena_alloc(arena, 1, sizeof(*r), _Alignof(teder_renderer));
	*r = (teder_renderer){{TD_RENDERER, TEDER_ABI_VERSION}, arena, c};
	*out = r;
	return TEDER_OK;
}

teder_code TEDER_CALL teder_renderer_create(const teder_renderer_config *c, teder_renderer **out)
{
	return td_renderer_create_with(c, td_libc_allocator(), out);
}

teder_code TEDER_CALL teder_renderer_configure(teder_renderer *r,
                                               const teder_renderer_config *input)
{
	teder_code code = td_handle_check(r, TD_RENDERER);
	teder_renderer_config c;
	if (code == TEDER_OK)
		code = resolve(input, &c);
	if (code == TEDER_OK)
		r->config = c;
	return code;
}

void TEDER_CALL teder_renderer_destroy(teder_renderer *r)
{
	if (td_handle_check(r, TD_RENDERER) == TEDER_OK)
		td_arena_destroy(r->arena);
}

/* Overwriting a wide continuation erases its left cell too. Inspect both
 * boundaries before any write, so that no existing glyph crosses the clip. */
static teder_code boundary(struct ncplane *plane, int y, int x)
{
	nccell cell = {0};
	int result = ncplane_at_yx_cell(plane, y, x, &cell);
	bool crosses = nccell_wide_right_p(&cell);
	nccell_release(plane, &cell);
	return result < 0 ? TEDER_IO : crosses ? TEDER_UNSUPPORTED : TEDER_OK;
}

static uint32_t height(double db, float floor_db, uint32_t rows)
{
	double fraction = (db - floor_db) / -(double)floor_db;
	if (fraction <= 0)
		return 0;
	if (fraction >= 1)
		return rows;
	return (uint32_t)floor(fraction * rows + .5);
}

static double level(float value, float floor_db)
{
	return value > 0 ? 20 * log10((double)value) : floor_db;
}

teder_code TEDER_CALL teder_renderer_draw(teder_renderer *r,
                                          const teder_snapshot *snapshot,
                                          struct ncplane *plane,
                                          const teder_rectangle *rect)
{
	teder_code code = td_handle_check(r, TD_RENDERER);
	if (code != TEDER_OK)
		return code;
	if (!plane || !rect)
		return TEDER_INVALID;
	if (!rect->rows || !rect->columns)
		return TEDER_OK;
	unsigned rows, columns;
	ncplane_dim_yx(plane, &rows, &columns);
	if (rows > INT_MAX || columns > INT_MAX)
		return TEDER_LIMIT;
	int64_t y0 = rect->y < 0 ? 0 : rect->y, x0 = rect->x < 0 ? 0 : rect->x;
	int64_t bottom = (int64_t)rect->y + rect->rows, right = (int64_t)rect->x + rect->columns;
	int64_t y1 = bottom < rows ? bottom : rows, x1 = right < columns ? right : columns;
	if (y0 >= y1 || x0 >= x1)
		return TEDER_OK;
	teder_snapshot_info info = {.struct_size = sizeof(info), .abi_version = TEDER_ABI_VERSION};
	const float *bands = NULL;
	uint32_t count = 0;
	if (snapshot) {
		code = teder_snapshot_get_info(snapshot, &info);
		if (code == TEDER_OK && r->config.mode == TEDER_RENDER_SPECTRUM)
			code = teder_snapshot_bands(snapshot, &bands, &count);
		if (code != TEDER_OK)
			return code;
	}
	for (int64_t y = y0; y < y1; y++) {
		code = boundary(plane, (int)y, (int)x0);
		if (code == TEDER_OK && x1 < columns)
			code = boundary(plane, (int)y, (int)x1);
		if (code != TEDER_OK)
			return code;
	}
	uint64_t saved_channels = ncplane_channels(plane);
	uint16_t saved_styles = ncplane_styles(plane);
	ncplane_set_styles(plane, 0);
	for (int64_t x = x0; x < x1 && code == TEDER_OK; x++) {
		uint64_t column = (uint64_t)(x - rect->x);
		uint32_t filled = 0, peak = 0;
		if (count) {
			uint32_t first = (uint32_t)(column * count / rect->columns);
			uint32_t end = (uint32_t)((column + 1) * count / rect->columns);
			if (end == first)
				end++;
			float db = bands[first];
			for (uint32_t n = first + 1; n < end; n++)
				db = fmaxf(db, bands[n]);
			filled = height(db, r->config.floor_db, rect->rows);
		} else if (snapshot && r->config.mode == TEDER_RENDER_LEVELS) {
			unsigned ch = (unsigned)(2 * column / rect->columns);
			float rms = rect->columns == 1 ? fmaxf(info.rms[0], info.rms[1]) : info.rms[ch];
			float p = rect->columns == 1 ? fmaxf(info.peak[0], info.peak[1]) : info.peak[ch];
			filled = height(level(rms, r->config.floor_db), r->config.floor_db, rect->rows);
			peak = height(level(p, r->config.floor_db), r->config.floor_db, rect->rows);
		}
		for (int64_t y = y0; y < y1 && code == TEDER_OK; y++) {
			uint32_t rgb =
			    y >= bottom - filled ? r->config.foreground_rgb : r->config.background_rgb;
			if (peak && y == bottom - peak)
				rgb = r->config.peak_rgb;
			uint64_t channels = 0;
			ncchannels_set_fg_rgb(&channels, r->config.foreground_rgb);
			ncchannels_set_bg_rgb(&channels, rgb);
			ncplane_set_channels(plane, channels);
			/* putc duplicates its glyph on the heap; an inline ASCII EGC does not. */
			if (ncplane_putegc_yx(plane, (int)y, (int)x, " ", NULL) != 1)
				code = TEDER_IO;
		}
	}
	ncplane_set_styles(plane, saved_styles);
	ncplane_set_channels(plane, saved_channels);
	return code;
}
