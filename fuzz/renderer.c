#include "support.h"
#include "engine.h"
#include "analysis_pool.h"
#include "renderer.h"
#include <notcurses/notcurses.h>
#include <notcurses/version.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <unistd.h>

/* One headless notcurses context per process. Each input owns a fresh child plane,
 * renderer and snapshot pool, with one counting heap for each library owner.
 * Header, little-endian:
 *   0 rows-1 (mod 48)  1 columns-1 (mod 96)  2 origin (y low nibble, x high nibble)
 *   3 fill glyph (mod 9; 8 leaves cells empty)  4-13 fill style  14-29 config
 *   30 bit 0: creation fails with NO_MEMORY  31 loans 1+(b&3), bit 2: publish 32-54
 *   and acquire it
 * Then up to 64 operations: an opcode byte (mod 16; 13-15 also draw) and its fixed
 * operand record (operands[]); missing bytes read as zero. Shared records:
 *   style   stylemask (2), foreground and background channels (4 each, legal bits)
 *   config  fault (low nibble: 8 size, 9 ABI, 10 NULL, 11-13 fg/bg/peak alpha, 14
 *           reserved, 15 mode, else none; high nibble picks the bad value), mode mod 3,
 *           fg/bg/peak RGB (3 each), floor (< 0xe0 table index, else the raw bits next)
 *   publish band count (2, mod 257), band generator, seed (4), rms[2] and peak[2] bits
 *   draw    targets (renderer/plane/rect 2 bits each, 3 = NULL; top bits: no snapshot or
 *           the newest/oldest/middle loan), field modes (near/raw/extreme/near), y, x,
 *           rows, columns
 *   put     glyph (low 7 bits; bit 7 writes at the cursor after moving it to y, x when
 *           both are on the plane), y (mod rows+2), x (mod columns+2), style
 */
enum { RH_HEADER = 55, RH_OPS = 64, RH_RECORD = 24 /* opcode and largest record */ };
enum { RH_MAX_INPUT = RH_HEADER + RH_OPS * RH_RECORD, RH_GLYPH = 32, RH_BANDS = 256, RH_LOANS = 4 };
/* Creation and resize bounds; one autogrowing put adds at most a row or two columns. */
enum { RH_ROWS = 48, RH_COLUMNS = 96 };
enum { RH_MAX_ROWS = RH_ROWS + RH_OPS, RH_MAX_COLUMNS = RH_COLUMNS + 2 * RH_OPS };
enum { OP_DRAW, OP_PUT, OP_BASE, OP_STYLE, OP_FLAGS, OP_RESIZE, OP_PUBLISH, OP_ACQUIRE };
enum { OP_RELEASE = OP_ACQUIRE + 1, OP_DISOWN, OP_CONFIGURE, OP_RECREATE, OP_BUILD_INFO, OP_COUNT };
static const uint8_t operands[OP_COUNT] = {18, 13, 11, 10, 1, 2, 23, 0, 1, 0, 17, 17, 1};

static const char *const glyphs[] = {
    "!",
    "a",
    "\xe7\x95\x8c", /* U+754C, two columns */
    ("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
     "\xf0\x9f\x91\xa6"), /* ZWJ family, stored in the plane's glyph pool */
    "e\xcc\x81\xcc\x88",  /* combining sequence, also pooled */
    "\xf0\x9f\x8e\xb5",   /* U+1F3B5, inline */
    "\xd7\xa9",
    " "};
static const float floors[] = {
    0, -0.0f, -90, -1, -24, -FLT_TRUE_MIN, -FLT_MAX, NAN, INFINITY, -INFINITY, 1, FLT_MAX};
static const float extremes[] = {
    -FLT_MAX, FLT_MAX, 0, -0.0f, -90, -27, -45, -FLT_TRUE_MIN, FLT_TRUE_MIN, 1e30f, 120, -0.5f};
enum { RH_GLYPHS = sizeof(glyphs) / sizeof(*glyphs), RH_FLOORS = sizeof(floors) / sizeof(*floors) };
enum { RH_EXTREMES = sizeof(extremes) / sizeof(*extremes) };

typedef struct {
	char glyph[RH_GLYPH];
	uint64_t channels;
	uint16_t stylemask;
	uint8_t bytes;
	bool wide; /* right-hand continuation of a wide glyph */
} rh_cell;

typedef struct {
	unsigned rows, columns;
	int y, x;
	uint64_t channels;
	uint16_t styles;
	bool scrolling, autogrow;
	rh_cell base;
} rh_shape;

