#include "harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool th_tracker_init(th_tracker *t)
{
	memset(t, 0, sizeof(*t));
	return ma_mutex_init(&t->lock) == MA_SUCCESS;
}

void *th_alloc(void *context, size_t size, const char *site)
{
	th_tracker *t = context;
	void *p = NULL;
	ma_mutex_lock(&t->lock);
	t->attempts++;
	if (t->attempts == t->fail_at)
		t->failed_site = site;
	else if (size && size <= SIZE_MAX - t->bytes) {
		for (size_t i = 0; i < 128; i++)
			if (!t->blocks[i].pointer) {
				p = malloc(size);
				if (!p)
					break;
				t->blocks[i].pointer = p;
				t->blocks[i].size = size;
				t->blocks[i].site = site;
				t->live++;
				t->bytes += size;
				if (t->bytes > t->peak)
					t->peak = t->bytes;
				break;
			}
	}
	ma_mutex_unlock(&t->lock);
	return p;
}

void th_free(void *context, void *p)
{
	th_tracker *t = context;
	if (!p)
		return;
	ma_mutex_lock(&t->lock);
	for (size_t i = 0; i < 128; i++)
		if (t->blocks[i].pointer == p) {
			t->bytes -= t->blocks[i].size;
			t->live--;
			t->blocks[i].pointer = NULL;
			free(p);
			ma_mutex_unlock(&t->lock);
			return;
		}
	t->errors++;
	fprintf(stderr, "wrong free: unowned block\n");
	ma_mutex_unlock(&t->lock);
}

bool th_tracker_clean(th_tracker *t)
{
	bool clean;
	ma_mutex_lock(&t->lock);
	clean = !t->live && !t->bytes && !t->errors;
	for (size_t i = 0; i < 128; i++)
		if (t->blocks[i].pointer)
			fprintf(stderr, "leak site=%s bytes=%zu\n", t->blocks[i].site, t->blocks[i].size);
	ma_mutex_unlock(&t->lock);
	return clean;
}

void th_tracker_uninit(th_tracker *t)
{
	if (t->live)
		abort();
	ma_mutex_uninit(&t->lock);
}

td_allocator th_allocator(th_tracker *t)
{
	return (td_allocator){t, th_alloc, th_free};
}

static void *ma_alloc(size_t n, void *t)
{
	return th_alloc(t, n, "miniaudio.malloc");
}

static void ma_release(void *p, void *t)
{
	th_free(t, p);
}

static void *ma_resize(void *p, size_t n, void *context)
{
	th_tracker *t = context;
	size_t old = 0;
	void *next;
	if (!p)
		return th_alloc(t, n, "miniaudio.realloc");
	if (!n) {
		th_free(t, p);
		return NULL;
	}
	ma_mutex_lock(&t->lock);
	for (size_t i = 0; i < 128; i++)
		if (t->blocks[i].pointer == p)
			old = t->blocks[i].size;
	if (!old)
		t->errors++;
	ma_mutex_unlock(&t->lock);
	if (!old)
		return NULL;
	next = th_alloc(t, n, "miniaudio.realloc");
	if (next) {
		memcpy(next, p, old < n ? old : n);
		th_free(t, p);
	}
	return next;
}

ma_allocation_callbacks th_ma_allocator(th_tracker *t)
{
	ma_allocation_callbacks cb = {t, ma_alloc, ma_resize, ma_release};
	return cb;
}

static void event_ok(ma_result result)
{
	if (result != MA_SUCCESS)
		abort();
}

bool th_gate_init(th_gate *g)
{
	if (ma_event_init(&g->arrived) != MA_SUCCESS)
		return false;
	if (ma_event_init(&g->release) == MA_SUCCESS)
		return true;
	ma_event_uninit(&g->arrived);
	return false;
}

void th_gate_hit(th_gate *g)
{
	event_ok(ma_event_signal(&g->arrived));
	event_ok(ma_event_wait(&g->release));
}

void th_gate_await(th_gate *g)
{
	event_ok(ma_event_wait(&g->arrived));
}

