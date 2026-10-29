#include "support.h"
#include "engine.h"
#include "flac_heap.h"
#include "spectrum.h"
#include <FLAC/stream_encoder.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

/* One engine per input: an 8-byte configuration header, then at most 96 operations.
 * Every handle, result and loan is legal; only argument values are hostile. */
enum { SQ_HEADER = 8, SQ_MAX_INPUT = 4096, SQ_OPS = 96, SQ_OWNED = 16, SQ_KEY = 4096 };
enum { P_SHORT, P_LONG, P_MIXED, P_EMPTY, P_TEXT, P_DIRECTORY, P_MISSING, P_FILES, P_NONE = -1 };
enum { A_STALE = 1, A_BAD_STATE = 2, A_TOO_LATE = 4 };

/* Expected STREAMINFO for the generated tracks; text/directory/missing never succeed. */
static const teder_probe_info tracks[] = {
    {.sample_rate = 44100, .channels = 2, .bits_per_sample = 16, .duration_frames = 3000},
    {.sample_rate = 44100, .channels = 2, .bits_per_sample = 16, .duration_frames = 30000},
    {.sample_rate = 22050, .channels = 1, .bits_per_sample = 24, .duration_frames = 4000},
    {.sample_rate = 44100, .channels = 2, .bits_per_sample = 16, .duration_frames = 0}};
static char root[32], paths[P_FILES][64], oversized[32769];

typedef struct {
	const char *data;
	uint32_t bytes;
	teder_code expect;
	int file;
} sq_path;

typedef struct {
	uint64_t id;
	uint32_t op, state, kind;
	teder_code outcome;
	int file;
	bool terminal, taken, acked, failed_before;
} sq_request;

typedef struct {
	teder_result *result;
	uint64_t hash;
	uint32_t kind;
} sq_result;

typedef struct {
	teder_snapshot *snapshot;
	uint64_t hash;
} sq_loan;

typedef struct {
	const uint8_t *data;
	size_t left;
} sq_bytes;

typedef struct {
	fz_heap heap;
	fz_platform platform;
	teder_engine *engine;
	teder_config config; /* resolved limits */
	teder_status status; /* last observed */
	sq_request requests[SQ_OPS];
	sq_result results[SQ_OWNED];
	sq_loan loans[SQ_OWNED];
	uint8_t keys[2][SQ_KEY], scratch[SQ_KEY];
	uint64_t key_bytes[2];
	uint64_t last_id, last_event, last_snapshot;
	uint64_t deadline; /* monotonic end of this input's waiting budget */
	unsigned count, result_count, loan_count;
	bool closing, no_memory_seen;
} sq_state;

static sq_state q;
static bool trace; /* TEDER_FUZZ_TRACE=1 logs operations for failure triage */