typedef struct {
	float bands[RH_BANDS], rms[2], peak[2];
	uint32_t count;
} rh_publication;

typedef struct {
	uint32_t fill, peak;
	bool fill_tie, peak_tie;
} rh_bar;

static struct {
	const uint8_t *at, *end;
	fz_heap renderer_heap, pool_heap;
	teder_renderer *renderer;
	teder_renderer_config config; /* the renderer's effective configuration */
	td_snapshot_pool *pool;
	const rh_publication *current;
	teder_snapshot *loans[RH_LOANS]; /* oldest first */
	const rh_publication *loaned[RH_LOANS];
	unsigned loan_count, loan_limit, publications;
	struct ncplane *plane;
	char scene[192]; /* the operation being checked */
} g;
static rh_publication publications[RH_OPS + 1];
static rh_cell before[RH_MAX_ROWS * RH_MAX_COLUMNS], after[RH_MAX_ROWS * RH_MAX_COLUMNS];
static struct notcurses *nc;
static FILE *output;
static teder_build_info reference;
static bool trace; /* TEDER_FUZZ_TRACE=1 logs every operation and draw outcome */

/* Evaluates once; failures name the operation and condition even without tracing. */
#define RH_CHECK(condition) \
	do { \
		bool rh_passed = (condition); \
		if (!rh_passed) \
			fz_trace("%s: %s\n", g.scene, #condition); \
		FZ_CHECK(rh_passed); \
	} while (0)
#define RH_TRACE(...) \
	do { \
		if (trace) \
			fz_trace(__VA_ARGS__); \
	} while (0)

static void take(uint8_t *out, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++)
		out[i] = g.at < g.end ? *g.at++ : 0;
}

/* Optionally clearing the top exponent bit makes infinities and NaNs finite. */
static float as_float(uint32_t bits, bool finite)
{
	float value;
	memcpy(&value, &bits, sizeof(value));
	return !finite || isfinite(value) ? value : as_float(bits & ~UINT32_C(0x40000000), false);
}

static uint64_t style_channels(const uint8_t *b)
{
	return ncchannels_channels((uint64_t)fz_u32(b + 2) << 32 | fz_u32(b + 6));
}

static void set_style(const uint8_t *b)
{
	ncplane_set_styles(g.plane, (unsigned)(b[0] | b[1] << 8));
	ncplane_set_channels(g.plane, style_channels(b));
}

static size_t attempts(void)
{
	return g.renderer_heap.attempts + g.pool_heap.attempts;
}

/* A matching prefix gets the whole record; a mismatch gets only its 8-byte prefix,
 * so ASan rejects any access past what the contract allows. */
static void *sized(const void *record, size_t size)
{
	uint32_t prefix[2];
	memcpy(prefix, record, sizeof(prefix));
	size_t bytes = prefix[0] == size && prefix[1] == TEDER_ABI_VERSION ? size : sizeof(prefix);
	void *block = malloc(bytes);
	FZ_CHECK(block);
	memcpy(block, record, bytes);
	return block;
}

enum { RH_CONFIG = sizeof(teder_renderer_config), RH_INFO = sizeof(teder_build_info) };
static const uint32_t sizes[] = {
    0, 8, RH_CONFIG - 1, RH_CONFIG + 1, RH_INFO - 1, RH_INFO + 1, UINT32_MAX};
static const uint32_t abis[] = {0, TEDER_ABI_VERSION + 1, TEDER_ABI_VERSION + 256, UINT32_MAX};

static teder_renderer_config decode_config(const uint8_t *b, bool *null)
{
	unsigned fault = b[0] & 15, value = b[0] >> 4;
	uint32_t alpha = UINT32_C(0x01000000) << (value % 8);
	teder_renderer_config c = FZ_INIT(teder_renderer_config);
	c.mode = fault == 15 ? (value == 15 ? UINT32_MAX : 3 + value) : b[1] % 3u;
	c.foreground_rgb = (fz_u32(b + 2) & 0xffffff) | (fault == 11 ? alpha : 0);
	c.background_rgb = (fz_u32(b + 5) & 0xffffff) | (fault == 12 ? alpha : 0);
	c.peak_rgb = (fz_u32(b + 8) & 0xffffff) | (fault == 13 ? alpha : 0);
	c.floor_db = b[11] < 0xe0 ? floors[b[11] % RH_FLOORS] : as_float(fz_u32(b + 12), false);
	c.reserved = fault == 14 ? UINT32_C(1) << (2 * value) : 0;
	c.struct_size = fault == 8 ? sizes[value % 7] : c.struct_size;
	c.abi_version = fault == 9 ? abis[value % 4] : c.abi_version;
	*null = fault == 10;
	return c;
}

