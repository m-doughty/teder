#include "renderer.h"
#include "support/harness.h"
#include "support/engine.h"
#include <notcurses/notcurses.h>
#include <check.h>
#include <math.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#include <dlfcn.h>
#endif

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert_msg((call) == TEDER_OK, "%s failed", #call)

static teder_renderer_config config(void)
{
	teder_renderer_config c = INIT(teder_renderer_config);
	c.foreground_rgb = 0x33cc66;
	c.background_rgb = 0x112233;
	c.peak_rgb = 0xff9900;
	return c;
}

static uint64_t channels(unsigned fg, unsigned bg)
{
	uint64_t c = 0;
	ncchannels_set_fg_rgb(&c, fg);
	ncchannels_set_bg_rgb(&c, bg);
	return c;
}

typedef struct {
	FILE *output;
	struct notcurses *nc;
	struct ncplane *plane;
} screen;

static screen screen_open(void)
{
	screen s = {0};
#ifdef _WIN32
	s.output = fopen("NUL", "w");
#else
	s.output = fopen("/dev/null", "w");
#endif
	ck_assert_ptr_nonnull(s.output);
	notcurses_options opts = {.termtype = "xterm-256color",
	                          .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN |
	                                   NCOPTION_NO_QUIT_SIGHANDLERS | NCOPTION_NO_WINCH_SIGHANDLER |
	                                   NCOPTION_DRAIN_INPUT};
	s.nc = notcurses_core_init(&opts, s.output);
	ck_assert_msg(s.nc, "Pinned notcurses core/terminfo initialization failed");
	ncplane_options plane = {.y = 2, .x = 3, .rows = 6, .cols = 8};
	s.plane = ncplane_create(notcurses_stdplane(s.nc), &plane);
	ck_assert_ptr_nonnull(s.plane);
	ck_assert(ncplane_set_base(s.plane, "b", NCSTYLE_UNDERLINE, channels(0x123456, 0x654321)) >= 0);
	return s;
}

static void screen_close(screen *s)
{
	ck_assert(ncplane_destroy(s->plane) == 0);
	ck_assert(notcurses_stop(s->nc) == 0);
	ck_assert(fclose(s->output) == 0);
}

static void fill(struct ncplane *p)
{
	unsigned rows, cols;
	ncplane_dim_yx(p, &rows, &cols);
	ncplane_set_channels(p, channels(0xabcdef, 0x987654));
	ncplane_set_styles(p, NCSTYLE_BOLD);
	for (unsigned y = 0; y < rows; y++)
		for (unsigned x = 0; x < cols; x++)
			ck_assert(ncplane_putegc_yx(p, (int)y, (int)x, "!", NULL) == 1);
}

static void
cell(struct ncplane *p, int y, int x, const char *glyph, uint64_t colors, unsigned styles)
{
	nccell c = {0};
	ck_assert(ncplane_at_yx_cell(p, y, x, &c) >= 0);
	ck_assert_str_eq(nccell_extended_gcluster(p, &c), glyph);
	ck_assert(c.channels == colors && c.stylemask == styles);
	nccell_release(p, &c);
}

static void unchanged(struct ncplane *p)
{
	unsigned rows, cols;
	ncplane_dim_yx(p, &rows, &cols);
	for (unsigned y = 0; y < rows; y++)
		for (unsigned x = 0; x < cols; x++)
			cell(p, (int)y, (int)x, "!", channels(0xabcdef, 0x987654), NCSTYLE_BOLD);
}

/* A real worker publishes post-gain PCM; the loan outlives its destroyed engine. */
static teder_snapshot *snapshot(bool spectrum)
{
	teder_config ec = INIT(teder_config);
	teder_engine *e;
	OK(teder_create(&ec, &e));
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = 1;
	c.bands = spectrum ? 2 : 0;
	c.window_frames = spectrum ? 32 : 4;
	c.low_hz = spectrum ? 750 : 0;
	c.high_hz = spectrum ? 12000 : 0;
	uint64_t id;
	teder_code code;
	do {
		code = teder_configure_analysis(e, &c, &id);
	} while (code == TEDER_BUSY);
	OK(code);
	teder_request_info request = INIT(teder_request_info);
	for (;;) {
		code = teder_request_get(e, id, &request);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && request.state == TEDER_TERMINAL)
			break;
		th_yield();
	}
	OK(request.outcome);
	float pcm[64] = {.5f, .125f};
	if (spectrum)
		for (unsigned n = 0; n < 32; n++) {
			pcm[2 * n] = (float)sin(2 * 3.141592653589793 * (n % 8) / 8);
			pcm[2 * n + 1] = -pcm[2 * n];
		}
	td_analysis_write w = td_analysis_begin(&e->analysis, 1, 48000, 0, true);
	td_analysis_append(&w, pcm, c.window_frames, 1, 2);
	td_analysis_finish(&w);
	teder_snapshot *s = NULL;
	do {
		code = teder_snapshot_acquire(e, &s);
		if (code == TEDER_AGAIN)
			th_yield();
	} while (code == TEDER_AGAIN);
	OK(code);
	OK(th_close(e));
	OK(teder_destroy(&e));
	return s;
}