static void
encode(const char *path, unsigned channels, unsigned bits, unsigned rate, unsigned frames)
{
	td_flac_heap heap;
	FZ_CHECK(td_flac_heap_init(&heap, td_libc_allocator(), 8 * 1024 * 1024) == TEDER_OK);
	td_flac_heap *previous = td_flac_scope(&heap);
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FZ_CHECK(e && FLAC__stream_encoder_set_streamable_subset(e, false) &&
	         FLAC__stream_encoder_set_channels(e, channels) &&
	         FLAC__stream_encoder_set_bits_per_sample(e, bits) &&
	         FLAC__stream_encoder_set_sample_rate(e, rate) &&
	         FLAC__stream_encoder_set_blocksize(e, 256) &&
	         FLAC__stream_encoder_set_total_samples_estimate(e, frames));
	FZ_CHECK(FLAC__stream_encoder_init_file(e, path, NULL, NULL) ==
	         FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	int32_t samples[256 * 2];
	for (unsigned frame = 0; frame < frames;) {
		unsigned count = frames - frame < 256 ? frames - frame : 256;
		for (unsigned i = 0; i < count * channels; i++)
			samples[i] = (int32_t)(((frame * channels + i) * 37u) % 2001u) - 1000;
		FZ_CHECK(FLAC__stream_encoder_process_interleaved(e, samples, count));
		frame += count;
	}
	FZ_CHECK(FLAC__stream_encoder_finish(e));
	FLAC__stream_encoder_delete(e);
	td_flac_scope(previous);
	FZ_CHECK(heap.error == TEDER_OK && !heap.live && !heap.bytes);
}

static void cleanup(void)
{
	for (int i = 0; i < P_FILES; i++)
		if (i != P_DIRECTORY && i != P_MISSING)
			unlink(paths[i]);
	rmdir(root);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	static const char *names[] = {
	    "short.flac", "long.flac", "mixed.flac", "empty.flac", "text.flac", NULL, "missing.flac"};
	strcpy(root, "/tmp/teder-seq-XXXXXX");
	FZ_CHECK(mkdtemp(root));
	for (int i = 0; i < P_FILES; i++) {
		int n = names[i] ? snprintf(paths[i], sizeof(paths[i]), "%s/%s", root, names[i])
		                 : snprintf(paths[i], sizeof(paths[i]), "%s", root);
		FZ_CHECK(n > 0 && (size_t)n < sizeof(paths[i]));
	}
	for (int i = 0; i <= P_EMPTY; i++)
		encode(paths[i],
		       tracks[i].channels,
		       tracks[i].bits_per_sample,
		       tracks[i].sample_rate,
		       (unsigned)tracks[i].duration_frames);
	FILE *text = fopen(paths[P_TEXT], "wb");
	FZ_CHECK(text && fputs("not a FLAC stream\n", text) >= 0 && fclose(text) == 0);
	memset(oversized, 'a', sizeof(oversized));
	oversized[0] = '/';
	FZ_CHECK(atexit(cleanup) == 0);
	trace = getenv("TEDER_FUZZ_TRACE") != NULL;
	return 0;
}

static uint64_t now(void)
{
	struct timespec t;
	FZ_CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void pause_us(unsigned microseconds)
{
	struct timespec t = {0, (long)microseconds * 1000};
	nanosleep(&t, NULL);
}

static uint8_t next(sq_bytes *b)
{
	if (!b->left)
		return 0;
	b->left--;
	return *b->data++;
}

static uint64_t mix(uint64_t h, const void *data, size_t bytes)
{
	const unsigned char *p = data;
	for (size_t i = 0; i < bytes; i++)
		h = (h ^ p[i]) * UINT64_C(1099511628211);
	return h;
}

/* Empty views have NULL data and zero length; nonempty views have data. */
static uint64_t view(uint64_t h, const void *data, uint64_t bytes)
{
	FZ_CHECK(!bytes == !data);
	h = mix(h, &bytes, sizeof(bytes));
	return mix(h, data, (size_t)bytes);
}

static unsigned outstanding(void)
{
	unsigned n = 0;
	for (unsigned i = 0; i < q.count; i++)
		n += !q.requests[i].acked;
	return n;
}

static sq_request *find(uint64_t id)
{
	for (unsigned i = 0; i < q.count; i++)
		if (q.requests[i].id == id)
			return &q.requests[i];
	return NULL;
}

/* Documented status relations, monotonic counters and the engine byte budget. */
static void observe(void)
{
	teder_status s = FZ_INIT(teder_status);
	teder_code code;
	while ((code = teder_get_status(q.engine, &s)) == TEDER_AGAIN)
		sched_yield();
	FZ_CHECK(code == TEDER_OK && s.struct_size == sizeof(s));
	if (trace)
		fz_trace("  status lifecycle %u transport %u output %u source %" PRIu64 " epoch %" PRIu64
		         " media %" PRIu64 " next %u frame %" PRIu64 " underrun %" PRIu64
		         " rendered %" PRIu64 " credit %u\n",
		         s.lifecycle,
		         s.transport,
		         s.output_state,
		         s.current.source,
		         s.current.epoch,
		         s.media_request,
		         s.next_state,
		         s.output_frame,
		         s.underrun_frames,
		         (uint64_t)atomic_load(&q.platform.rendered),
		         atomic_load(&q.platform.credit));
	if (!q.closing)
		FZ_CHECK(s.lifecycle == TEDER_LIVE);
	FZ_CHECK(s.lifecycle >= q.status.lifecycle && s.lifecycle <= TEDER_CLOSED_STATE);
	FZ_CHECK(s.transport >= TEDER_EMPTY && s.transport <= TEDER_FAILED);
	FZ_CHECK(s.output_state <= TEDER_OUTPUT_LOST && s.next_state <= TEDER_NEXT_COMMITTED);
	FZ_CHECK((s.current.source == 0) == (s.transport == TEDER_EMPTY));
	if (s.transport == TEDER_EMPTY)
		FZ_CHECK(!s.track && !s.source_frame && !s.source_rate);
	FZ_CHECK(s.current.epoch >= 1 && s.current.epoch >= q.status.current.epoch);
	if (s.current.epoch == q.status.current.epoch)
		FZ_CHECK(s.output_frame >= q.status.output_frame &&
		         s.underrun_frames >= q.status.underrun_frames);
	FZ_CHECK(s.event_sequence >= q.status.event_sequence && s.event_sequence >= q.last_event);
	FZ_CHECK(s.events_dropped >= q.status.events_dropped);
	FZ_CHECK(s.analysis_sequence >= q.status.analysis_sequence);
	FZ_CHECK(isfinite(s.gain) && s.gain >= 0 && s.gain <= 1 && s.mute <= 1);
	FZ_CHECK(s.media_request <= q.last_id && s.source_channels <= 8);
	FZ_CHECK(!s.output_channels || s.output_channels == 2);
	q.status = s;
	td_lock(q.engine);
	size_t owned = q.engine->owned_bytes;
	td_unlock(q.engine);
	FZ_CHECK(owned <= q.config.engine_bytes_limit);
}

/* The fake backend allocates only at construction/enumeration, whose failures
 * close poisoned contexts. Source/preparation errors belong to one request or
 * become asynchronous stream failures; fresh requests cannot inherit them. */
static void outcome(const sq_request *r)
{
	teder_code c = r->outcome;
	FZ_CHECK(c <= TEDER_OK && c != TEDER_BUSY && c != TEDER_INTERNAL && c != TEDER_ABI_MISMATCH &&
	         c != TEDER_BUFFER_TOO_SMALL);
	if (c == TEDER_NO_MEMORY) {
		FZ_CHECK(fz_heap_failed(&q.heap) && !r->failed_before && !q.no_memory_seen);
		q.no_memory_seen = true; /* inspect calls outcome only on the first terminal read */
	}
	bool bad_file = r->file >= P_TEXT;
	if (bad_file)
		FZ_CHECK(c != TEDER_OK);
	switch (r->op) {
	case TEDER_OP_OPEN:
	case TEDER_OP_PREPARE:
		FZ_CHECK(c != TEDER_INVALID && c != TEDER_ENCODING);
		break;
	case TEDER_OP_TRANSPORT:
		FZ_CHECK(c != TEDER_INVALID && c != TEDER_ENCODING && c != TEDER_TOO_LATE);
		break;
	case TEDER_OP_SEEK:
	case TEDER_OP_SELECT_DEVICE:
		FZ_CHECK(c != TEDER_ENCODING && c != TEDER_TOO_LATE);
		break;
	case TEDER_OP_GAIN:
		FZ_CHECK(c == TEDER_OK || c == TEDER_CANCELLED || c == TEDER_DEVICE_LOST);
		break;
	case TEDER_OP_CLEAR_NEXT:
		FZ_CHECK(c == TEDER_OK || c == TEDER_CANCELLED || c == TEDER_STALE || c == TEDER_TOO_LATE);
		break;
	case TEDER_OP_PROBE:
		FZ_CHECK(c != TEDER_STALE && c != TEDER_BAD_STATE && c != TEDER_TOO_LATE &&
		         c != TEDER_INVALID && c != TEDER_DEVICE_LOST);
		break;
	case TEDER_OP_DEVICES:
		FZ_CHECK(c != TEDER_STALE && c != TEDER_BAD_STATE && c != TEDER_TOO_LATE &&
		         c != TEDER_INVALID && c != TEDER_ENCODING && c != TEDER_CORRUPT && c != TEDER_IO);
		break;
	case TEDER_OP_ANALYSIS:
		FZ_CHECK(c == TEDER_OK || c == TEDER_CANCELLED || c == TEDER_LIMIT || c == TEDER_NO_MEMORY);
		break;
	}
}

/* Request records only advance; a terminal outcome and result kind are immutable. */
static void inspect(sq_request *r, const teder_request_info *info)
{
	FZ_CHECK(info->struct_size == sizeof(*info) && info->abi_version == TEDER_ABI_VERSION);
	FZ_CHECK(info->request == r->id && info->operation == r->op);
	FZ_CHECK(info->state >= TEDER_QUEUED && info->state <= TEDER_TERMINAL &&
	         info->state >= r->state);
	FZ_CHECK(info->detail_bytes < sizeof(info->detail) && !info->detail[info->detail_bytes]);
	FZ_CHECK(!memchr(info->detail, 0, info->detail_bytes) && info->detail_truncated <= 1);
	FZ_CHECK(td_utf8((const unsigned char *)info->detail, info->detail_bytes));
	r->state = info->state;
	if (info->state != TEDER_TERMINAL)
		return;
	if (r->terminal) {
		FZ_CHECK(info->outcome == r->outcome && info->result_kind == r->kind);
		return;
	}
	r->terminal = true;
	r->outcome = info->outcome;
	r->kind = info->result_kind;
	if (trace)
		fz_trace("  request %" PRIu64 " op %u terminal %d\n", r->id, r->op, r->outcome);
	uint32_t kind = r->outcome != TEDER_OK      ? TEDER_RESULT_NONE
	                : r->op == TEDER_OP_PROBE   ? TEDER_RESULT_PROBE
	                : r->op == TEDER_OP_DEVICES ? TEDER_RESULT_DEVICES
	                                            : TEDER_RESULT_NONE;
	FZ_CHECK(r->kind == kind);
	outcome(r);
}

static teder_code get(uint64_t id, teder_request_info *info)
{
	teder_code code;
	*info = (teder_request_info)FZ_INIT(teder_request_info);
	while ((code = teder_request_get(q.engine, id, info)) == TEDER_AGAIN)
		sched_yield();
	return code;
}

static void refresh(sq_request *r)
{
	teder_request_info info;
	teder_code code = get(r->id, &info);
	if (r->acked)
		FZ_CHECK(code == TEDER_NOT_FOUND);
	else {
		FZ_CHECK(code == TEDER_OK);
		inspect(r, &info);
	}
}

/* expect is OK for valid input, otherwise the exact rejection code. */
static void admit(uint32_t op,
                  teder_code expect,
                  unsigned allowed,
                  teder_code code,
                  uint64_t id,
                  int file,
                  bool failed_before)
{
	if (trace)
		fz_trace("  submit op %u expect %d code %d id %" PRIu64 "\n", op, expect, code, id);
	if (expect != TEDER_OK) {
		FZ_CHECK(code == expect && !id);
		return;
	}
	if (code != TEDER_OK) {
		FZ_CHECK(!id);
		FZ_CHECK(code == TEDER_BUSY ||
		         (code == TEDER_LIMIT && outstanding() == q.config.request_capacity) ||
		         (code == TEDER_STALE && (allowed & A_STALE)) ||
		         (code == TEDER_BAD_STATE && (allowed & A_BAD_STATE)) ||
		         (code == TEDER_TOO_LATE && (allowed & A_TOO_LATE)));
		return;
	}
	FZ_CHECK(id > q.last_id && q.count < SQ_OPS && outstanding() < q.config.request_capacity);
	q.last_id = id;
	q.requests[q.count++] = (sq_request){
	    .id = id, .op = op, .state = TEDER_QUEUED, .file = file, .failed_before = failed_before};
}

static sq_path path(uint8_t s)
{
	uint32_t limit = q.config.max_input_bytes < 32767 ? q.config.max_input_bytes : 32767;
	unsigned kind = s % 12u;
	if (kind < P_FILES)
		return (sq_path){paths[kind], (uint32_t)strlen(paths[kind]), TEDER_OK, (int)kind};
	switch (kind) {
	case 7:
		return (sq_path){"short.flac", 10, TEDER_INVALID, P_NONE};
	case 8:
		return (sq_path){"/tmp/a\0b", 8, TEDER_INVALID, P_NONE};
	case 9:
		return (sq_path){"/tmp/\xc3\x28", 7, TEDER_ENCODING, P_NONE};
	case 10:
		return (sq_path){paths[P_SHORT], 0, TEDER_INVALID, P_NONE};
	default:
		return (sq_path){oversized, limit + 1, TEDER_LIMIT, P_NONE};
	}
}

static teder_target target(uint8_t s)
{
	teder_target t = q.status.current;
	if (s % 4 == 1)
		t.epoch++;
	if (s % 4 == 2)
		t.source++;
	if (s % 4 == 3)
		t = (teder_target){0};
	return t;
}

/* One valid template, optionally with exactly one documented violation. */
static teder_analysis_config analysis(sq_bytes *b, uint8_t s, teder_code *expect)
{
	teder_analysis_config c = FZ_INIT(teder_analysis_config);
	uint32_t bands = q.config.max_bands < TD_FFT_BANDS_MAX ? q.config.max_bands : TD_FFT_BANDS_MAX;
	uint8_t v = next(b), w = next(b);
	if (s & 3) {
		c.enabled = 1;
		c.window_frames = (uint32_t)TD_FFT_MIN << (w % 12);
		c.hop_frames = c.window_frames >> (w >> 6);
	}
	if ((s & 3) == 2) {
		c.bands = 1 + v % bands;
		c.low_hz = (s & 4) ? 50 : 0;
		c.high_hz = (s & 8) ? 16000 : 0;
		c.floor_db = (s & 16) ? -60 : 0;
	}
	if (s & 32)
		c.smoothing_ms = 100;
	if (s & 64)
		c.peak_decay_db_per_s = 12;
	*expect = TEDER_OK;
	if (!(s & 128))
		return c;
	if (!c.window_frames)
		c.window_frames = 2048;
	switch (v % 12) {
	case 0:
		c.reserved = 1;
		break;
	case 1:
		c.enabled = 2;
		break;
	case 2:
		c.low_hz = NAN;
		break;
	case 3:
		c.smoothing_ms = -1;
		break;
	case 4:
		c.peak_decay_db_per_s = INFINITY;
		break;
	case 5:
		c.window_frames = TD_FFT_MAX * 2;
		*expect = TEDER_LIMIT;
		return c;
	case 6:
		c.bands = bands + 1;
		*expect = TEDER_LIMIT;
		return c;
	case 7:
		c.bands = 1;
		c.window_frames = TD_FFT_MIN + 1;
		c.hop_frames = 0;
		break;
	case 8:
		c.hop_frames = c.window_frames + 1;
		break;
	case 9:
		c.bands = 0;
		c.hop_frames = 0;
		c.high_hz = 0;
		c.floor_db = 0;
		c.low_hz = 10;
		*expect = TEDER_UNSUPPORTED;
		return c;
	case 10:
		c.bands = 1;
		c.window_frames = 2048;
		c.hop_frames = 0;
		c.floor_db = 1;
		break;
	default:
		c.struct_size -= 4;
		*expect = TEDER_ABI_MISMATCH;
		return c;
	}
	*expect = TEDER_INVALID;
	return c;
}

static uint64_t digest(const teder_result *r, uint32_t expected)
{
	uint32_t kind = 0, count = 7;
	FZ_CHECK(teder_result_kind(r, &kind) == TEDER_OK && kind == expected);
	uint64_t h = mix(UINT64_C(14695981039346656037), &kind, sizeof(kind));
	teder_probe_info info = FZ_INIT(teder_probe_info);
	teder_text key, value;
	if (kind == TEDER_RESULT_PROBE) {
		FZ_CHECK(teder_result_device_count(r, &count) == TEDER_BAD_STATE && count == 7);
		FZ_CHECK(teder_result_probe(r, &info) == TEDER_OK && info.codec == TEDER_CODEC_FLAC);
		FZ_CHECK(teder_result_tag(r, 0, &key, &key) == TEDER_INVALID);
		h = mix(h, &info, sizeof(info));
		for (uint32_t i = 0; i < info.tag_count; i++) {
			FZ_CHECK(teder_result_tag(r, i, &key, &value) == TEDER_OK);
			h = view(view(h, key.data, key.bytes), value.data, value.bytes);
		}
		FZ_CHECK(teder_result_tag(r, info.tag_count, &key, &value) == TEDER_NOT_FOUND);
		teder_artwork art = FZ_INIT(teder_artwork);
		for (uint32_t i = 0; i < info.artwork_count; i++) {
			FZ_CHECK(teder_result_artwork(r, i, &art) == TEDER_OK);
			h = view(h, art.mime.data, art.mime.bytes);
			h = view(h, art.description.data, art.description.bytes);
			h = view(h, art.encoded.data, art.encoded.bytes);
		}
		return h;
	}
	FZ_CHECK(teder_result_probe(r, &info) == TEDER_BAD_STATE && !info.codec);
	FZ_CHECK(teder_result_device_count(r, &count) == TEDER_OK && count <= 128);
	h = mix(h, &count, sizeof(count));
	teder_device_info d = FZ_INIT(teder_device_info);
	for (uint32_t i = 0; i < count; i++) {
		FZ_CHECK(teder_result_device(r, i, &d) == TEDER_OK && d.is_default <= 1);
		FZ_CHECK(d.backend == TEDER_BACKEND_WASAPI && d.key.bytes <= q.config.max_input_bytes);
		h = view(
		    view(mix(h, &d.is_default, 4), d.name.data, d.name.bytes), d.key.data, d.key.bytes);
	}
	FZ_CHECK(teder_result_device(r, count, &d) == TEDER_NOT_FOUND);
	return h;
}

/* Taken results match the generated fixtures exactly and outlive the engine. */
static void keep(const sq_request *r, teder_result *result)
{
	uint64_t hash = digest(result, r->kind);
	if (r->kind == TEDER_RESULT_PROBE) {
		teder_probe_info info = FZ_INIT(teder_probe_info);
		FZ_CHECK(r->file >= P_SHORT && r->file <= P_EMPTY);
		FZ_CHECK(teder_result_probe(result, &info) == TEDER_OK);
		const teder_probe_info *t = &tracks[r->file];
		FZ_CHECK(info.sample_rate == t->sample_rate && info.channels == t->channels &&
		         info.bits_per_sample == t->bits_per_sample &&
		         info.duration_frames == t->duration_frames &&
		         info.duration_known == (t->duration_frames != 0) && !info.tag_count &&
		         !info.artwork_count);
	} else {
		uint32_t count = 0;
		FZ_CHECK(teder_result_device_count(result, &count) == TEDER_OK && count == 2);
		for (uint32_t i = 0; i < 2; i++) {
			teder_device_info d = FZ_INIT(teder_device_info);
			FZ_CHECK(teder_result_device(result, i, &d) == TEDER_OK && d.is_default == !i);
			FZ_CHECK(d.name.bytes == (i ? 4 : 5) &&
			         !memcmp(d.name.data, i ? "Beta" : "Alpha", d.name.bytes));
			FZ_CHECK(d.key.bytes <= SQ_KEY);
			memcpy(q.keys[i], d.key.data, (size_t)d.key.bytes);
			q.key_bytes[i] = d.key.bytes;
		}
	}
	q.results[q.result_count++] = (sq_result){result, hash, r->kind};
}

static void release_result(unsigned i)
{
	FZ_CHECK(digest(q.results[i].result, q.results[i].kind) == q.results[i].hash);
	teder_result_release(q.results[i].result);
	q.results[i] = q.results[--q.result_count];
}

static uint64_t snapshot_digest(const teder_snapshot *s, uint64_t *sequence)
{
	teder_snapshot_info info = FZ_INIT(teder_snapshot_info);
	const float *bands = NULL;
	const teder_span *spans = NULL;
	uint32_t band_count = 0, span_count = 0;
	FZ_CHECK(teder_snapshot_get_info(s, &info) == TEDER_OK && info.sequence);
	FZ_CHECK(teder_snapshot_bands(s, &bands, &band_count) == TEDER_OK && band_count == info.bands);
	FZ_CHECK(teder_snapshot_spans(s, &spans, &span_count) == TEDER_OK);
	FZ_CHECK(span_count == info.span_count && span_count <= q.config.max_spans);
	FZ_CHECK(band_count <= q.config.max_bands && info.channels == 2 && info.output_rate);
	for (unsigned c = 0; c < 2; c++)
		FZ_CHECK(isfinite(info.peak[c]) && isfinite(info.rms[c]) && info.peak[c] >= 0 &&
		         info.rms[c] >= 0);
	for (uint32_t i = 0; i < band_count; i++)
		FZ_CHECK(isfinite(bands[i]));
	*sequence = info.sequence;
	uint64_t h = mix(UINT64_C(14695981039346656037), &info, sizeof(info));
	h = view(h, bands, (uint64_t)band_count * sizeof(*bands));
	return view(h, spans, (uint64_t)span_count * sizeof(*spans));
}

static void release_loan(unsigned i)
{
	uint64_t sequence;
	FZ_CHECK(snapshot_digest(q.loans[i].snapshot, &sequence) == q.loans[i].hash);
	teder_snapshot_release(q.loans[i].snapshot);
	q.loans[i] = q.loans[--q.loan_count];
}

static uint64_t pick(sq_bytes *b, sq_request **r)
{
	uint8_t s = next(b);
	*r = NULL;
	if (q.count && s < 224) {
		*r = &q.requests[s % q.count];
		return (*r)->id;
	}
	return s & 1 ? 0 : q.last_id + 1 + (s & 7);
}

static void submit(sq_bytes *b, uint8_t op)
{
	/* The worker can fail before submission returns, so never sample in admit. */
	bool failed_before = fz_heap_failed(&q.heap);
	uint64_t id = 1;
	teder_code code, expect = TEDER_OK;
	uint8_t s = next(b);
	if (op == 0) {
		sq_path p = path(next(b));
		uint32_t flags = s & 1;
		expect = p.expect;
		if (expect == TEDER_OK && (s & 6) == 6) {
			flags |= 4;
			expect = TEDER_INVALID;
		}
		code = teder_open(q.engine, p.data, p.bytes, s >> 3, flags, &id);
		admit(TEDER_OP_OPEN, expect, 0, code, id, p.file, failed_before);
	} else if (op == 1) {
		teder_target t = target(next(b));
		uint32_t action = s % 5;
		expect = action >= TEDER_PLAY && action <= TEDER_STOP ? TEDER_OK : TEDER_INVALID;
		if (trace)
			fz_trace("  transport action %u epoch %" PRIu64 "\n", action, t.epoch);
		code = teder_transport(q.engine, &t, action, &id);
		admit(TEDER_OP_TRANSPORT, expect, A_STALE, code, id, P_NONE, failed_before);
	} else if (op == 2) {
		teder_target t = target(next(b));
		uint64_t d = q.status.duration_frames;
		uint64_t frames[] = {0, d / 2, d, d + 1, UINT64_C(1) << 36, (uint64_t)next(b) << 8};
		uint64_t frame = frames[s % 6];
		expect = frame >= (UINT64_C(1) << 36) ? TEDER_INVALID : TEDER_OK;
		code = teder_seek(q.engine, &t, frame, &id);
		admit(TEDER_OP_SEEK, expect, A_STALE | A_BAD_STATE, code, id, P_NONE, failed_before);
	} else if (op == 3) {
		static const float gains[] = {0, 0.25f, 1, -0.0f, 1.5f, -1, NAN, INFINITY};
		float gain = gains[s % 8];
		uint32_t mute = (s >> 3) % 3;
		expect = gain >= 0 && gain <= 1 && mute <= 1 ? TEDER_OK : TEDER_INVALID;
		code = teder_set_gain(q.engine, gain, mute, &id);
		admit(TEDER_OP_GAIN, expect, 0, code, id, P_NONE, failed_before);
	} else if (op == 4 || op == 5) {
		teder_target t = target(next(b));
		uint64_t expected[] = {0, q.status.next_source, q.status.current.source, 12345};
		if (op == 5) {
			code = teder_clear_next(q.engine, &t, expected[s % 4], &id);
			admit(TEDER_OP_CLEAR_NEXT,
			      TEDER_OK,
			      A_STALE | A_TOO_LATE,
			      code,
			      id,
			      P_NONE,
			      failed_before);
			return;
		}
		sq_path p = path(next(b));
		code = teder_prepare_next(q.engine, &t, expected[s % 4], p.data, p.bytes, s >> 2, &id);
		admit(TEDER_OP_PREPARE,
		      p.expect,
		      A_STALE | A_BAD_STATE | A_TOO_LATE,
		      code,
		      id,
		      p.file,
		      failed_before);
	} else if (op == 6) {
		sq_path p = path(next(b));
		uint32_t flags = p.expect == TEDER_OK ? s % 3 : s % 2;
		expect = flags > 1 ? TEDER_INVALID : p.expect;
		code = teder_probe(q.engine, p.data, p.bytes, flags, &id);
		admit(TEDER_OP_PROBE, expect, 0, code, id, p.file, failed_before);
	} else if (op == 7) {
		code = teder_enumerate_devices(q.engine, &id);
		admit(TEDER_OP_DEVICES, TEDER_OK, 0, code, id, P_NONE, failed_before);
	} else if (op == 8) {
		const uint8_t *key = NULL;
		uint64_t bytes = 0;
		unsigned which = s % 6;
		if ((which == 1 || which == 2) && q.key_bytes[which - 1]) {
			key = q.keys[which - 1];
			bytes = q.key_bytes[which - 1];
		} else if (which == 3) {
			bytes = q.key_bytes[0] ? q.key_bytes[0] : 16;
			memcpy(q.scratch, q.keys[0], (size_t)bytes);
			q.scratch[next(b) % bytes] ^= (uint8_t)(1 + (s >> 3));
			key = q.scratch;
		} else if (which == 4) {
			key = (const uint8_t *)oversized;
			bytes = (uint64_t)q.config.max_input_bytes + 1;
			expect = TEDER_LIMIT;
		} else if (which == 5) {
			bytes = 4;
			expect = TEDER_INVALID;
		}
		code = teder_select_device(q.engine, key, bytes, &id);
		admit(TEDER_OP_SELECT_DEVICE, expect, 0, code, id, P_NONE, failed_before);
	} else {
		teder_analysis_config c = analysis(b, s, &expect);
		code = teder_configure_analysis(q.engine, &c, &id);
		admit(TEDER_OP_ANALYSIS, expect, 0, code, id, P_NONE, failed_before);
	}
}

static void inquire(sq_bytes *b, uint8_t op)
{
	sq_request *r;
	uint64_t id = pick(b, &r);
	bool known = r && !r->acked;
	teder_request_info info;
	teder_code code;
	if (op == 10) {
		code = get(id, &info);
		FZ_CHECK(known ? code == TEDER_OK : code == TEDER_NOT_FOUND);
		if (known)
			inspect(r, &info);
		return;
	}
	if (known)
		refresh(r);
	if (op == 12) {
		code = teder_request_cancel(q.engine, id);
		if (trace)
			fz_trace("  cancel %" PRIu64 " code %d\n", id, code);
		if (!known)
			FZ_CHECK(code == TEDER_NOT_FOUND || code == TEDER_BUSY);
		else {
			FZ_CHECK(code == TEDER_OK || code == TEDER_TOO_LATE || code == TEDER_BUSY);
			FZ_CHECK(!r->terminal || code != TEDER_OK);
		}
	} else if (op == 13) {
		if (q.result_count == SQ_OWNED)
			release_result(0);
		teder_result *result = (teder_result *)&q;
		code = teder_request_take_result(q.engine, id, &result);
		FZ_CHECK(code == TEDER_OK ? result != NULL : result == NULL);
		if (!known) {
			FZ_CHECK(code == TEDER_NOT_FOUND || code == TEDER_BUSY);
			return;
		}
		FZ_CHECK(code == TEDER_OK || code == TEDER_NOT_FOUND || code == TEDER_BUSY);
		if (code == TEDER_BUSY)
			return;
		refresh(r);
		FZ_CHECK(r->terminal);
		if (code == TEDER_NOT_FOUND)
			FZ_CHECK(r->taken || r->kind == TEDER_RESULT_NONE);
		else {
			FZ_CHECK(!r->taken && r->kind != TEDER_RESULT_NONE);
			r->taken = true;
			keep(r, result);
		}
	} else {
		code = teder_request_ack(q.engine, id);
		if (!known) {
			FZ_CHECK(code == TEDER_NOT_FOUND || code == TEDER_BUSY);
			return;
		}
		FZ_CHECK(code == TEDER_OK || code == TEDER_BUSY);
		if (code == TEDER_OK) {
			r->acked = true;
			refresh(r);
		}
	}
}

static void step(sq_bytes *b)
{
	uint8_t op = next(b) % 24;
	if (op <= 9) {
		submit(b, op);
		return;
	}
	if (op == 10 || op == 12 || op == 13 || op == 14) {
		inquire(b, op);
		return;
	}
	uint8_t s = next(b);
	if (op == 11) {
		teder_request_info info = FZ_INIT(teder_request_info);
		teder_code code = teder_request_poll(q.engine, &info);
		FZ_CHECK(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK) {
			sq_request *r = find(info.request);
			FZ_CHECK(r && !r->acked && info.state == TEDER_TERMINAL);
			inspect(r, &info);
		}
	} else if (op == 15) {
		teder_event e = FZ_INIT(teder_event);
		teder_code code = teder_event_poll(q.engine, &e);
		FZ_CHECK(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK) {
			FZ_CHECK(e.struct_size == sizeof(e) && e.sequence > q.last_event && e.epoch >= 1);
			FZ_CHECK(e.kind >= TEDER_EVENT_TRANSITION && e.kind <= TEDER_EVENT_ERROR);
			q.last_event = e.sequence;
		}
	} else if (op == 16) {
		teder_snapshot *snapshot = (teder_snapshot *)&q;
		teder_code code = teder_snapshot_acquire(q.engine, &snapshot);
		bool full = q.loan_count == q.config.snapshot_loans;
		if (code != TEDER_OK) {
			FZ_CHECK(!snapshot && (code == TEDER_AGAIN || (code == TEDER_LIMIT && full)));
			return;
		}
		FZ_CHECK(snapshot && !full && q.loan_count < SQ_OWNED);
		uint64_t sequence;
		uint64_t hash = snapshot_digest(snapshot, &sequence);
		FZ_CHECK(sequence >= q.last_snapshot);
		q.last_snapshot = sequence;
		q.loans[q.loan_count++] = (sq_loan){snapshot, hash};
	} else if (op == 17 && q.loan_count) {
		release_loan(s % q.loan_count);
	} else if (op == 18 && q.result_count) {
		release_result(s % q.result_count);
	} else if (op == 19) {
		/* Render synchronously while started, within 20 ms of the input's budget. */
		fz_platform_credit(&q.platform, (1u + s) * 16u);
		uint64_t limit = now() + 20000000u;
		if (limit > q.deadline)
			limit = q.deadline;
		while (atomic_load(&q.platform.credit) && atomic_load(&q.platform.active) && now() < limit)
			pause_us(20);
	} else if (op == 20) {
		fz_platform_lose(&q.platform);
	} else if (op == 21 && q.count) {
		/* Wait up to 100 ms of the input's budget; running controls need callbacks. */
		sq_request *r = &q.requests[s % q.count];
		uint64_t limit = now() + 100000000u;
		if (limit > q.deadline)
			limit = q.deadline;
		refresh(r);
		while (!r->acked && !r->terminal && now() < limit) {
			fz_platform_refill(&q.platform, 64);
			pause_us(50);
			refresh(r);
		}
		if (trace)
			fz_trace("  settle %" PRIu64 " terminal %d acked %d\n", r->id, r->terminal, r->acked);
	} else if (op == 22 && now() < q.deadline) {
		pause_us(10u * (1u + s));
	}
}

/* Close, prove every request terminal, destroy, then release retained owners. */
static void finish(void)
{
	if (trace)
		fz_trace("close\n");
	FZ_CHECK(teder_request_close(q.engine) == TEDER_OK);
	FZ_CHECK(teder_request_close(q.engine) == TEDER_OK);
	q.closing = true;
	uint64_t id = 1;
	FZ_CHECK(teder_enumerate_devices(q.engine, &id) == TEDER_CLOSED && !id);
	teder_snapshot *snapshot = (teder_snapshot *)&q;
	FZ_CHECK(teder_snapshot_acquire(q.engine, &snapshot) == TEDER_CLOSED && !snapshot);
	do {
		observe();
		if (q.status.lifecycle != TEDER_CLOSED_STATE)
			sched_yield();
	} while (q.status.lifecycle != TEDER_CLOSED_STATE);
	for (unsigned i = 0; i < q.count; i++) {
		sq_request *r = &q.requests[i];
		if (r->acked)
			continue;
		refresh(r);
		FZ_CHECK(r->terminal);
		if (r->kind && !r->taken && (r->id & 1) && q.result_count < SQ_OWNED) {
			teder_result *result = NULL;
			FZ_CHECK(teder_request_take_result(q.engine, r->id, &result) == TEDER_OK);
			r->taken = true;
			keep(r, result);
		}
		if (r->id & 2) {
			FZ_CHECK(teder_request_ack(q.engine, r->id) == TEDER_OK);
			r->acked = true;
		}
	}
	FZ_CHECK(teder_destroy(&q.engine) == TEDER_OK && !q.engine);
	FZ_CHECK(teder_destroy(&q.engine) == TEDER_OK);
	while (q.loan_count)
		release_loan(q.loan_count - 1);
	while (q.result_count)
		release_result(q.result_count - 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < SQ_HEADER || size > SQ_MAX_INPUT)
		return 0;
	static const uint32_t pcm[] = {0, 1024, 4096, 16384};
	static const uint32_t bands[] = {0, 1, 16, 64};
	static const uint64_t limits[] = {0, 4 << 20, 12 << 20, 24 << 20};
	memset(&q, 0, sizeof(q));
	teder_config c = FZ_INIT(teder_config);
	c.request_capacity = 1 + (data[0] & 7);
	c.event_capacity = 1 + ((data[0] >> 3) & 7);
	c.pcm_frames = pcm[data[1] & 3];
	c.snapshot_slots = 1 + ((data[1] >> 2) & 3);
	c.snapshot_loans = 1 + ((data[1] >> 4) & 3);
	c.max_spans = 1 + ((data[1] >> 6) & 3);
	c.max_bands = bands[data[2] & 3];
	c.engine_bytes_limit = limits[(data[2] >> 2) & 3];
	c.result_bytes_limit = (data[2] & 16) ? 256 : 0;
	c.max_input_bytes = (data[2] & 32) ? 64 : 0;
	size_t fail_at = (data[4] & 128) ? 1 + (((size_t)data[4] & 7) << 8 | data[5]) : 0;
	fz_heap_init(&q.heap, fail_at);
	fz_platform_init(&q.platform, data[3]);
	teder_code code = td_engine_create_with(
	    &c, fz_heap_allocator(&q.heap), fz_platform_runtime(&q.platform), &q.engine);
	if (code != TEDER_OK) {
		FZ_CHECK(!q.engine &&
		         (code == TEDER_LIMIT || (code == TEDER_NO_MEMORY && fz_heap_failed(&q.heap))));
		fz_platform_end(&q.platform);
		fz_heap_end(&q.heap);
		if (trace)
			fz_trace_flush();
		return 0;
	}
	q.config = (teder_config)FZ_INIT(teder_config);
	FZ_CHECK(teder_get_config(q.engine, &q.config) == TEDER_OK);
	observe();
	FZ_CHECK(q.status.transport == TEDER_EMPTY && q.status.current.epoch == 1 &&
	         q.status.gain == 1 && !q.status.mute && q.status.output_state == TEDER_OUTPUT_NONE);
	sq_bytes b = {data + SQ_HEADER, size - SQ_HEADER};
	q.deadline = now() + 2000000000u;
	for (unsigned i = 0; i < SQ_OPS && b.left; i++) {
		if (trace)
			fz_trace("step %u opcode %u\n", i, *b.data % 24u);
		step(&b);
		observe();
	}
	finish();
	fz_platform_end(&q.platform);
	fz_heap_end(&q.heap);
	if (trace)
		fz_trace_flush();
	return 0;
}