/* docs/renderer.md "Configuration and ownership"; *resolved gets effective settings. */
static teder_code expect_config(const teder_renderer_config *c, teder_renderer_config *resolved)
{
	if (!c)
		return TEDER_INVALID;
	if (c->struct_size != sizeof(*c) || c->abi_version != TEDER_ABI_VERSION)
		return TEDER_ABI_MISMATCH;
	*resolved = *c;
	resolved->mode = resolved->mode ? resolved->mode : TEDER_RENDER_SPECTRUM;
	resolved->floor_db = resolved->floor_db == 0 ? -90 : resolved->floor_db;
	bool valid = resolved->mode == TEDER_RENDER_SPECTRUM || resolved->mode == TEDER_RENDER_LEVELS;
	valid = valid && isfinite(resolved->floor_db) && resolved->floor_db < 0 && !resolved->reserved;
	uint32_t colors = resolved->foreground_rgb | resolved->background_rgb | resolved->peak_rgb;
	return valid && !(colors & UINT32_C(0xff000000)) ? TEDER_OK : TEDER_INVALID;
}

/* Runs on a fresh renderer heap, so a successful create is its only allocation.
 * Flag 2 also probes a NULL output; flag 4 destroys NULL. */
static void create(const uint8_t *record, unsigned flags)
{
	bool null;
	teder_renderer_config c = decode_config(record, &null), resolved = {0};
	void *block = null ? NULL : sized(&c, sizeof(c));
	teder_code expected = expect_config(block ? &c : NULL, &resolved);
	td_allocator allocator = fz_heap_allocator(&g.renderer_heap);
	if (flags & 2)
		RH_CHECK(td_renderer_create_with(block, allocator, NULL) == TEDER_INVALID);
	if (flags & 4)
		teder_renderer_destroy(NULL);
	if (expected == TEDER_OK && g.renderer_heap.fail_at)
		expected = TEDER_NO_MEMORY;
	teder_renderer *out = (teder_renderer *)&g;
	teder_code code = td_renderer_create_with(block, allocator, &out);
	free(block);
	RH_TRACE("  create -> %d mode %u floor %a\n", code, resolved.mode, resolved.floor_db);
	RH_CHECK(code == expected && !out == (code != TEDER_OK));
	RH_CHECK(g.renderer_heap.live == (code == TEDER_OK) &&
	         g.renderer_heap.attempts == (code == TEDER_OK || code == TEDER_NO_MEMORY));
	g.renderer = out;
	if (code == TEDER_OK)
		g.config = resolved;
}

static void configure(const uint8_t *b)
{
	teder_renderer *r = (b[0] & 3) == 3 ? NULL : g.renderer;
	bool null;
	teder_renderer_config c = decode_config(b + 1, &null), resolved = {0};
	void *block = null ? NULL : sized(&c, sizeof(c));
	teder_code expected = r ? expect_config(block ? &c : NULL, &resolved) : TEDER_INVALID;
	size_t allocations = attempts();
	teder_code code = teder_renderer_configure(r, block);
	free(block);
	RH_TRACE("  configure %s -> %d\n", r ? "renderer" : "NULL", code);
	RH_CHECK(code == expected && attempts() == allocations);
	if (code == TEDER_OK)
		g.config = resolved; /* any failure keeps the previous configuration */
}

/* Selector 0-4 valid, 5 size, 6 ABI, 7 NULL; the upper bits pick the bad value. */
static void build_info(uint8_t b)
{
	teder_build_info info;
	memset(&info, 0xa5, sizeof(info));
	info.struct_size = (b & 7) == 5 ? sizes[(b >> 3) % 7] : sizeof(info);
	info.abi_version = (b & 7) == 6 ? abis[(b >> 3) % 4] : TEDER_ABI_VERSION;
	void *block = (b & 7) == 7 ? NULL : sized(&info, sizeof(info));
	bool exact = info.struct_size == sizeof(info) && info.abi_version == TEDER_ABI_VERSION;
	teder_code expected = !block ? TEDER_INVALID : exact ? TEDER_OK : TEDER_ABI_MISMATCH;
	size_t allocations = attempts();
	teder_code code = teder_renderer_get_build_info(block);
	RH_TRACE("  build info -> %d\n", code);
	RH_CHECK(code == expected && attempts() == allocations);
	if (block) /* the core's build information, or an untouched prefix */
		RH_CHECK(
		    !memcmp(block, code ? (void *)&info : (void *)&reference, code ? 8 : sizeof(info)));
	free(block);
}

