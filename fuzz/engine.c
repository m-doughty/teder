#include "support.h"
#include "engine.h"
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <time.h>

enum { FZ_HEAP_MAX = 256 * 1024 * 1024, FZ_RENDER_MAX = 256 };

typedef union {
	max_align_t alignment;
	struct {
		fz_heap *owner;
		size_t size;
	} value;
} block;

static void *heap_allocate(void *context, size_t size, const char *site)
{
	(void)site;
	fz_heap *h = context;
	FZ_CHECK(size && size <= FZ_HEAP_MAX);
	FZ_CHECK(pthread_mutex_lock(&h->lock) == 0);
	bool fail = ++h->attempts == h->fail_at;
	if (fail)
		h->failed = true;
	else {
		FZ_CHECK(h->bytes <= FZ_HEAP_MAX - size);
		h->bytes += size;
		h->live++;
		if (h->bytes > h->peak)
			h->peak = h->bytes;
	}
	FZ_CHECK(pthread_mutex_unlock(&h->lock) == 0);
	if (fail)
		return NULL;
	block *b = malloc(sizeof(*b) + size);
	FZ_CHECK(b);
	b->value.owner = h;
	b->value.size = size;
	return b + 1;
}

static void heap_release(void *context, void *pointer)
{
	if (!pointer)
		return;
	fz_heap *h = context;
	block *b = (block *)pointer - 1;
	FZ_CHECK(b->value.owner == h);
	FZ_CHECK(pthread_mutex_lock(&h->lock) == 0);
	FZ_CHECK(h->live && h->bytes >= b->value.size);
	h->bytes -= b->value.size;
	h->live--;
	FZ_CHECK(pthread_mutex_unlock(&h->lock) == 0);
	free(b);
}

void fz_heap_init(fz_heap *h, size_t fail_at)
{
	memset(h, 0, sizeof(*h));
	FZ_CHECK(pthread_mutex_init(&h->lock, NULL) == 0);
	h->fail_at = fail_at;
}

td_allocator fz_heap_allocator(fz_heap *h)
{
	return (td_allocator){h, heap_allocate, heap_release};
}

bool fz_heap_failed(fz_heap *h)
{
	FZ_CHECK(pthread_mutex_lock(&h->lock) == 0);
	bool failed = h->failed;
	FZ_CHECK(pthread_mutex_unlock(&h->lock) == 0);
	return failed;
}

void fz_heap_end(fz_heap *h)
{
	FZ_CHECK(!h->live && !h->bytes);
	FZ_CHECK(pthread_mutex_destroy(&h->lock) == 0);
}

/* Every context/device callback runs on the engine's single lifecycle worker. */
static void owner(fz_platform *f)
{
	FZ_CHECK(f->owned && pthread_equal(pthread_self(), f->owner) &&
	         !pthread_equal(f->owner, f->host));
}

static ma_result
describe(ma_context *c, ma_device_type type, const ma_device_id *id, ma_device_info *info)
{
	(void)c;
	(void)type;
	memset(info, 0, sizeof(*info));
	info->id.wasapi[0] = id ? id->wasapi[0] : 'A';
	memcpy(info->name, "Selected", 9);
	return MA_SUCCESS;
}

static ma_result device_info(ma_device *d, ma_device_type type, ma_device_info *info)
{
	owner(d->pContext->pUserData);
	return describe(d->pContext, type, &d->playback.id, info);
}

static void *render(void *context)
{
	fz_platform *f = context;
	static const unsigned sizes[] = {1, 31, FZ_RENDER_MAX, 7, 64};
	float pcm[2 * FZ_RENDER_MAX + 1];
	unsigned call = 0;
	while (!atomic_load(&f->stop)) {
		if (atomic_exchange(&f->lose, 0)) {
			ma_device_notification n = {.pDevice = f->native,
			                            .type = ma_device_notification_type_rerouted};
			f->native->onNotification(&n);
		}
		unsigned credit = atomic_load(&f->credit);
		if (!credit) {
			sched_yield();
			continue;
		}
		unsigned frames = sizes[call++ % 5];
		if (frames > credit)
			frames = credit;
		atomic_fetch_sub(&f->credit, frames);
		pcm[2 * frames] = 12345;
		f->native->onData(f->native, pcm, NULL, frames);
		FZ_CHECK(pcm[2 * frames] == 12345);
		for (unsigned i = 0; i < 2 * frames; i++)
			FZ_CHECK(isfinite(pcm[i]));
		atomic_fetch_add(&f->rendered, frames);
		atomic_fetch_add(&f->callbacks, 1);
		/* About 1 us per frame: faster than hardware, yet producers can still keep up. */
		struct timespec pace = {0, (long)frames * 1000};
		nanosleep(&pace, NULL);
	}
	return NULL;
}

