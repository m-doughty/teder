#include "support.h"
#include "source.h"
#include <math.h>

/* Documented hostile-input outcomes. The tracking allocator never returns NULL,
 * so NO_MEMORY is a finding; INVALID/INTERNAL/BAD_STATE expose adapter faults.
 * A generated seed with exact expectations in known-good.txt must instead succeed. */
static bool rejected(const fz_input *f, teder_code code)
{
	if (code == TEDER_CANCELLED)
		return atomic_load(&f->cancel) != 0;
	return code == TEDER_CORRUPT || code == TEDER_UNSUPPORTED || code == TEDER_IO ||
	       code == TEDER_LIMIT;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fz_input f;
	if (!fz_begin(&f, data, size))
		return 0;
	fz_expect known;
	bool exact = fz_expect_find("source", data, size, &known);
	static const uint32_t rates[] = {0, 44100, 48000, 8000, 96000, 1, 1048575};
	td_source_config c = {.generation = 17,
	                      .source = 29,
	                      .track = 41,
	                      .output_start = 101,
	                      .output_rate = rates[f.control[0] % 7],
	                      .read_size = (f.control[1] & 4) ? 1 : 4096,
	                      .codec_bytes = (f.control[1] & 8) ? 4096 : 4 * 1024 * 1024,
	                      .boundaries = true,
	                      .cancel = &f.cancel,
	                      .context = &f,
	                      .checkpoint = fz_checkpoint};
	uint32_t bytes = (uint32_t)strlen(f.path);
	FZ_CHECK(td_source_plan(bytes, c.codec_bytes, &c.reservation) == TEDER_OK);
	td_source *s = NULL;
	teder_code code = td_source_open(f.path, bytes, &c, fz_allocator(&f), &s);
	if (code != TEDER_OK) {
		FZ_CHECK(!s && !exact && rejected(&f, code));
		fz_end(&f);
		return 0;
	}
	FZ_CHECK(s && s->info.channels >= 1 && s->info.channels <= 8 && s->info.sample_rate);
	if (exact)
		FZ_CHECK(s->info.channels == fz_expect_value(&known, "channels") &&
		         s->info.sample_rate == fz_expect_value(&known, "rate") &&
		         s->info.bits_per_sample == fz_expect_value(&known, "bits") &&
		         s->info.duration_known &&
		         s->info.duration_frames == fz_expect_value(&known, "duration"));
	unsigned seek = f.control[1] & 3;
	if (seek) {
		uint64_t frame = seek == 1   ? 0
		                 : seek == 2 ? s->info.duration_frames / 2
		                             : fz_u32(f.control + 8);
		code = td_source_seek(s, frame);
		/* Only an arbitrary target may lie beyond a known duration. */
		bool beyond = s->info.duration_known && frame > s->info.duration_frames;
		FZ_CHECK(code == TEDER_OK || (!exact && (rejected(&f, code) ||
		                                         (code == TEDER_INVALID && seek == 3 && beyond))));
	}
	if (code == TEDER_OK && (f.control[4] & 1)) {
		for (unsigned i = 0; i < 16; i++) {
			code = td_source_prime(s);
			if (code != TEDER_AGAIN)
				break;
		}
		FZ_CHECK(code == TEDER_OK || code == TEDER_AGAIN || (!exact && rejected(&f, code)));
	}
	bool pumped = code == TEDER_OK || code == TEDER_AGAIN;
	size_t capacity = 0;
	td_pcm_layout layout;
	FZ_CHECK(td_pcm_plan(&capacity, 2048, &layout));
	td_arena *arena = td_arena_create(fz_allocator(&f), capacity);
	FZ_CHECK(arena);
	td_pcm_pool pool;
	td_pcm_init(&pool, td_arena_alloc(arena, capacity, 1, _Alignof(max_align_t)), &layout, 2048);
	FZ_CHECK(td_pcm_select(&pool, 17) == TEDER_OK);
	uint64_t frames = 0, digest = FZ_HASH_INIT;
	bool final = false;
	for (unsigned i = 0; i < 128 && (code == TEDER_OK || code == TEDER_AGAIN); i++) {
		code = td_source_pump(s, &pool);
		td_pcm_view v;
		while (td_pcm_peek(&pool, &v) == TEDER_OK) {
			FZ_CHECK(!final && v.generation->generation == 17 && v.generation->source == 29 &&
			         v.generation->track == 41 && v.meta->output_start + v.offset == 101 + frames);
			FZ_CHECK(v.frames <= TD_PCM_BLOCK_FRAMES && v.meta->source_start >= s->start);
			for (uint32_t j = 0; j < v.frames * 2; j++) {
				FZ_CHECK(isfinite(v.samples[j]));
				digest = fz_hash_f32(digest, v.samples[j]);
			}
			frames += v.frames;
			final = (v.meta->flags & TD_PCM_FINAL) != 0;
			FZ_CHECK(td_pcm_consume(&pool, v.frames) == TEDER_OK);
		}
	}
	FZ_CHECK(!pumped || code == TEDER_OK || code == TEDER_AGAIN || code == TEDER_CLOSED ||
	         (!exact && rejected(&f, code)));
	if (code == TEDER_CLOSED) {
		uint64_t expected;
		FZ_CHECK(td_rate_frames(
		    s->decoded - s->start, s->config.output_rate, s->info.sample_rate, true, &expected));
		FZ_CHECK(frames == expected);
	}
	if (exact) {
		/* The whole stream reaches its final block with the independently derived frame
		 * count, and the converter-free streams carry exactly the expected samples. */
		FZ_CHECK(code == TEDER_CLOSED && final && frames == fz_expect_value(&known, "frames"));
		FZ_CHECK(!fz_expect_has(&known, "pcm") || digest == fz_expect_value(&known, "pcm"));
	}
	size_t pool_bytes;
	FZ_CHECK(td_arena_size(capacity, &pool_bytes) && f.peak <= c.reservation + pool_bytes);
	td_pcm_clear(&pool);
	FZ_CHECK(td_source_retire(&s) == TEDER_OK && !s);
	td_arena_destroy(arena);
	fz_end(&f);
	if (exact)
		fz_expect_passed(&known);
	return 0;
}