static float band(uint8_t generator, uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	unsigned kind = generator % 4 == 3 ? *state >> 30 : generator % 4u;
	if (kind == 1)
		return as_float(*state, true);
	if (kind == 2)
		return extremes[*state % RH_EXTREMES];
	return -(float)(*state & 255) * .5f; /* half-dB steps from 0 to -127.5 */
}

static void publish(const uint8_t *b)
{
	rh_publication *p = &publications[g.publications];
	p->count = (uint32_t)(b[0] | b[1] << 8) % (RH_BANDS + 1);
	uint32_t state = fz_u32(b + 3) ? fz_u32(b + 3) : UINT32_C(0x9e3779b9);
	for (uint32_t i = 0; i < p->count; i++)
		p->bands[i] = band(b[2], &state);
	teder_snapshot_info info = FZ_INIT(teder_snapshot_info);
	for (unsigned c = 0; c < 2; c++) {
		info.rms[c] = p->rms[c] = as_float(fz_u32(b + 7 + 4 * c) & INT32_MAX, true);
		info.peak[c] = p->peak[c] = as_float(fz_u32(b + 15 + 4 * c) & INT32_MAX, true);
	}
	info.epoch = info.frames = info.config_revision = 1;
	info.output_rate = 48000;
	info.channels = 2;
	info.bands = p->count;
	info.span_count = 1;
	teder_span span = {0, 1, 0, 0};
	size_t allocations = attempts();
	teder_code code = td_snapshot_publish(g.pool, &info, p->count ? p->bands : NULL, &span);
	RH_TRACE("  publish %u bands -> %d\n", p->count, code);
	RH_CHECK(code == (g.pool ? TEDER_OK : TEDER_INVALID) && attempts() == allocations);
	if (code == TEDER_OK)
		g.current = &publications[g.publications++];
}

static void acquire(void)
{
	teder_snapshot *s = (teder_snapshot *)&g;
	teder_code code = td_snapshot_acquire(g.pool, &s);
	teder_code expected = !g.pool                        ? TEDER_INVALID
	                      : !g.current                   ? TEDER_AGAIN
	                      : g.loan_count == g.loan_limit ? TEDER_LIMIT
	                                                     : TEDER_OK;
	RH_TRACE("  acquire -> %d\n", code);
	RH_CHECK(code == expected && !s == (code != TEDER_OK));
	if (code != TEDER_OK)
		return;
	g.loaned[g.loan_count] = g.current;
	g.loans[g.loan_count++] = s;
}

static void release_loan(unsigned index)
{
	teder_snapshot_release(g.loans[index]);
	g.loan_count--;
	memmove(&g.loans[index], &g.loans[index + 1], (g.loan_count - index) * sizeof(*g.loans));
	memmove(&g.loaned[index], &g.loaned[index + 1], (g.loan_count - index) * sizeof(*g.loaned));
	RH_TRACE("  release loan %u, %u left\n", index, g.loan_count);
	RH_CHECK(g.pool_heap.live == (g.pool || g.loan_count));
}

/* Loans stay drawable after this. The arena lives while it has an owner or a loan. */
static void disown(void)
{
	td_snapshot_pool_release(&g.pool);
	RH_CHECK(!g.pool);
	g.current = NULL;
	RH_TRACE("  pool owner released, %u loans held\n", g.loan_count);
	RH_CHECK(g.pool_heap.live == (g.pool || g.loan_count));
}

__extension__ typedef unsigned __int128 rh_wide;

/* Sign of a * 2^ea - b * 2^eb for positive a and b below 2^126. */
static int compare(rh_wide a, int ea, rh_wide b, int eb)
{
	for (; ea > eb && !(a >> 126); ea--)
		a <<= 1;
	for (; eb > ea && !(b >> 126); eb--)
		b <<= 1;
	return ea != eb ? (ea > eb ? 1 : -1) : (a > b) - (a < b);
}

/* docs/renderer.md "Mapping" in exact arithmetic: fraction (db - floor) / -floor,
 * clamped to [0, 1], gives floor(fraction * rows + 1/2) cells. *tie reports that
 * fraction * rows + 1/2 is exactly that whole number. */
