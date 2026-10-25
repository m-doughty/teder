#ifndef TEDER_INTERNAL_H
#define TEDER_INTERNAL_H
#include "teder.h"
#include "memory.h"
#include <string.h>

enum { TD_ENGINE = 0x5444454e, TD_RESULT = 0x54445245 };
typedef struct { uint32_t kind, version; } td_handle;
/* Reading a prefix does not certify an address or a released handle. */
static inline teder_code td_prefix(const void *p, size_t size)
{
	uint32_t prefix[2];
	if (!p) return TEDER_INVALID;
	memcpy(prefix, p, sizeof(prefix));
	return prefix[0] == size && prefix[1] == TEDER_ABI_VERSION ? TEDER_OK : TEDER_ABI_MISMATCH;
}
static inline teder_code td_handle_check(const void *p, uint32_t kind)
{
	td_handle header;
	if (!p) return TEDER_INVALID;
	memcpy(&header, p, sizeof(header));
	if (header.version != TEDER_ABI_VERSION) return TEDER_ABI_MISMATCH;
	return header.kind == kind ? TEDER_OK : TEDER_BAD_STATE;
}
typedef struct { teder_text key, value; } td_tag;
typedef struct {
	uint32_t kind, device_count;
	const teder_probe_info *probe;
	const td_tag *tags;
	const teder_artwork *artwork;
	const teder_device_info *devices;
} td_result_input;
struct teder_result {
	td_handle header;
	td_arena *arena;
	size_t bytes;
	uint32_t kind, device_count;
	teder_probe_info probe;
	td_tag *tags;
	teder_artwork *artwork;
	teder_device_info *devices;
};
typedef struct {
	teder_request_info info;
	teder_result *result;
	uint8_t *input;
} td_request_slot;
struct teder_engine {
	td_handle header;
	td_arena *arena;
	teder_config config;
	teder_status status;
	size_t owned_bytes;
	td_request_slot *requests;
	teder_event *events;
	float *pcm;
};
/* Private seams use the same implementation as public creation. Allocator
 * contexts outlive all resulting arenas; no global allocator replacement.
 * Input structs and their views remain valid and immutable throughout creation. */
teder_code td_engine_create(const teder_config *, td_allocator, teder_engine **);
teder_code td_result_create(const td_result_input *, td_allocator, uint64_t limit, teder_result **);
/* Slot ownership only, not request admission/state/ID validation.
 * Attach consumes *owner only on success. Take clears the slot exactly once.
 * Producers must budget construction before allocation; attach is a second
 * check at transfer. All access is serialized with the engine owner. */
teder_code td_engine_attach_result(teder_engine *, uint32_t slot, teder_result **owner);
teder_code td_engine_take_result(teder_engine *, uint32_t slot, teder_result **out);
#endif