static void join(fz_platform *f)
{
	atomic_store(&f->stop, 1);
	atomic_store(&f->active, 0);
	if (f->rendering) {
		FZ_CHECK(pthread_join(f->render, NULL) == 0);
		f->rendering = false;
	}
}

static ma_result device_init(ma_device *d,
                             const ma_device_config *config,
                             ma_device_descriptor *playback,
                             ma_device_descriptor *capture)
{
	(void)capture;
	fz_platform *f = d->pContext->pUserData;
	owner(f);
	FZ_CHECK(!f->live_devices && config->playback.pDeviceID);
	ma_wchar_win32 id = config->playback.pDeviceID->wasapi[0];
	FZ_CHECK(id == 'A' || id == 'B');
	if (f->fail_init)
		return MA_ERROR;
	playback->format = ma_format_f32;
	playback->channels = 2;
	playback->sampleRate = id == 'A' ? 44100 : 22050;
	playback->periodSizeInFrames = f->period;
	f->native = d;
	f->devices++;
	f->live_devices++;
	return MA_SUCCESS;
}

static ma_result device_uninit(ma_device *d)
{
	fz_platform *f = d->pContext->pUserData;
	owner(f);
	FZ_CHECK(f->live_devices == 1 && f->native == d);
	join(f);
	f->releases++;
	f->live_devices--;
	f->native = NULL;
	return MA_SUCCESS;
}

static ma_result device_start(ma_device *d)
{
	fz_platform *f = d->pContext->pUserData;
	owner(f);
	FZ_CHECK(f->native == d && !f->rendering);
	if (f->trace)
		fz_trace("    device start%s\n", f->fail_start ? " (injected failure)" : "");
	if (f->fail_start)
		return MA_ERROR;
	atomic_store(&f->stop, 0);
	FZ_CHECK(pthread_create(&f->render, NULL, render, f) == 0);
	f->rendering = true;
	atomic_store(&f->active, 1);
	return MA_SUCCESS;
}

static ma_result device_stop(ma_device *d)
{
	fz_platform *f = d->pContext->pUserData;
	owner(f);
	FZ_CHECK(f->native == d);
	join(f);
	if (f->trace)
		fz_trace("    device stop after %" PRIu64 " frames\n", (uint64_t)f->rendered);
	return MA_SUCCESS;
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	(void)backends;
	(void)count;
	fz_platform *f = config->pUserData;
	if (!f->owned) {
		f->owner = pthread_self();
		f->owned = true;
	}
	owner(f);
	FZ_CHECK(f->contexts == f->closed && !f->native_heap);
	memset(c, 0, sizeof(*c));
	c->backend = ma_backend_wasapi;
	c->pUserData = f;
	c->allocationCallbacks = config->allocationCallbacks;
	/* Exercise teder's bounded backend heap from the native side. */
	f->native_heap = ma_malloc(64, &c->allocationCallbacks);
	if (!f->native_heap)
		return MA_OUT_OF_MEMORY;
	ma_result code = ma_mutex_init(&c->deviceInfoLock);
	if (code != MA_SUCCESS) {
		ma_free(f->native_heap, &c->allocationCallbacks);
		f->native_heap = NULL;
		return code;
	}
	c->callbacks.onContextGetDeviceInfo = describe;
	c->callbacks.onDeviceGetInfo = device_info;
	c->callbacks.onDeviceInit = device_init;
	c->callbacks.onDeviceUninit = device_uninit;
	c->callbacks.onDeviceStart = device_start;
	c->callbacks.onDeviceStop = device_stop;
	f->contexts++;
	return MA_SUCCESS;
}

static ma_result context_uninit(ma_context *c)
{
	fz_platform *f = c->pUserData;
	owner(f);
	FZ_CHECK(!f->live_devices);
	ma_free(f->native_heap, &c->allocationCallbacks);
	ma_mutex_uninit(&c->deviceInfoLock);
	f->native_heap = NULL;
	f->closed++;
	return MA_SUCCESS;
}

static ma_result enumerate(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	owner(c->pUserData);
	void *temporary = ma_malloc(32, &c->allocationCallbacks);
	if (!temporary)
		return MA_OUT_OF_MEMORY;
	ma_free(temporary, &c->allocationCallbacks);
	for (unsigned i = 0; i < 2; i++) {
		ma_device_info info = {0};
		info.id.wasapi[0] = (ma_wchar_win32)('A' + i);
		info.isDefault = i == 0;
		memcpy(info.name, i ? "Beta" : "Alpha", i ? 5 : 6);
		if (!callback(c, ma_device_type_playback, &info, user))
			break;
	}
	return MA_SUCCESS;
}