static uint32_t height(double db, float floor_db, uint32_t rows, bool *tie)
{
	*tie = false;
	if (db <= floor_db)
		return 0;
	if (db >= 0)
		return rows;
	int ef, ed;
	uint64_t f = (uint64_t)ldexp(frexp(-(double)floor_db, &ef), 53);
	uint64_t d = (uint64_t)ldexp(frexp(-db, &ed), 53), low = 0, high = rows;
	/* At least k cells exactly when (2 * rows - 2k + 1) * -floor >= 2 * rows * -db. */
	rh_wide scaled = (rh_wide)(2 * (uint64_t)rows) * d;
	int sign = 1;
	while (low < high) {
		uint64_t k = low + (high - low + 1) / 2;
		int s = compare((rh_wide)(2 * (uint64_t)rows - 2 * k + 1) * f, ef, scaled, ed);
		low = s >= 0 ? k : low;
		high = s >= 0 ? high : k - 1;
		sign = s >= 0 ? s : sign;
	}
	*tie = low && !sign;
	return (uint32_t)low;
}

/* The documented bar for a column of the original rectangle. */
static rh_bar bar(const rh_publication *p, uint64_t column, const teder_rectangle *r)
{
	rh_bar out = {0};
	if (p && g.config.mode == TEDER_RENDER_LEVELS) {
		unsigned channel = (unsigned)(2 * column / r->columns);
		bool both = r->columns == 1; /* one column takes each metric's channel maximum */
		double rms = both ? fmaxf(p->rms[0], p->rms[1]) : p->rms[channel];
		double peak = both ? fmaxf(p->peak[0], p->peak[1]) : p->peak[channel];
		/* Linear amplitude in dB is 20*log10(value); zero is the floor. */
		out.fill = rms > 0 ? height(20 * log10(rms), g.config.floor_db, r->rows, &out.fill_tie) : 0;
		out.peak =
		    peak > 0 ? height(20 * log10(peak), g.config.floor_db, r->rows, &out.peak_tie) : 0;
	} else if (p && p->count) {
		/* Bands from floor(x*B/columns) up to floor((x+1)*B/columns), at least one. */
		uint64_t first = column * p->count / r->columns;
		uint64_t end = (column + 1) * p->count / r->columns;
		float db = p->bands[first];
		for (uint64_t n = first + 1; n < end; n++)
			db = p->bands[n] > db ? p->bands[n] : db;
		out.fill = height(db, g.config.floor_db, r->rows, &out.fill_tie);
	}
	return out;
}

static void keep(nccell *c, rh_cell *out)
{
	const char *glyph = nccell_extended_gcluster(g.plane, c);
	size_t bytes = strlen(glyph);
	RH_CHECK(bytes < RH_GLYPH);
	memcpy(out->glyph, glyph, bytes);
	out->bytes = (uint8_t)bytes;
	out->stylemask = c->stylemask;
	out->channels = c->channels;
	out->wide = nccell_wide_right_p(c);
	nccell_release(g.plane, c);
}

static void capture(rh_shape *s, rh_cell *cells)
{
	ncplane_dim_yx(g.plane, &s->rows, &s->columns);
	RH_CHECK(s->rows <= RH_MAX_ROWS && s->columns <= RH_MAX_COLUMNS);
	ncplane_yx(g.plane, &s->y, &s->x);
	s->channels = ncplane_channels(g.plane);
	s->styles = ncplane_styles(g.plane);
	s->scrolling = ncplane_scrolling_p(g.plane);
	s->autogrow = ncplane_autogrow_p(g.plane);
	nccell c = {0};
	RH_CHECK(ncplane_base(g.plane, &c) >= 0);
	keep(&c, &s->base);
	for (unsigned y = 0; y < s->rows; y++)
		for (unsigned x = 0; x < s->columns; x++) {
			c = (nccell){0};
			RH_CHECK(ncplane_at_yx_cell(g.plane, (int)y, (int)x, &c) >= 0);
			keep(&c, &cells[(size_t)y * s->columns + x]);
		}
}

static bool same(const rh_cell *a, const rh_cell *b)
{
	return a->bytes == b->bytes && !memcmp(a->glyph, b->glyph, a->bytes) &&
	       a->stylemask == b->stylemask && a->channels == b->channels && a->wide == b->wide;
}

