#ifndef TEDER_DEMO_H
#define TEDER_DEMO_H
#include <teder.h>
#include <stdbool.h>

#define DEMO_INIT(type) {.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION}

/* One frame-thread owner; paths borrow argv until finish. No private library API. */
typedef struct demo {
	teder_engine *engine;
	teder_result *metadata;
	teder_snapshot *snapshot;
	teder_status status;
	teder_probe_info probe;
	teder_request_info completion;
	const char *path, *next;
	uint64_t pending, prepared_source;
	unsigned stage;
	bool native, taken, closing;
	teder_code outcome;
} demo;

uint64_t demo_millis(void);
void demo_wait(unsigned ms);
teder_code demo_start(demo *d, const char *path, const char *next, bool native);
teder_code demo_issue(demo *d, char command, uint64_t frame);
teder_code demo_tick(demo *d);
const char *demo_note(const demo *d);
/* Outside the frame loop: close/join before releasing independent results/loans. */
int demo_finish(demo *d);
#endif
