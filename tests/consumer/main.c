#define _POSIX_C_SOURCE 200809L
#include <teder.h>
#include <teder_notcurses.h>
#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
static void tick(void) { Sleep(1); }
#else
#include <time.h>
static void tick(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
#endif
static teder_code status_read(teder_engine *e, teder_status *s)
{
	teder_code code;
	while ((code = teder_get_status(e, s)) == TEDER_AGAIN) tick();
	return code;
}
int main(void)
{
	teder_build_info info = {0};
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	if (teder_get_build_info(&info) != TEDER_OK) return 1;
	if (info.abi_version != 1 || info.capabilities != 0 || info.backend_mask != 0) return 2;
	teder_config config = {0};
	config.struct_size = sizeof(config); config.abi_version = TEDER_ABI_VERSION;
	teder_engine *engine = NULL;
	if (teder_create(&config, &engine) != TEDER_OK || !engine) return 3;
	if (teder_get_config(engine, &config) != TEDER_OK || config.request_capacity != 32) return 4;
	teder_status status = {0};
	status.struct_size = sizeof(status); status.abi_version = TEDER_ABI_VERSION;
	if (status_read(engine, &status) != TEDER_OK || status.lifecycle != TEDER_LIVE ||
		status.transport != TEDER_EMPTY || status.gain != 1.0f) return 5;
	if (teder_destroy(&engine) != TEDER_BUSY || !engine) return 6;
	if (teder_request_close(engine) != TEDER_OK) return 7;
	/* A UI schedules these polls; this standalone consumer waits off the UI. */
	do {
		if (status_read(engine, &status) != TEDER_OK) return 8;
		if (status.lifecycle != TEDER_CLOSED_STATE) tick();
	} while (status.lifecycle != TEDER_CLOSED_STATE);
	teder_request_info request = {0};
	request.struct_size = sizeof(request); request.abi_version = TEDER_ABI_VERSION;
	if (teder_request_poll(engine, &request) != TEDER_AGAIN) return 10;
	if (teder_request_get(engine, 1, &request) != TEDER_NOT_FOUND) return 11;
	if (teder_request_ack(engine, 1) != TEDER_NOT_FOUND) return 12;
	if (teder_destroy(&engine) != TEDER_OK || engine) return 9;
	teder_result_release(NULL);
	return 0;
}