/* Whether column x of rows [y0, y1) shows exactly this bar and peak marker. */
static bool shows(unsigned columns, unsigned x, int64_t y0, int64_t y1, int64_t bottom, rh_bar e)
{
	for (int64_t y = y0; y < y1; y++) {
		const rh_cell *c = &after[(size_t)y * columns + x];
		uint64_t step = (uint64_t)(bottom - y); /* 1 in the rectangle's bottom row */
		uint32_t color = e.peak && step == e.peak ? g.config.peak_rgb
		                 : step <= e.fill         ? g.config.foreground_rgb
		                                          : g.config.background_rgb;
		uint64_t channels = 0; /* configured foreground, opaque RGB background */
		ncchannels_set_fg_rgb(&channels, g.config.foreground_rgb);
		ncchannels_set_bg_rgb(&channels, color);
		if (c->bytes != 1 || c->glyph[0] != ' ' || c->stylemask || c->wide ||
		    c->channels != channels)
			return false;
	}
	return true;
}

/* A rectangle field near the plane, raw, or extreme; origins are two's complement. */
static int64_t field(unsigned mode, const uint8_t *b, uint32_t d, bool origin)
{
	const uint32_t extreme[] = {
	    0x80000000u, 0x80000001u, UINT32_MAX, UINT32_MAX - 1, 0, 1, INT32_MAX, d, d + 1, 0u - d};
	uint32_t v = fz_u32(b);
	v = (mode & 3) == 1 ? v : (mode & 3) == 2 ? extreme[v % 10] : v % (d + 9) - (origin ? 4 : 0);
	return origin && v >> 31 ? (int64_t)v - (INT64_C(1) << 32) : (int64_t)v;
}

static void draw(const uint8_t *b)
{
	unsigned which = b[0] >> 6;
	teder_renderer *r = (b[0] & 3) == 3 ? NULL : g.renderer;
	struct ncplane *plane = (b[0] >> 2 & 3) == 3 ? NULL : g.plane;
	bool no_rect = (b[0] >> 4 & 3) == 3;
	unsigned loan = which == 1 ? g.loan_count - 1 : which == 2 ? 0 : g.loan_count / 2;
	teder_snapshot *snapshot = which && g.loan_count ? g.loans[loan] : NULL;
	const rh_publication *p = snapshot ? g.loaned[loan] : NULL;
	rh_shape s, now;
	capture(&s, before);
	teder_rectangle rect = {(int32_t)field(b[1], b + 2, s.rows, true),
	                        (int32_t)field(b[1] >> 2, b + 6, s.columns, true),
	                        (uint32_t)field(b[1] >> 4, b + 10, s.rows, false),
	                        (uint32_t)field(b[1] >> 6, b + 14, s.columns, false)};
	/* Documented order: renderer, plane and rectangle, empty area, cut wide glyphs. */
	teder_code expected = !r || !plane || no_rect ? TEDER_INVALID : TEDER_OK;
	int64_t y0 = rect.y > 0 ? rect.y : 0, x0 = rect.x > 0 ? rect.x : 0;
	int64_t bottom = (int64_t)rect.y + rect.rows, right = (int64_t)rect.x + rect.columns;
	int64_t y1 = bottom < s.rows ? bottom : s.rows, x1 = right < s.columns ? right : s.columns;
	bool painting = expected == TEDER_OK && rect.rows && rect.columns && y0 < y1 && x0 < x1;
	for (int64_t y = y0; painting && y < y1; y++) {
		const rh_cell *row = &before[(size_t)y * s.columns];
		if (row[x0].wide || (x1 < s.columns && row[x1].wide)) {
			expected = TEDER_UNSUPPORTED;
			painting = false;
		}
	}
	size_t n = strlen(g.scene);
	snprintf(g.scene + n,
	         sizeof(g.scene) - n,
	         ": %02x {%" PRId32 ",%" PRId32 ",%" PRIu32 ",%" PRIu32 "} on %ux%u mode %u bands %u",
	         b[0],
	         rect.y,
	         rect.x,
	         rect.rows,
	         rect.columns,
	         s.rows,
	         s.columns,
	         g.config.mode,
	         p ? p->count : 0);
	size_t allocations = attempts();
	teder_code code = teder_renderer_draw(r, snapshot, plane, no_rect ? NULL : &rect);
	RH_TRACE("  %s -> %d%s\n", g.scene, code, painting ? " painted" : "");
	RH_CHECK(code == expected && attempts() == allocations);
	capture(&now, after);
	/* Geometry, drawing state and base cell survive every call; the cursor need not. */
	RH_CHECK(now.rows == s.rows && now.columns == s.columns && now.y == s.y && now.x == s.x);
	RH_CHECK(now.channels == s.channels && now.styles == s.styles);
	RH_CHECK(now.scrolling == s.scrolling && now.autogrow == s.autogrow &&
	         same(&now.base, &s.base));
	for (unsigned x = 0; x < s.columns; x++) {
		bool drawn = painting && x >= x0 && x < x1;
		if (drawn) {
			rh_bar e = bar(p, (uint64_t)(x - (int64_t)rect.x), &rect), shown = e;
			/* The library rounds some exact halves down; accept only that deviation. */
			bool ok = shows(s.columns, x, y0, y1, bottom, e);
			for (unsigned alt = 1; !ok && alt < 4; alt++) {
				shown.fill = e.fill - (e.fill_tie && (alt & 1));
				shown.peak = e.peak - (e.peak_tie && (alt & 2));
				ok = shows(s.columns, x, y0, y1, bottom, shown);
			}
			if (!ok)
				fz_trace(
				    "column %u: %u %u ties %d %d\n", x, e.fill, e.peak, e.fill_tie, e.peak_tie);
			RH_CHECK(ok);
			if (shown.fill != e.fill || shown.peak != e.peak)
				RH_TRACE("  column %u shows an exact half rounded down\n", x);
		}
		for (unsigned y = 0; y < s.rows; y++) {
			size_t i = (size_t)y * s.columns + x;
			bool kept = (drawn && y >= y0 && y < y1) || same(&after[i], &before[i]);
			if (!kept)
				fz_trace("cell %u,%u changed\n", y, x);
			RH_CHECK(kept);
		}
	}
}

