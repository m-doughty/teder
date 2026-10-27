#define _POSIX_C_SOURCE 200809L
#include <teder.h>
#include <teder_notcurses.h>
#include <stddef.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

static void tick(void)
{
	Sleep(1);
}
#else
#include <time.h>

static void tick(void)
{
	struct timespec t = {0, 1000000};
	nanosleep(&t, NULL);
}
#endif
static teder_code status_read(teder_engine *e, teder_status *s)
{
	teder_code code;
	while ((code = teder_get_status(e, s)) == TEDER_AGAIN)
		tick();
	return code;
}

static teder_code completed(teder_engine *e, uint64_t id, teder_request_info *r)
{
	for (;;) {
		teder_code code = teder_request_get(e, id, r);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			return code;
		if (code == TEDER_OK && r->state == TEDER_TERMINAL)
			return r->outcome;
		tick();
	}
}

int main(int argc, char **argv)
{
	teder_build_info info = {0};
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	if (teder_get_build_info(&info) != TEDER_OK)
		return 1;
	if (info.abi_version != 1 || info.capabilities != 0 || info.backend_mask != 0)
		return 2;
	uint64_t invalid_request = 99;
	if (teder_enumerate_devices(NULL, &invalid_request) != TEDER_INVALID || invalid_request != 0)
		return 19;
	invalid_request = 99;
	if (teder_select_device(NULL, NULL, 0, &invalid_request) != TEDER_INVALID ||
	    invalid_request != 0)
		return 20;
	teder_target invalid_target = {0};
	if (teder_transport(NULL, &invalid_target, TEDER_PLAY, &invalid_request) != TEDER_INVALID ||
	    invalid_request != 0)
		return 21;
	if (teder_set_gain(NULL, 0.5f, 0, &invalid_request) != TEDER_INVALID || invalid_request != 0)
		return 22;
	if (teder_prepare_next(NULL, &invalid_target, 0, NULL, 0, 42, &invalid_request) !=
	        TEDER_INVALID ||
	    invalid_request != 0)
		return 23;
	if (teder_clear_next(NULL, &invalid_target, 0, &invalid_request) != TEDER_INVALID ||
	    invalid_request != 0)
		return 24;
	teder_snapshot_info snapshot = {0};
	const float *bands = NULL;
	const teder_span *spans = NULL;
	uint32_t count = 99;
	if (teder_snapshot_get_info(NULL, &snapshot) != TEDER_INVALID ||
	    teder_snapshot_bands(NULL, &bands, &count) != TEDER_INVALID ||
	    teder_snapshot_spans(NULL, &spans, &count) != TEDER_INVALID || count != 99)
		return 29;
	teder_snapshot_release(NULL);
	teder_config config = {0};
	config.struct_size = sizeof(config);
	config.abi_version = TEDER_ABI_VERSION;
	teder_engine *engine = NULL;
	if (teder_create(&config, &engine) != TEDER_OK || !engine)
		return 3;
	if (teder_get_config(engine, &config) != TEDER_OK || config.request_capacity != 32 ||
	    config.snapshot_slots != 4 || config.snapshot_loans != 16 || config.max_bands != 256 ||
	    config.max_spans != 16)
		return 4;
	teder_status status = {0};
	status.struct_size = sizeof(status);
	status.abi_version = TEDER_ABI_VERSION;
	if (status_read(engine, &status) != TEDER_OK || status.lifecycle != TEDER_LIVE ||
	    status.transport != TEDER_EMPTY || status.gain != 1.0f || status.analysis_sequence != 0 ||
	    status.analysis_dropped_frames != 0 || status.analysis_dropped_publications != 0)
		return 5;
	if (argc != 2)
		return 13;
	uint64_t id;
	teder_code code;
	teder_analysis_config analysis = {.struct_size = sizeof(analysis),
	                                  .abi_version = TEDER_ABI_VERSION,
	                                  .enabled = 1,
	                                  .bands = 64};
	for (unsigned enabled = 1;; enabled = 0) {
		analysis.enabled = enabled;
		do {
			code = teder_configure_analysis(engine, &analysis, &id);
		} while (code == TEDER_BUSY);
		teder_request_info configured = {.struct_size = sizeof(configured),
		                                 .abi_version = TEDER_ABI_VERSION};
		if (code != TEDER_OK || completed(engine, id, &configured) != TEDER_OK ||
		    configured.operation != TEDER_OP_ANALYSIS)
			return 30;
		do {
			code = teder_request_ack(engine, id);
		} while (code == TEDER_BUSY);
		teder_snapshot *loan = NULL;
		if (code != TEDER_OK || teder_snapshot_acquire(engine, &loan) != TEDER_AGAIN || loan)
			return 31;
		if (!enabled)
			break;
	}
	do {
		code = teder_open(engine, argv[1], (uint32_t)strlen(argv[1]), 42, 0, &id);
		if (code == TEDER_BUSY)
			tick();
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK)
		return 14;
	teder_request_info opened = {.struct_size = sizeof(opened), .abi_version = TEDER_ABI_VERSION};
	if (completed(engine, id, &opened) != TEDER_OK || opened.committed.source != 1 ||
	    opened.committed.epoch != 2 || opened.track != 42)
		return 15;
	do {
		code = teder_request_ack(engine, id);
		if (code == TEDER_BUSY)
			tick();
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK || status_read(engine, &status) != TEDER_OK ||
	    status.transport != TEDER_READY || status.duration_frames != 16 ||
	    status.source_rate != 44100)
		return 16;
	do {
		code = teder_seek(engine, &opened.committed, 7, &id);
		if (code == TEDER_BUSY)
			tick();
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK || completed(engine, id, &opened) != TEDER_OK ||
	    opened.committed.epoch != 3 || opened.committed.source != 1)
		return 17;
	do {
		code = teder_request_ack(engine, id);
		if (code == TEDER_BUSY)
			tick();
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK || status_read(engine, &status) != TEDER_OK || status.source_frame != 7)
		return 18;
	do {
		code = teder_probe(engine, argv[1], (uint32_t)strlen(argv[1]), TEDER_PROBE_ARTWORK, &id);
		if (code == TEDER_BUSY)
			tick();
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK || completed(engine, id, &opened) != TEDER_OK ||
	    opened.result_kind != TEDER_RESULT_PROBE)
		return 25;
	teder_result *metadata = NULL;
	do {
		code = teder_request_take_result(engine, id, &metadata);
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK || !metadata)
		return 26;
	do {
		code = teder_request_ack(engine, id);
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK)
		return 27;
	if (teder_destroy(&engine) != TEDER_BUSY || !engine)
		return 6;
	if (teder_request_close(engine) != TEDER_OK)
		return 7;
	/* A UI schedules these polls; this standalone consumer waits off the UI. */
	do {
		if (status_read(engine, &status) != TEDER_OK)
			return 8;
		if (status.lifecycle != TEDER_CLOSED_STATE)
			tick();
	} while (status.lifecycle != TEDER_CLOSED_STATE);
	teder_request_info request = {0};
	request.struct_size = sizeof(request);
	request.abi_version = TEDER_ABI_VERSION;
	if (teder_request_poll(engine, &request) != TEDER_AGAIN)
		return 10;
	if (teder_request_get(engine, 1, &request) != TEDER_NOT_FOUND)
		return 11;
	if (teder_request_ack(engine, 1) != TEDER_NOT_FOUND)
		return 12;
	if (teder_destroy(&engine) != TEDER_OK || engine)
		return 9;
	teder_probe_info probe = {.struct_size = sizeof(probe), .abi_version = TEDER_ABI_VERSION};
	if (teder_result_probe(metadata, &probe) != TEDER_OK || probe.codec != TEDER_CODEC_FLAC ||
	    probe.sample_rate != 44100 || probe.channels != 2 || probe.bits_per_sample != 16 ||
	    !probe.duration_known || probe.duration_frames != 16)
		return 28;
	teder_result_release(metadata);
	teder_result_release(NULL);
	return 0;
}