START_TEST(bounds)
{
	th_tracker tracker;
	ck_assert(th_tracker_init(&tracker));
	teder_renderer_config c = config();
	teder_renderer *r = NULL;
	for (unsigned n = 0; n < 7; n++) {
		teder_renderer_config bad = c;
		if (n == 0)
			bad.mode = 99;
		else if (n == 1)
			bad.floor_db = NAN;
		else if (n == 2)
			bad.floor_db = 1;
		else if (n == 3)
			bad.foreground_rgb |= 0x1000000;
		else if (n == 4)
			bad.background_rgb |= 0x1000000;
		else if (n == 5)
			bad.peak_rgb |= 0x1000000;
		else
			bad.reserved = 1;
		ck_assert(td_renderer_create_with(&bad, th_allocator(&tracker), &r) == TEDER_INVALID && !r);
	}
	c.struct_size--;
	ck_assert(teder_renderer_create(&c, &r) == TEDER_ABI_MISMATCH && !r);
	c = config();
	tracker.fail_at = 1;
	ck_assert(td_renderer_create_with(&c, th_allocator(&tracker), &r) == TEDER_NO_MEMORY && !r);
	ck_assert(th_tracker_clean(&tracker));
	tracker.fail_at = 0;
	OK(td_renderer_create_with(&c, th_allocator(&tracker), &r));
	ck_assert(tracker.live == 1);
	c.floor_db = -60;
	OK(teder_renderer_configure(r, &c));
	ck_assert(tracker.attempts == 2 && tracker.live == 1);
	ck_assert(teder_renderer_create(NULL, NULL) == TEDER_INVALID);
	ck_assert(teder_renderer_configure(NULL, &c) == TEDER_INVALID);
	teder_renderer_destroy(r);
	teder_renderer_destroy(NULL);
	ck_assert(th_tracker_clean(&tracker));
	th_tracker_uninit(&tracker);
}
END_TEST

START_TEST(spectrum_cells)
{
	screen s = screen_open();
	fill(s.plane);
	teder_renderer_config c = config();
	teder_renderer *r;
	OK(teder_renderer_create(&c, &r));
	teder_snapshot *data = snapshot(true);
	teder_rectangle rect = {1, 1, 4, 4};
	c.foreground_rgb = 0; /* caller mutation cannot change the renderer copy */
	c.mode = 99;
	ck_assert(teder_renderer_configure(r, &c) == TEDER_INVALID);
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 8; x++) {
			bool inside = y >= 1 && y < 5 && x >= 1 && x < 5;
			cell(s.plane,
			     y,
			     x,
			     inside ? " " : "!",
			     inside ? channels(0x33cc66, x >= 3 ? 0x33cc66 : 0x112233)
			            : channels(0xabcdef, 0x987654),
			     inside ? 0 : NCSTYLE_BOLD);
		}
	ck_assert(ncplane_channels(s.plane) == channels(0xabcdef, 0x987654));
	ck_assert(ncplane_styles(s.plane) == NCSTYLE_BOLD);
	nccell base = {0};
	ck_assert(ncplane_base(s.plane, &base) >= 0);
	ck_assert_str_eq(nccell_extended_gcluster(s.plane, &base), "b");
	ck_assert(base.channels == channels(0x123456, 0x654321) && base.stylemask == NCSTYLE_UNDERLINE);
	nccell_release(s.plane, &base);
	int y, x;
	ncplane_yx(s.plane, &y, &x);
	ck_assert(y == 2 && x == 3);
	rect = (teder_rectangle){0, 7, 1, 1};
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	cell(s.plane, 0, 7, " ", channels(0x33cc66, 0x33cc66), 0);
	rect = (teder_rectangle){0, 7, 6, 1}; /* one column aggregates both bands by maximum */
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	cell(s.plane, 0, 7, " ", channels(0x33cc66, 0x33cc66), 0);
	OK(teder_renderer_draw(r, NULL, s.plane, &rect));
	cell(s.plane, 5, 7, " ", channels(0x33cc66, 0x112233), 0);
	teder_snapshot_release(data);
	teder_renderer_destroy(r);
	screen_close(&s);
}
END_TEST