static void put(const uint8_t *b)
{
	unsigned rows, columns;
	ncplane_dim_yx(g.plane, &rows, &columns);
	set_style(b + 3);
	int y = (int)(b[1] % (rows + 2)), x = (int)(b[2] % (columns + 2));
	const char *glyph = glyphs[(b[0] & 0x7f) % RH_GLYPHS];
	bool cursor = b[0] & 0x80;
	int n;
	if (cursor) {
		/* Like text output: a glyph that doesn't fit wraps to the next line, and a
		 * scrolling plane scrolls from its last row. An off-plane y, x keeps the cursor. */
		(void)ncplane_cursor_move_yx(g.plane, y, x);
		n = ncplane_putegc(g.plane, glyph, NULL);
	} else
		n = ncplane_putegc_yx(g.plane, y, x, glyph, NULL);
	unsigned cy, cx;
	ncplane_cursor_yx(g.plane, &cy, &cx);
	RH_TRACE("  put glyph %u at %d,%d%s -> %d, cursor %u,%u\n",
	         (b[0] & 0x7f) % RH_GLYPHS,
	         y,
	         x,
	         cursor ? " (cursor)" : "",
	         n,
	         cy,
	         cx);
}

static void fill(uint8_t glyph, const uint8_t *style)
{
	unsigned rows, columns;
	ncplane_dim_yx(g.plane, &rows, &columns);
	set_style(style);
	for (unsigned y = 0; glyph % (RH_GLYPHS + 1) < RH_GLYPHS && y < rows; y++)
		for (unsigned x = 0; x < columns;) {
			int n =
			    ncplane_putegc_yx(g.plane, (int)y, (int)x, glyphs[glyph % (RH_GLYPHS + 1)], NULL);
			x += n > 0 ? (unsigned)n : 1;
		}
}

static void step(unsigned op, const uint8_t *b)
{
	int result = 0;
	if (op == OP_DRAW)
		draw(b);
	else if (op == OP_PUT)
		put(b);
	else if (op == OP_BASE)
		result = ncplane_set_base(
		    g.plane, glyphs[b[0] % RH_GLYPHS], (uint16_t)(b[1] | b[2] << 8), style_channels(b + 1));
	else if (op == OP_STYLE)
		set_style(b);
	else if (op == OP_FLAGS) {
		ncplane_set_scrolling(g.plane, b[0] & 1);
		ncplane_set_autogrow(g.plane, b[0] >> 1 & 1);
	} else if (op == OP_RESIZE)
		result = ncplane_resize_simple(g.plane, 1 + b[0] % RH_ROWS, 1 + b[1] % RH_COLUMNS);
	else if (op == OP_PUBLISH)
		publish(b);
	else if (op == OP_ACQUIRE)
		acquire();
	else if (op == OP_RELEASE && g.loan_count)
		release_loan(b[0] % g.loan_count);
	else if (op == OP_DISOWN)
		disown();
	else if (op == OP_CONFIGURE)
		configure(b);
	else if (op == OP_RECREATE) {
		teder_renderer_destroy(g.renderer);
		fz_heap_end(&g.renderer_heap);
		fz_heap_init(&g.renderer_heap, b[0] & 1);
		create(b + 1, b[0]);
	} else if (op == OP_BUILD_INFO)
		build_info(b[0]);
	if (op == OP_BASE || op == OP_RESIZE)
		RH_TRACE("  %s -> %d\n", op == OP_BASE ? "base" : "resize", result);
}

