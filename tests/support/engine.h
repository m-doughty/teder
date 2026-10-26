#ifndef TEDER_TEST_ENGINE_H
#define TEDER_TEST_ENGINE_H
#include "internal.h"
#ifndef _WIN32
#include <sched.h>
#endif
static void th_yield(void)
{
#ifdef _WIN32
	SwitchToThread();
#else
	sched_yield();
#endif
}

/* Poll observed state, not elapsed-time assumptions. CTest owns the timeout;
 * a timeout kills the process rather than freeing worker-visible storage. */
static teder_code th_read_status(teder_engine *e, teder_status *out)
{
	teder_code code;
	while ((code = teder_get_status(e, out)) == TEDER_AGAIN)
		th_yield();
	return code;
}

static teder_code th_close(teder_engine *e)
{
	teder_code code = teder_request_close(e);
	if (code != TEDER_OK)
		return code;
	teder_status s = {.struct_size = sizeof(s), .abi_version = TEDER_ABI_VERSION};
	do {
		code = th_read_status(e, &s);
		if (code != TEDER_OK)
			return code;
		if (s.lifecycle != TEDER_CLOSED_STATE)
			th_yield();
	} while (s.lifecycle != TEDER_CLOSED_STATE);
	return TEDER_OK;
}
#endif