START_TEST(clipping)
{
	screen s = screen_open();
	fill(s.plane);
	th_tracker tracker;
	ck_assert(th_tracker_init(&tracker));
	teder_renderer_config c = config();
	teder_renderer *r;
	OK(td_renderer_create_with(&c, th_allocator(&tracker), &r));
	const teder_rectangle empty[] = {{0, 0, 0, UINT32_MAX},
	                                 {0, 0, 1, 0},
	                                 {INT32_MAX, INT32_MAX, UINT32_MAX, UINT32_MAX},
	                                 {INT32_MIN, INT32_MIN, 1, 1}};
	for (unsigned n = 0; n < sizeof(empty) / sizeof(empty[0]); n++)
		OK(teder_renderer_draw(r, NULL, s.plane, &empty[n]));
	unchanged(s.plane);
	teder_snapshot *data = snapshot(true);
	teder_rectangle rect = {-1, -1, 4, 4};
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 8; x++) {
			bool in = y < 3 && x < 3;
			cell(s.plane,
			     y,
			     x,
			     in ? " " : "!",
			     in ? channels(c.foreground_rgb, x ? c.foreground_rgb : c.background_rgb)
			        : channels(0xabcdef, 0x987654),
			     in ? 0 : NCSTYLE_BOLD);
		}
	ck_assert(ncplane_resize_simple(s.plane, 2, 3) == 0);
	rect = (teder_rectangle){INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX};
	OK(teder_renderer_draw(r, NULL, s.plane, &rect));
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 3; x++)
			cell(s.plane, y, x, " ", channels(c.foreground_rgb, c.background_rgb), 0);
	ck_assert(ncplane_dim_y(s.plane) == 2 && ncplane_dim_x(s.plane) == 3);
	ck_assert(teder_renderer_draw(r, NULL, NULL, &rect) == TEDER_INVALID);
	ck_assert(teder_renderer_draw(r, NULL, s.plane, NULL) == TEDER_INVALID);
	ck_assert(tracker.attempts == 1 && tracker.live == 1);
	teder_snapshot_release(data);
	teder_renderer_destroy(r);
	ck_assert(th_tracker_clean(&tracker));
	th_tracker_uninit(&tracker);
	screen_close(&s);
}
END_TEST

START_TEST(levels)
{
	screen s = screen_open();
	teder_renderer_config c = config();
	c.mode = TEDER_RENDER_LEVELS;
	c.floor_db = -24;
	teder_renderer *r;
	OK(teder_renderer_create(&c, &r));
	teder_snapshot *data = snapshot(false);
	teder_rectangle rect = {0, 0, 4, 2};
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	const unsigned expected[4][2] = {
	    {0x112233, 0x112233}, {0xff9900, 0x112233}, {0x33cc66, 0x112233}, {0x33cc66, 0xff9900}};
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 2; x++)
			cell(s.plane, y, x, " ", channels(c.foreground_rgb, expected[y][x]), 0);
	rect = (teder_rectangle){0, 2, 4, 1};
	OK(teder_renderer_draw(r, data, s.plane, &rect));
	cell(s.plane, 1, 2, " ", channels(c.foreground_rgb, c.peak_rgb), 0);
	c.mode = TEDER_RENDER_SPECTRUM;
	OK(teder_renderer_configure(r, &c));
	OK(teder_renderer_draw(r, data, s.plane, &rect)); /* meters have no spectrum data */
	cell(s.plane, 3, 2, " ", channels(c.foreground_rgb, c.background_rgb), 0);
	teder_snapshot_release(data);
	teder_renderer_destroy(r);
	screen_close(&s);
}
END_TEST