static void close_terminal(void)
{
	FZ_CHECK(notcurses_stop(nc) == 0);
	FZ_CHECK(fclose(output) == 0);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	/* Without a controlling terminal, notcurses cannot reach the invoking tty. */
	FZ_CHECK(setsid() >= 0 || errno == EPERM);
	output = fopen("/dev/null", "w");
	FZ_CHECK(output);
	notcurses_options options = {.termtype = "xterm-256color",
	                             .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN |
	                                      NCOPTION_NO_QUIT_SIGHANDLERS |
	                                      NCOPTION_NO_WINCH_SIGHANDLER | NCOPTION_DRAIN_INPUT};
	nc = notcurses_core_init(&options, output);
	if (!nc)
		fputs("notcurses core/terminfo initialization failed\n", stderr);
	FZ_CHECK(nc);
	FZ_CHECK(atexit(close_terminal) == 0);
	/* The loaded runtime is the pinned one these headers describe. */
	int major, minor, patch, tweak;
	notcurses_version_components(&major, &minor, &patch, &tweak);
	FZ_CHECK(major == NOTCURSES_VERNUM_MAJOR && minor == NOTCURSES_VERNUM_MINOR &&
	         patch == NOTCURSES_VERNUM_PATCH);
	reference = (teder_build_info)FZ_INIT(teder_build_info);
	FZ_CHECK(teder_get_build_info(&reference) == TEDER_OK);
	trace = getenv("TEDER_FUZZ_TRACE") != NULL;
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < RH_HEADER || size > RH_MAX_INPUT)
		return 0;
	memset(&g, 0, sizeof(g));
	g.at = data;
	g.end = data + size;
	uint8_t h[RH_HEADER], b[RH_RECORD - 1], op;
	take(h, sizeof(h));
	strcpy(g.scene, "setup");
	struct ncplane *std = notcurses_stdplane(nc);
	ncplane_options shape = {.y = h[2] & 15,
	                         .x = h[2] >> 4,
	                         .rows = 1u + h[0] % RH_ROWS,
	                         .cols = 1u + h[1] % RH_COLUMNS};
	g.plane = ncplane_create(std, &shape);
	RH_CHECK(g.plane);
	fill(h[3], h + 4);
	fz_heap_init(&g.pool_heap, 0);
	g.loan_limit = 1 + (h[31] & 3u);
	/* A spare slot means publication never waits for a loan. */
	teder_config c = FZ_INIT(teder_config);
	c.snapshot_slots = g.loan_limit + 1;
	c.snapshot_loans = g.loan_limit;
	c.max_bands = RH_BANDS;
	c.max_spans = 1;
	td_allocator pool = fz_heap_allocator(&g.pool_heap);
	RH_CHECK(td_snapshot_pool_create(&c, SIZE_MAX, pool, (td_runtime){0}, &g.pool) == TEDER_OK);
	RH_CHECK(g.pool_heap.live == (g.pool || g.loan_count));
	if (h[31] & 4) {
		publish(h + 32);
		acquire();
	}
	fz_heap_init(&g.renderer_heap, h[30] & 1);
	create(h + 14, 0);
	for (unsigned i = 0; i < RH_OPS && g.at < g.end; i++) {
		take(&op, 1);
		op = op % 16 < OP_COUNT ? op % 16 : OP_DRAW;
		take(b, operands[op]);
		snprintf(g.scene, sizeof(g.scene), "step %u op %u", i, op);
		RH_TRACE("%s\n", g.scene);
		step(op, b);
	}
	strcpy(g.scene, "teardown");
	while (g.loan_count)
		release_loan(g.loan_count - 1);
	disown();
	teder_renderer_destroy(g.renderer);
	RH_CHECK(!g.renderer_heap.live);
	RH_CHECK(ncplane_destroy(g.plane) == 0);
	RH_CHECK(ncpile_top(std) == std && ncpile_bottom(std) == std);
	fz_heap_end(&g.renderer_heap);
	fz_heap_end(&g.pool_heap);
	if (trace)
		fz_trace_flush();
	return 0;
}
