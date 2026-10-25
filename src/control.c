#include "internal.h"
#include <assert.h>
#include <stdlib.h>
#ifdef _WIN32
#include <process.h>
#else
#include <errno.h>
#endif

bool td_try_lock(teder_engine *e)
{
#ifdef _WIN32
	return TryAcquireSRWLockExclusive(&e->gate) != 0;
#else
	int code = pthread_mutex_trylock(&e->gate);
	if (code && code != EBUSY) abort();
	return code == 0;
#endif
}
void td_lock(teder_engine *e)
{
#ifdef _WIN32
	AcquireSRWLockExclusive(&e->gate);
#else
	if (pthread_mutex_lock(&e->gate)) abort();
#endif
}
void td_unlock(teder_engine *e)
{
#ifdef _WIN32
	ReleaseSRWLockExclusive(&e->gate);
#else
	if (pthread_mutex_unlock(&e->gate)) abort();
#endif
}
void td_signal(teder_engine *e) { if (ma_event_signal(&e->wake) != MA_SUCCESS) abort(); }
static void checkpoint(teder_engine *e, uint32_t point)
{
	if (e->runtime.checkpoint) e->runtime.checkpoint(e->runtime.context, point);
}
static void control(teder_engine *e)
{
	checkpoint(e, TD_STARTED);
	for (;;) {
		td_lock(e);
		uint32_t slot = e->config.request_capacity;
		for (uint32_t i = 0; i < e->config.request_capacity; i++)
			if (e->requests[i].info.state == TEDER_QUEUED &&
				(slot == e->config.request_capacity || e->requests[i].info.request < e->requests[slot].info.request)) slot = i;
		if (slot == e->config.request_capacity) {
			bool closing = atomic_load(&e->lifecycle) != TEDER_LIVE;
			td_unlock(e);
			if (closing) break;
			if (ma_event_wait(&e->wake) != MA_SUCCESS) abort();
			continue;
		}
		td_request_slot *s = &e->requests[slot];
		s->info.state = TEDER_RUNNING;
		bool cancel = s->cancelled || atomic_load(&e->lifecycle) != TEDER_LIVE;
		td_unlock(e);
		teder_code code = cancel ? TEDER_CANCELLED : e->runtime.execute(e->runtime.context, e, slot);
		td_lock(e);
		if (!s->committed && (s->cancelled || atomic_load(&e->lifecycle) != TEDER_LIVE)) code = TEDER_CANCELLED;
		if (code == TEDER_OK && !s->committed) code = TEDER_INTERNAL;
		/* Retire unpublished results before terminal publication or slot reuse. */
		if (code != TEDER_OK && s->result) {
			teder_result *discard = s->result;
			size_t bytes = discard->bytes; s->result = NULL;
			td_unlock(e); teder_result_release(discard); td_lock(e);
			e->owned_bytes -= bytes;
		}
		s->info.outcome = code;
		s->info.result_kind = s->result ? s->result->kind : TEDER_RESULT_NONE;
		s->completed = ++e->last_completion;
		s->info.state = TEDER_TERMINAL;
		td_unlock(e);
	}
	/* This worker is also the lifecycle owner. Future workers must be joined
	 * here. Neither this function nor its native trampoline may access e after
	 * the final release-store; destroy joins the remaining OS-thread return. */
	checkpoint(e, TD_STOPPED);
	atomic_store(&e->lifecycle, TEDER_CLOSED_STATE);
}
#ifdef _WIN32
static unsigned __stdcall entry(void *p) { control(p); return 0; }
#else
static void *entry(void *p) { control(p); return NULL; }
#endif
static bool permit(teder_engine *e, uint32_t stage)
{
	return !e->runtime.permit || e->runtime.permit(e->runtime.context, stage);
}
teder_code td_control_start(teder_engine *e)
{
	if (!permit(e, TD_INIT_LOCK)) return TEDER_INTERNAL;
#ifdef _WIN32
	InitializeSRWLock(&e->gate);
#else
	if (pthread_mutex_init(&e->gate, NULL)) return TEDER_INTERNAL;
#endif
	if (!permit(e, TD_INIT_WAKE) || ma_event_init(&e->wake) != MA_SUCCESS) goto lock_failed;
	if (!permit(e, TD_INIT_THREAD)) goto thread_failed;
#ifdef _WIN32
	e->thread = (HANDLE)_beginthreadex(NULL, 0, entry, e, 0, NULL);
	if (!e->thread) goto thread_failed;
#else
	if (pthread_create(&e->thread, NULL, entry, e)) goto thread_failed;
#endif
	if (permit(e, TD_INIT_PUBLISH)) return TEDER_OK;
	teder_request_close(e); td_control_join(e);
thread_failed:
	ma_event_uninit(&e->wake);
lock_failed:
#ifndef _WIN32
	if (pthread_mutex_destroy(&e->gate)) abort();
#endif
	return TEDER_INTERNAL;
}
void td_control_join(teder_engine *e)
{
#ifdef _WIN32
	if (WaitForSingleObject(e->thread, INFINITE) != WAIT_OBJECT_0 || !CloseHandle(e->thread)) abort();
#else
	if (pthread_join(e->thread, NULL)) abort();
#endif
}
void td_control_dispose(teder_engine *e)
{
	ma_event_uninit(&e->wake);
#ifndef _WIN32
	if (pthread_mutex_destroy(&e->gate)) abort();
#else
	(void)e;
#endif
}
teder_code td_commit(teder_engine *e, uint32_t slot)
{
	td_lock(e);
	td_request_slot *s = &e->requests[slot];
	assert(s->info.state == TEDER_RUNNING && !s->committed);
	/* The same-value RMW orders commitment against close's independent RMW.
	 * Cancellation is ordered by the gate. Close never needs that gate. */
	unsigned live = TEDER_LIVE;
	bool commit = !s->cancelled && atomic_compare_exchange_strong(&e->lifecycle, &live, TEDER_LIVE);
	s->committed = commit;
	td_unlock(e);
	return commit ? TEDER_OK : TEDER_CANCELLED;
}
teder_code td_build_result(teder_engine *e, uint32_t slot, const td_result_input *input)
{
	td_lock(e);
	td_request_slot *s = &e->requests[slot];
	assert(s->info.state == TEDER_RUNNING && !s->result);
	uint64_t available = e->config.engine_bytes_limit - e->owned_bytes;
	size_t reservation = (size_t)(available < e->config.result_bytes_limit ? available : e->config.result_bytes_limit);
	e->owned_bytes += reservation;
	td_unlock(e);
	teder_result *r = NULL;
	teder_code code = td_result_create(input, e->allocator, reservation, &r);
	td_lock(e);
	e->owned_bytes -= reservation;
	if (code == TEDER_OK) { s->result = r; e->owned_bytes += r->bytes; }
	td_unlock(e);
	return code;
}
