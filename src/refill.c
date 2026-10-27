#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"
#include "source.h"
#include <stdlib.h>
#ifndef _WIN32
#include <errno.h>
#include <time.h>
#endif

/* Poll while control delegates a potentially active consumer's refill work.
 * This is not a latency guarantee. No callback signals a waiter. */
static void interval(unsigned milliseconds)
{
#ifdef _WIN32
	Sleep(milliseconds);
#else
	struct timespec remaining = {0, (long)milliseconds * 1000000};
	while (nanosleep(&remaining, &remaining))
		if (errno != EINTR)
			abort();
#endif
}

void td_worker_interval(void)
{
	interval(1);
}

void td_refill_run(teder_engine *e)
{
	td_media_checkpoint(e, 0, TD_REFILL_STARTED);
	for (;;) {
		if (ma_event_wait(&e->refill_wake) != MA_SUCCESS)
			abort();
		if (atomic_load(&e->refill_stop))
			break;
		while (atomic_load(&e->refill_enabled) && atomic_load(&e->lifecycle) == TEDER_LIVE) {
			if (td_device_lost(e->device)) {
				td_signal(e); /* control alone may stop/free native owners */
				while (atomic_load(&e->refill_enabled) && atomic_load(&e->lifecycle) == TEDER_LIVE)
					td_worker_interval();
				break;
			}
			td_lock(e);
			bool monitor = e->device && e->status.transport != TEDER_PLAYING &&
			               e->status.transport != TEDER_DRAINING;
			td_unlock(e);
			if (monitor) {
				interval(10); /* paused/ready/EOF: observe loss without decoding or reporting */
				continue;
			}
			td_media_checkpoint(e, 0, TD_REFILL_PUMP);
			bool progress = td_pcm_reap(&e->pcm) != 0;
			progress = td_decode_pump(e) || progress;
			progress = td_output_report(e) || progress;
			if (progress)
				td_signal(e); /* retirement/status aggregation stays off the callback */
			else
				td_worker_interval();
		}
		/* Exactly one acknowledgement per resume, after the last producer access.
		 * Control consumes it before resuming again, so it cannot become stale. */
		if (ma_event_signal(&e->refill_idle) != MA_SUCCESS)
			abort();
		td_media_checkpoint(e, 0, TD_REFILL_PARKED);
		/* Close cannot itself terminate this loop: control may already have
		 * issued another resume after the acknowledgement above. */
	}
	td_media_checkpoint(e, 0, TD_REFILL_STOPPED);
}

void td_refill_pause(teder_engine *e)
{
	if (atomic_load(&e->refill_enabled)) {
		td_media_checkpoint(e, 0, TD_REFILL_PAUSING);
		atomic_store(&e->refill_enabled, 0);
		if (ma_event_wait(&e->refill_idle) != MA_SUCCESS)
			abort();
	}
}

void td_refill_stop(teder_engine *e)
{
	td_refill_pause(e);
	atomic_store(&e->refill_stop, 1);
	if (ma_event_signal(&e->refill_wake) != MA_SUCCESS)
		abort();
}

void td_refill_resume(teder_engine *e)
{
	/* Every selected device needs loss observation, even before playback or
	 * while sending pause/EOF silence. No selected device means no idle polling. */
	if (!e->runtime.consumer_join && !e->device)
		return;
	/* Control owns source state here, after pause. Preparation owns only STAGING
	 * jobs and cannot mutate any ACTIVE/RETIRING source inspected below. */
	td_lock(e);
	bool needed = e->device != NULL;
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		if (j->state == TD_JOB_RETIRING ||
		    (j->state == TD_JOB_ACTIVE && !j->source->done && j->source->error == TEDER_OK))
			needed = true;
	}
	td_unlock(e);
	if (!needed || atomic_load(&e->lifecycle) != TEDER_LIVE)
		return;
	if (atomic_exchange(&e->refill_enabled, 1) || ma_event_signal(&e->refill_wake) != MA_SUCCESS)
		abort();
	td_media_checkpoint(e, 0, TD_REFILL_RESUMED);
}