static bool permit(void *context, uint32_t stage)
{
	(void)context;
	(void)stage;
	return true;
}

static void control_point(void *context, uint32_t point)
{
	fz_platform *f = context;
	if (point == TD_STARTED)
		atomic_fetch_add(&f->entries[0], 1);
	if (point == TD_STOPPED)
		atomic_fetch_add(&f->exits[0], 1);
}

static void media_point(void *context, uint64_t request, unsigned point)
{
	fz_platform *f = context;
	/* Frequent worker-loop points would drown the media and device sequence. */
	if (f->trace && point != TD_REFILL_PUMP && point != TD_DECODE_IDLE &&
	    point != TD_ANALYSIS_BLOCK)
		fz_trace("    checkpoint %u request %" PRIu64 "\n", point, request);
	static const unsigned starts[] = {
	    TD_PREPARE_STARTED, TD_REFILL_STARTED, TD_PROBE_STARTED, TD_ANALYSIS_STARTED};
	static const unsigned stops[] = {
	    TD_PREPARE_STOPPED, TD_REFILL_STOPPED, TD_PROBE_STOPPED, TD_ANALYSIS_STOPPED};
	for (unsigned i = 0; i < 4; i++) {
		if (point == starts[i])
			atomic_fetch_add(&f->entries[i + 1], 1);
		if (point == stops[i])
			atomic_fetch_add(&f->exits[i + 1], 1);
	}
	if (point == TD_FILE_OPENED)
		atomic_fetch_add(&f->sources_opened, 1);
	if (point == TD_FILE_CLOSED)
		atomic_fetch_add(&f->sources_closed, 1);
	if (point == TD_PROBE_OPENED)
		atomic_fetch_add(&f->probes_opened, 1);
	if (point == TD_PROBE_CLOSED)
		atomic_fetch_add(&f->probes_closed, 1);
}

void fz_platform_init(fz_platform *f, uint8_t flags)
{
	static const unsigned periods[] = {8, 64, 256, 1024};
	memset(f, 0, sizeof(*f));
	f->host = pthread_self();
	f->fail_init = flags & 1;
	f->fail_start = (flags & 2) != 0;
	f->period = periods[(flags >> 2) & 3];
	/* Level 2 adds worker checkpoints and device calls; it perturbs scheduling. */
	const char *trace = getenv("TEDER_FUZZ_TRACE");
	f->trace = trace && atoi(trace) >= 2;
	atomic_init(&f->stop, 0);
	atomic_init(&f->lose, 0);
	atomic_init(&f->credit, 0);
	atomic_init(&f->active, 0);
	atomic_init(&f->rendered, 0);
	atomic_init(&f->callbacks, 0);
	for (unsigned i = 0; i < FZ_WORKERS; i++) {
		atomic_init(&f->entries[i], 0);
		atomic_init(&f->exits[i], 0);
	}
	atomic_init(&f->sources_opened, 0);
	atomic_init(&f->sources_closed, 0);
	atomic_init(&f->probes_opened, 0);
	atomic_init(&f->probes_closed, 0);
}

td_runtime fz_platform_runtime(fz_platform *f)
{
	return (td_runtime){.context = f,
	                    .permit = permit,
	                    .checkpoint = control_point,
	                    .media_checkpoint = media_point,
	                    .backend = {context_init, context_uninit, enumerate, f}};
}

void fz_platform_credit(fz_platform *f, unsigned frames)
{
	atomic_fetch_add(&f->credit, frames);
}

void fz_platform_refill(fz_platform *f, unsigned frames)
{
	unsigned empty = 0;
	atomic_compare_exchange_strong(&f->credit, &empty, frames);
}

void fz_platform_lose(fz_platform *f)
{
	atomic_store(&f->lose, 1);
}

void fz_platform_end(fz_platform *f)
{
	for (unsigned i = 0; i < FZ_WORKERS; i++)
		FZ_CHECK(atomic_load(&f->entries[i]) == atomic_load(&f->exits[i]));
	FZ_CHECK(atomic_load(&f->sources_opened) == atomic_load(&f->sources_closed));
	FZ_CHECK(atomic_load(&f->probes_opened) == atomic_load(&f->probes_closed));
	FZ_CHECK(f->contexts == f->closed && f->devices == f->releases && !f->live_devices);
	FZ_CHECK(!f->rendering && !f->native && !f->native_heap);
}
