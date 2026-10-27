#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "demo.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

uint64_t demo_millis(void)
{
#ifdef _WIN32
	return GetTickCount64();
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

void demo_wait(unsigned ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000};
	nanosleep(&delay, NULL);
#endif
}

teder_code demo_start(demo *d, const char *path, const char *next, bool native)
{
	*d = (demo){.path = path,
	            .next = next,
	            .native = native,
	            .stage = native ? 0 : 1,
	            .status = DEMO_INIT(teder_status),
	            .probe = DEMO_INIT(teder_probe_info),
	            .completion = DEMO_INIT(teder_request_info)};
	teder_config config = DEMO_INIT(teder_config);
	return teder_create(&config, &d->engine);
}

teder_code demo_issue(demo *d, char command, uint64_t frame)
{
	if (d->pending || d->closing)
		return TEDER_BUSY;
	teder_engine *e = d->engine;
	teder_target *target = &d->status.current;
	uint64_t *id = &d->pending;
	teder_code code;
	teder_analysis_config analysis = DEMO_INIT(teder_analysis_config);
	analysis.enabled = 1;
	analysis.bands = 64;
	analysis.window_frames = 2048;
	analysis.hop_frames = 512;
	analysis.smoothing_ms = 90;
	analysis.peak_decay_db_per_s = 18;
	switch (command) {
	case 'd':
		code = teder_select_device(e, NULL, 0, id);
		break;
	case 'a':
		code = teder_configure_analysis(e, &analysis, id);
		break;
	case 'i':
		code = teder_probe(e, d->path, (uint32_t)strlen(d->path), TEDER_PROBE_ARTWORK, id);
		break;
	case 'o':
		code = teder_open(e, d->path, (uint32_t)strlen(d->path), 1, 0, id);
		break;
	case 'n':
		if (!d->next)
			return TEDER_INVALID;
		code = teder_prepare_next(
		    e, target, d->status.next_source, d->next, (uint32_t)strlen(d->next), 2, id);
		break;
	case 'c':
		code = teder_clear_next(e, target, d->status.next_source, id);
		break;
	case 'p':
		code = teder_transport(e, target, TEDER_PLAY, id);
		break;
	case ' ':
		code = teder_transport(e,
		                       target,
		                       d->status.transport == TEDER_PLAYING ||
		                               d->status.transport == TEDER_DRAINING
		                           ? TEDER_PAUSE
		                           : TEDER_PLAY,
		                       id);
		break;
	case 's':
		code = teder_transport(e, target, TEDER_STOP, id);
		break;
	case 'j':
		code = teder_seek(e, target, frame, id);
		break;
	case 'm':
		code = teder_set_gain(e, d->status.gain, !d->status.mute, id);
		break;
	default:
		return TEDER_INVALID;
	}
	if (code != TEDER_BUSY)
		d->outcome = code;
	d->taken = false;
	return code;
}

static teder_code complete(demo *d)
{
	teder_code code = teder_request_get(d->engine, d->pending, &d->completion);
	if (code != TEDER_OK)
		return code;
	if (d->completion.state != TEDER_TERMINAL)
		return TEDER_AGAIN;
	if (d->completion.result_kind == TEDER_RESULT_PROBE && !d->taken) {
		teder_result *result = NULL;
		code = teder_request_take_result(d->engine, d->pending, &result);
		if (code != TEDER_OK)
			return code == TEDER_BUSY ? TEDER_AGAIN : code;
		teder_result_release(d->metadata);
		d->metadata = result;
		d->taken = true;
		if (teder_result_probe(result, &d->probe) != TEDER_OK)
			return TEDER_INTERNAL;
	}
	code = teder_request_ack(d->engine, d->pending);
	if (code != TEDER_OK)
		return code == TEDER_BUSY ? TEDER_AGAIN : code;
	d->pending = 0;
	d->outcome = d->completion.outcome;
	if (d->completion.operation == TEDER_OP_PREPARE && d->outcome == TEDER_OK)
		d->prepared_source = d->completion.committed.source;
	if (d->stage < 6) {
		if (d->outcome != TEDER_OK && (d->stage == 1 || d->stage == 3))
			d->stage = 6;
		else
			++d->stage;
	}
	return TEDER_OK;
}

teder_code demo_tick(demo *d)
{
	teder_code code;
	if (d->pending && !d->closing) {
		code = complete(d);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			return code;
	}
	/* Read after completion, so callers never observe pre-commit status as idle. */
	code = teder_get_status(d->engine, &d->status);
	if (code != TEDER_OK || d->closing)
		return code;
	if (!d->pending && d->stage < 6) {
		if (d->stage == 4 && !d->next)
			++d->stage;
		if (d->stage == 5 && !d->native)
			++d->stage;
		if (d->stage < 6) {
			code = demo_issue(d, "daionp"[d->stage], 0);
			if (code != TEDER_OK && code != TEDER_BUSY)
				d->stage = 6;
		}
	}
	teder_snapshot *fresh = NULL;
	code = teder_snapshot_acquire(d->engine, &fresh);
	if (code != TEDER_OK && code != TEDER_AGAIN)
		return code;
	teder_snapshot_info info = DEMO_INIT(teder_snapshot_info);
	if (fresh || (d->snapshot && (teder_snapshot_get_info(d->snapshot, &info) != TEDER_OK ||
	                              info.epoch != d->status.current.epoch))) {
		teder_snapshot_release(d->snapshot);
		d->snapshot = fresh;
	}
	/* Telemetry is optional; identities and queue decisions use coherent status. */
	teder_event event = DEMO_INIT(teder_event);
	for (unsigned i = 0; i < 16 && teder_event_poll(d->engine, &event) == TEDER_OK; ++i) {
	}
	return TEDER_OK;
}

const char *demo_note(const demo *d)
{
	teder_code error = d->outcome != TEDER_OK ? d->outcome : d->status.last_error;
	if (d->closing)
		return "Closing...";
	if (d->status.output_state == TEDER_OUTPUT_LOST)
		return "Output lost: D selects default; Space resumes";
	if (d->pending || d->stage < 6)
		return "Working...";
	if (error == TEDER_TOO_LATE)
		return "Successor missed cutoff; choose a new playback explicitly";
	if (error != TEDER_OK)
		return "Request failed (code below); Q closes safely";
	if (d->status.transport == TEDER_ENDED)
		return "Submitted EOF; allow the device tail to finish, then Q";
	return d->metadata ? (d->probe.artwork_count ? "Initial file has artwork"
	                                             : "Initial file has no artwork")
	                   : "Metadata unavailable";
}

int demo_finish(demo *d)
{
	int failed = 0;
	if (d->engine) {
		failed |= teder_request_close(d->engine) != TEDER_OK;
		while (teder_get_status(d->engine, &d->status) != TEDER_OK ||
		       d->status.lifecycle != TEDER_CLOSED_STATE)
			demo_wait(5);
		failed |= teder_destroy(&d->engine) != TEDER_OK;
	}
	/* Deliberately verify independent owners after the engine has gone. */
	teder_snapshot_info info = DEMO_INIT(teder_snapshot_info);
	if (d->snapshot)
		failed |= teder_snapshot_get_info(d->snapshot, &info) != TEDER_OK;
	if (d->metadata)
		failed |= teder_result_probe(d->metadata, &d->probe) != TEDER_OK;
	teder_snapshot_release(d->snapshot);
	teder_result_release(d->metadata);
	d->snapshot = NULL;
	d->metadata = NULL;
	return failed;
}
