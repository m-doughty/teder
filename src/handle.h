#ifndef TEDER_HANDLE_H
#define TEDER_HANDLE_H
#include "teder.h"
#include <stddef.h>
#include <string.h>

enum {
	TD_ENGINE = 0x5444454e,
	TD_RESULT = 0x54445245,
	TD_SNAPSHOT = 0x5444534e,
	TD_RENDERER = 0x54445244
};

typedef struct {
	uint32_t kind, version;
} td_handle;

/* Reading a prefix does not certify an address or a released handle. */
static inline teder_code td_prefix(const void *p, size_t size)
{
	uint32_t prefix[2];
	if (!p)
		return TEDER_INVALID;
	memcpy(prefix, p, sizeof(prefix));
	return prefix[0] == size && prefix[1] == TEDER_ABI_VERSION ? TEDER_OK : TEDER_ABI_MISMATCH;
}

static inline teder_code td_handle_check(const void *p, uint32_t kind)
{
	td_handle header;
	if (!p)
		return TEDER_INVALID;
	memcpy(&header, p, sizeof(header));
	if (header.version != TEDER_ABI_VERSION)
		return TEDER_ABI_MISMATCH;
	return header.kind == kind ? TEDER_OK : TEDER_BAD_STATE;
}

#endif
