#define _GNU_SOURCE
#include <teder.h>
#include <teder_notcurses.h>
#include <stddef.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#endif

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expr); \
			failed = 1; \
			goto done; \
		} \
	} while (0)

/* Every address is used, requiring real linkable definitions as well as types. */
static int contract(void)
{
	_Static_assert(sizeof(void *) == 8 && sizeof(teder_code) == 4, "64-bit ABI");
	_Static_assert(_Generic((teder_code)0, int32_t: 1, default: 0), "signed result code");
	_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24, "binary32");
#define ABI_RECORD(type, size, align) \
	_Static_assert(sizeof(type) == size && _Alignof(type) == align, #type);
#define ABI_FIELD(type, field, offset, size, kind) \
	_Static_assert(offsetof(type, field) == offset && sizeof(((type *)0)->field) == size && \
	                   _Generic(((type *)0)->field, kind: 1, default: 0), \
	               #type "." #field);
#define ABI_VALUE(name, value) _Static_assert(name == value, #name);
#define ABI_FUNCTION(result, name, args) \
	_Static_assert(_Generic(&name, result(TEDER_CALL *) args: 1, default: 0), #name); \
	result(TEDER_CALL *volatile address_##name) args = &name; \
	if (!address_##name) \
		return 1;
#include "contract.h"
	return 0;
}

static void tick(void)
{
#ifdef _WIN32
	Sleep(1);
#else
	struct timespec delay = {0, 1000000};
	nanosleep(&delay, NULL);
#endif
}

int main(void)
{
	int failed = 0;
	teder_engine *engine = NULL;
	teder_status status = INIT(teder_status);
	teder_config config = INIT(teder_config);
	void *memory = NULL;
	size_t page;
#ifdef ABI_WITH_RENDERER
	teder_renderer *renderer = NULL;
	teder_renderer_config drawing = INIT(teder_renderer_config);
#endif
#ifdef _WIN32
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	page = system.dwPageSize;
	memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	CHECK(memory != NULL);
	DWORD old;
	CHECK(VirtualProtect((char *)memory + page, page, PAGE_NOACCESS, &old));
#else
	long size = sysconf(_SC_PAGESIZE);
	CHECK(size > 0);
	page = (size_t)size;
	memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
		memory = NULL;
	CHECK(memory != NULL);
	CHECK(mprotect((char *)memory + page, page, PROT_NONE) == 0);
#endif
	CHECK(contract() == 0);
	teder_build_info info = INIT(teder_build_info);
	CHECK(teder_get_build_info(&info) == TEDER_OK);
	CHECK(info.abi_version == 1 && info.capabilities == 15 && info.reserved == 0);
	CHECK(info.version_major == 0 && info.version_minor == 1 && info.version_patch == 0);
#ifdef _WIN32
	CHECK(info.backend_mask == 2);
#elif defined(__APPLE__)
	CHECK(info.backend_mask == 4);
#else
	CHECK(info.backend_mask == 24);
#endif
	CHECK(teder_create(&config, &engine) == TEDER_OK);
#ifdef ABI_WITH_RENDERER
	teder_build_info renderer_info = INIT(teder_build_info);
	CHECK(teder_renderer_get_build_info(&renderer_info) == TEDER_OK);
	CHECK(memcmp(&info, &renderer_info, sizeof(info)) == 0);
	CHECK(teder_renderer_create(&drawing, &renderer) == TEDER_OK);
#endif
	/* Only eight readable bytes. Any later access faults against the guard page. */
	void *prefix = (char *)memory + page - 8;
	for (unsigned variant = 0; variant < 4; ++variant) {
#define REJECT(type, call) \
	do { \
		uint32_t expected[2] = {sizeof(type), TEDER_ABI_VERSION}; \
		if (variant < 2) \
			expected[0] = variant ? sizeof(type) + 8 : sizeof(type) - 1; \
		else \
			expected[1] = variant == 2 ? 0 : 2; \
		memcpy(prefix, expected, sizeof(expected)); \
		CHECK((call) == TEDER_ABI_MISMATCH); \
		CHECK(memcmp(prefix, expected, sizeof(expected)) == 0); \
	} while (0)
		REJECT(teder_build_info, teder_get_build_info(prefix));
		teder_engine *rejected = engine;
		REJECT(teder_config, teder_create(prefix, &rejected));
		CHECK(rejected == NULL);
		REJECT(teder_config, teder_get_config(engine, prefix));
		REJECT(teder_status, teder_get_status(engine, prefix));
		REJECT(teder_request_info, teder_request_poll(engine, prefix));
		REJECT(teder_request_info, teder_request_get(engine, 0, prefix));
		REJECT(teder_event, teder_event_poll(engine, prefix));
		uint64_t request = 99;
		REJECT(teder_analysis_config, teder_configure_analysis(engine, prefix, &request));
		CHECK(request == 0);
#ifdef ABI_WITH_RENDERER
		REJECT(teder_build_info, teder_renderer_get_build_info(prefix));
		teder_renderer *rejected_renderer = renderer;
		REJECT(teder_renderer_config, teder_renderer_create(prefix, &rejected_renderer));
		CHECK(rejected_renderer == NULL);
		REJECT(teder_renderer_config, teder_renderer_configure(renderer, prefix));
#endif
	}
	teder_analysis_config analysis = INIT(teder_analysis_config);
	analysis.reserved = 1;
	uint64_t request = 99;
	CHECK(teder_configure_analysis(engine, &analysis, &request) == TEDER_INVALID && request == 0);
	CHECK(teder_destroy(&engine) == TEDER_BUSY && engine != NULL);
done:
#ifdef ABI_WITH_RENDERER
	teder_renderer_destroy(renderer);
#endif
	if (engine) {
		failed |= teder_request_close(engine) != TEDER_OK;
		while (teder_get_status(engine, &status) != TEDER_OK ||
		       status.lifecycle != TEDER_CLOSED_STATE)
			tick();
		failed |= teder_destroy(&engine) != TEDER_OK;
	}
	if (memory) {
#ifdef _WIN32
		failed |= !VirtualFree(memory, 0, MEM_RELEASE);
#else
		failed |= munmap(memory, page * 2) != 0;
#endif
	}
	if (!failed)
		puts("ABI 1 layouts, constants, linked signatures and guarded prefixes passed");
	return failed;
}