void th_gate_release(th_gate *g)
{
	event_ok(ma_event_signal(&g->release));
}

void th_gate_uninit(th_gate *g)
{
	ma_event_uninit(&g->release);
	ma_event_uninit(&g->arrived);
}
#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID p)
{
	th_thread *t = p;
	t->run(t->context);
	return 0;
}
#else
static void *thread_entry(void *p)
{
	th_thread *t = p;
	t->run(t->context);
	return NULL;
}
#endif
bool th_thread_start(th_thread *t, void (*run)(void *), void *context)
{
	t->run = run;
	t->context = context;
#ifdef _WIN32
	t->handle = CreateThread(NULL, 0, thread_entry, t, 0, NULL);
	return t->handle != NULL;
#else
	return pthread_create(&t->handle, NULL, thread_entry, t) == 0;
#endif
}

void th_thread_join(th_thread *t)
{
#ifdef _WIN32
	if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0)
		abort();
	if (!CloseHandle(t->handle))
		abort();
#else
	if (pthread_join(t->handle, NULL))
		abort();
#endif
}

bool th_sink_init(
    th_sink *s, float *pcm, size_t capacity, size_t channels, th_render render, void *context)
{
	size_t samples, bytes;
	if (!pcm || !render || !capacity || !channels || !td_size_mul(capacity, channels, &samples) ||
	    !td_size_mul(samples, sizeof(float), &bytes))
		return false;
	memset(s, 0, sizeof(*s));
	s->pcm = pcm;
	s->capacity = capacity;
	s->channels = channels;
	s->render = render;
	s->context = context;
	return ma_mutex_init(&s->lock) == MA_SUCCESS;
}

th_status th_sink_step(th_sink *s, size_t frames, bool force_underrun)
{
	size_t got = 0;
	float *out;
	th_status status;
	ma_mutex_lock(&s->lock);
	status = s->device_error                    ? TH_DEVICE
	         : s->stopped                       ? TH_STOPPED
	         : s->active                        ? TH_BUSY
	         : !frames                          ? TH_INVALID
	         : frames > s->capacity - s->frames ? TH_CAPACITY
	                                            : TH_OK;
	if (status != TH_OK) {
		ma_mutex_unlock(&s->lock);
		return status;
	}
	s->active = true;
	out = s->pcm + s->frames * s->channels;
	ma_mutex_unlock(&s->lock);
	memset(out, 0, frames * s->channels * sizeof(float));
	if (!force_underrun)
		got = s->render(s->context, out, frames);
	ma_mutex_lock(&s->lock);
	if (got > frames)
		status = TH_INVALID;
	else {
		s->frames += frames;
		s->underruns += frames - got;
	}
	s->active = false;
	ma_mutex_unlock(&s->lock);
	return status;
}

bool th_sink_stop(th_sink *s)
{
	bool quiet;
	ma_mutex_lock(&s->lock);
	s->stopped = true;
	quiet = !s->active;
	ma_mutex_unlock(&s->lock);
	return quiet;
}

void th_sink_device_error(th_sink *s)
{
	ma_mutex_lock(&s->lock);
	s->device_error = true;
	ma_mutex_unlock(&s->lock);
}

bool th_sink_uninit(th_sink *s)
{
	if (!th_sink_stop(s))
		return false;
	ma_mutex_uninit(&s->lock);
	return true;
}

bool th_clock_advance(uint64_t *now, uint64_t delta)
{
	if (delta > UINT64_MAX - *now)
		return false;
	*now += delta;
	return true;
}

bool th_pcm_equal(const float *a, const float *b, size_t frames, size_t channels)
{
	size_t samples;
	if (!channels || !td_size_mul(frames, channels, &samples))
		return false;
	for (size_t i = 0; i < samples; i++)
		if (a[i] != b[i]) {
			fprintf(stderr,
			        "pcm mismatch frame=%zu channel=%zu expected=%a actual=%a\n",
			        i / channels,
			        i % channels,
			        (double)b[i],
			        (double)a[i]);
			return false;
		}
	return true;
}