START_TEST(wide_boundaries)
{
	screen s = screen_open();
	teder_renderer_config c = config();
	teder_renderer *r;
	OK(teder_renderer_create(&c, &r));
	for (unsigned side = 0; side < 2; side++) {
		fill(s.plane);
		ck_assert(ncplane_putegc_yx(s.plane, 2, 2, "界", NULL) == 2);
		teder_rectangle rect = {1, side ? 1 : 3, 3, 2};
		ck_assert(teder_renderer_draw(r, NULL, s.plane, &rect) == TEDER_UNSUPPORTED);
		cell(s.plane, 1, rect.x, "!", channels(0xabcdef, 0x987654), NCSTYLE_BOLD);
		cell(s.plane, 2, 2, "界", channels(0xabcdef, 0x987654), NCSTYLE_BOLD);
	}
	teder_rectangle rect = {2, 2, 1, 2};
	OK(teder_renderer_draw(r, NULL, s.plane, &rect));
	cell(s.plane, 2, 2, " ", channels(c.foreground_rgb, c.background_rgb), 0);
	cell(s.plane, 2, 3, " ", channels(c.foreground_rgb, c.background_rgb), 0);
	ck_assert(ncplane_putegc_yx(s.plane, 0, 1, "é̈", NULL) == 1);
	rect = (teder_rectangle){0, 0, 1, 1};
	for (unsigned n = 0; n < 32; n++)
		OK(teder_renderer_draw(r, NULL, s.plane, &rect));
	cell(s.plane, 0, 1, "é̈", channels(0xabcdef, 0x987654), NCSTYLE_BOLD);
	teder_renderer_destroy(r);
	screen_close(&s);
}
END_TEST

START_TEST(resize_cut)
{
	/* Shrinking the plane cuts wide glyphs at its new last column. Drawing that column
	 * writes only inside it: the next row's first cell and the cells to its left keep
	 * their contents. */
	screen s = screen_open();
	teder_renderer_config c = config();
	teder_renderer *r;
	OK(teder_renderer_create(&c, &r));
	fill(s.plane);
	ck_assert(ncplane_putegc_yx(s.plane, 0, 6, "界", NULL) == 2);
	ck_assert(ncplane_putegc_yx(s.plane, 5, 6, "界", NULL) == 2);
	ck_assert(ncplane_resize_simple(s.plane, 6, 7) == 0);
	teder_rectangle rect = {0, 6, 6, 1};
	OK(teder_renderer_draw(r, NULL, s.plane, &rect));
	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 7; x++) {
			bool in = x == 6;
			cell(s.plane,
			     y,
			     x,
			     in ? " " : "!",
			     in ? channels(c.foreground_rgb, c.background_rgb) : channels(0xabcdef, 0x987654),
			     in ? 0 : NCSTYLE_BOLD);
		}
	ck_assert(ncplane_dim_y(s.plane) == 6 && ncplane_dim_x(s.plane) == 7);
	teder_renderer_destroy(r);
	screen_close(&s);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
#ifdef _WIN32
	char runtime[4096];
	DWORD length =
	    GetModuleFileNameA(GetModuleHandleW(L"libnotcurses-core.dll"), runtime, sizeof(runtime));
	if (!length || length == sizeof(runtime))
		return 2;
	printf("notcurses runtime: %s\n", runtime);
	FreeConsole();
#else
	Dl_info runtime;
	if (!dladdr((void *)(uintptr_t)notcurses_core_init, &runtime))
		return 2;
	printf("notcurses runtime: %s\n", runtime.dli_fname);
	if (setsid() < 0)
		return 2;
#endif
	const struct {
		const char *name;
		const TTest *test;
	} cases[] = {{"bounds", bounds},
	             {"cells", spectrum_cells},
	             {"clipping", clipping},
	             {"levels", levels},
	             {"wide", wide_boundaries},
	             {"resize-cut", resize_cut}};
	for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		if (strcmp(argv[1], cases[n].name))
			continue;
		Suite *suite = suite_create("renderer");
		TCase *tc = tcase_create(cases[n].name);
		tcase_add_test(tc, cases[n].test);
		suite_add_tcase(suite, tc);
		SRunner *runner = srunner_create(suite);
		srunner_set_fork_status(runner, CK_NOFORK);
		srunner_run_all(runner, CK_NORMAL);
		int failed = srunner_ntests_failed(runner);
		srunner_free(runner);
		return failed ? 1 : 0;
	}
	return 2;
}
