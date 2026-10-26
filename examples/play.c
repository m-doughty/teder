#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <teder.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
/* This standalone CLI may wait. UI clients submit and poll from their own loop. */
static void tick(void)
{
#ifdef _WIN32
	Sleep(10);
#else
	struct timespec delay = {0, 10000000};
	nanosleep(&delay, NULL);
#endif
}

static teder_code completed(teder_engine *e, uint64_t id)
{
	teder_request_info r = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &r);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			return code;
		if (code == TEDER_OK && r.state == TEDER_TERMINAL)
			return r.outcome;
		tick();
	}
}

static teder_code choose(teder_engine *e, const char *index)
{
	uint64_t id;
	teder_code code;
	do {
		code = teder_enumerate_devices(e, &id);
	} while (code == TEDER_BUSY);
	if (code == TEDER_OK)
		code = completed(e, id);
	if (code != TEDER_OK)
		return code;
	teder_result *devices = NULL;
	do {
		code = teder_request_take_result(e, id, &devices);
	} while (code == TEDER_BUSY);
	if (code != TEDER_OK)
		return code;
	uint32_t count;
	code = teder_result_device_count(devices, &count);
	char *end = NULL;
	unsigned long wanted = index ? strtoul(index, &end, 10) : 0;
	if (code == TEDER_OK && index && (!*index || *end || wanted >= count))
		code = TEDER_INVALID;
	teder_device_info selected = INIT(teder_device_info);
	bool found = false;
	for (uint32_t i = 0; code == TEDER_OK && i < count; i++) {
		teder_device_info item = INIT(teder_device_info);
		code = teder_result_device(devices, i, &item);
		if (code != TEDER_OK)
			break;
		printf("%u%s: ", i, item.is_default ? " (default)" : "");
		fwrite(item.name.data, 1, (size_t)item.name.bytes, stdout);
		putchar('\n');
		if ((index && i == wanted) || (!index && item.is_default && !found)) {
			selected = item;
			found = true;
		}
	}
	if (code == TEDER_OK && !found) {
		fputs("No default output reported; provide a device index.\n", stderr);
		code = TEDER_NOT_FOUND;
	}
	if (code == TEDER_OK) {
		fputs("Selected: ", stdout);
		fwrite(selected.name.data, 1, (size_t)selected.name.bytes, stdout);
		putchar('\n');
		do {
			code = teder_select_device(e, selected.key.data, selected.key.bytes, &id);
		} while (code == TEDER_BUSY);
	}
	teder_result_release(devices); /* select copied the key during admission */
	return code == TEDER_OK ? completed(e, id) : code;
}

static int play(const char *path, const char *index)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	teder_engine *e = NULL;
	teder_config config = INIT(teder_config);
	teder_code code = teder_create(&config, &e);
	if (code != TEDER_OK)
		goto done;
	code = choose(e, index);
	if (code != TEDER_OK)
		goto done;
	teder_status s = INIT(teder_status);
	while ((code = teder_get_status(e, &s)) == TEDER_AGAIN)
		tick();
	if (code != TEDER_OK)
		goto done;
	printf("Backend=%u rate=%u channels=%u period=%u\n",
	       s.backend,
	       s.output_rate,
	       s.output_channels,
	       s.output_period_frames);
	uint64_t id;
	do {
		code = teder_open(e, path, (uint32_t)strlen(path), 1, TEDER_OPEN_AUTOPLAY, &id);
	} while (code == TEDER_BUSY);
	if (code == TEDER_OK)
		code = completed(e, id);
	if (code != TEDER_OK)
		goto done;
	puts("Playing...");
	fflush(stdout);
	for (;;) {
		code = teder_get_status(e, &s);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			break;
		if (code == TEDER_OK &&
		    (s.transport == TEDER_FAILED || s.output_state == TEDER_OUTPUT_LOST)) {
			code = s.last_error;
			break;
		}
		if (code == TEDER_OK && s.transport == TEDER_ENDED) {
			puts("Final PCM submitted. Press Enter after the sound finishes to close.");
			fflush(stdout);
			getchar(); /* no guessed device latency or premature stop at the final callback */
			break;
		}
		tick();
	}
done:
	if (code != TEDER_OK)
		fprintf(stderr, "teder error: %d\n", code);
	if (e) {
		teder_request_close(e);
		teder_status closed = INIT(teder_status);
		while (teder_get_status(e, &closed) != TEDER_OK || closed.lifecycle != TEDER_CLOSED_STATE)
			tick();
		if (teder_destroy(&e) != TEDER_OK)
			return 1;
	}
	return code == TEDER_OK ? 0 : 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
	char path[32768], index[32];
	SetConsoleOutputCP(CP_UTF8);
	if (argc < 2 || argc > 3) {
		fputs("Usage: teder_play ABSOLUTE_FLAC_PATH [DEVICE_INDEX]\n", stderr);
		return 2;
	}
	if (!WideCharToMultiByte(
	        CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1, path, sizeof(path), NULL, NULL) ||
	    (argc == 3 &&
	     !WideCharToMultiByte(
	         CP_UTF8, WC_ERR_INVALID_CHARS, argv[2], -1, index, sizeof(index), NULL, NULL)))
		return 2;
	return play(path, argc == 3 ? index : NULL);
}
#else
int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		fputs("Usage: teder_play ABSOLUTE_FLAC_PATH [DEVICE_INDEX]\n", stderr);
		return 2;
	}
	return play(argv[1], argc == 3 ? argv[2] : NULL);
}
#endif
