#include "internal.h"

teder_code td_engine_create_with(const teder_config *input, td_allocator allocator, td_runtime runtime, teder_engine **out)
{
	teder_config config;
	teder_code code;
	size_t capacity = 0, total, handle, requests, payload, events, pcm;
	if (!out) return TEDER_INVALID;
	*out = NULL;
	code = td_prefix(input, sizeof(*input));
	if (code != TEDER_OK) return code;
	if (!allocator.allocate || !allocator.release) return TEDER_INVALID;
	memcpy(&config, input, sizeof(config));
#define DEFAULT(field, value) if (!config.field) config.field = (value)
	DEFAULT(request_capacity, 32); DEFAULT(event_capacity, 64);
	DEFAULT(max_input_bytes, 4096); DEFAULT(pcm_frames, 65536);
	DEFAULT(snapshot_slots, 4); DEFAULT(snapshot_loans, 16);
	DEFAULT(max_bands, 256); DEFAULT(max_spans, 16);
	DEFAULT(engine_bytes_limit, UINT64_C(64) * 1024 * 1024);
	DEFAULT(result_bytes_limit, UINT64_C(16) * 1024 * 1024);
#undef DEFAULT
	if (!td_layout_add(&capacity, 1, sizeof(teder_engine), _Alignof(teder_engine), &handle) ||
		!td_layout_add(&capacity, config.request_capacity, sizeof(td_request_slot), _Alignof(td_request_slot), &requests) ||
		!td_layout_add(&capacity, config.request_capacity, config.max_input_bytes, 1, &payload) ||
		!td_layout_add(&capacity, config.event_capacity, sizeof(teder_event), _Alignof(teder_event), &events) ||
		!td_layout_add(&capacity, config.pcm_frames, 2 * sizeof(float), _Alignof(float), &pcm) ||
		!td_arena_size(capacity, &total) || total > config.engine_bytes_limit) return TEDER_LIMIT;
	td_arena *arena = td_arena_create(allocator, capacity);
	if (!arena) return TEDER_NO_MEMORY;
	/* One reserved region: no per-slot allocation and no publication until ready. */
	unsigned char *storage = td_arena_alloc(arena, capacity, 1, _Alignof(max_align_t));
	if (!storage) { td_arena_destroy(arena); return TEDER_INTERNAL; }
	memset(storage, 0, capacity);
	teder_engine *engine = (teder_engine *)(storage + handle);
	engine->header = (td_handle){TD_ENGINE, TEDER_ABI_VERSION};
	engine->arena = arena; engine->config = config; engine->owned_bytes = total;
	engine->requests = (td_request_slot *)(storage + requests);
	engine->events = (teder_event *)(storage + events); engine->pcm = (float *)(storage + pcm);
	for (uint32_t i = 0; i < config.request_capacity; i++)
		engine->requests[i].input = storage + payload + (size_t)i * config.max_input_bytes;
	engine->status.struct_size = sizeof(engine->status);
	engine->status.abi_version = TEDER_ABI_VERSION;
	engine->status.lifecycle = TEDER_LIVE; engine->status.transport = TEDER_EMPTY;
	engine->status.current.epoch = 1; engine->status.gain = 1.0f;
	engine->allocator = allocator; engine->runtime = runtime;
	atomic_init(&engine->lifecycle, TEDER_LIVE);
	code = td_control_start(engine);
	if (code != TEDER_OK) { td_arena_destroy(arena); return code; }
	*out = engine;
	return TEDER_OK;
}
teder_code td_engine_create(const teder_config *input, td_allocator allocator, teder_engine **out)
{
	return td_engine_create_with(input, allocator, (td_runtime){0}, out);
}
teder_code TEDER_CALL teder_create(const teder_config *config, teder_engine **out)
{
	return td_engine_create(config, td_libc_allocator(), out);
}
teder_code TEDER_CALL teder_get_config(teder_engine *engine, teder_config *out)
{
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code == TEDER_OK) code = td_prefix(out, sizeof(*out));
	if (code == TEDER_OK) memcpy(out, &engine->config, sizeof(*out));
	return code;
}
teder_code TEDER_CALL teder_get_status(teder_engine *engine, teder_status *out)
{
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code == TEDER_OK) code = td_prefix(out, sizeof(*out));
	if (code != TEDER_OK) return code;
	if (!td_try_lock(engine)) return TEDER_AGAIN;
	memcpy(out, &engine->status, sizeof(*out));
	out->lifecycle = atomic_load(&engine->lifecycle);
	td_unlock(engine);
	return code;
}
teder_code TEDER_CALL teder_request_close(teder_engine *engine)
{
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code != TEDER_OK) return code;
	unsigned live = TEDER_LIVE;
	if (atomic_compare_exchange_strong(&engine->lifecycle, &live, TEDER_CLOSING)) td_signal(engine);
	return TEDER_OK;
}
teder_code TEDER_CALL teder_destroy(teder_engine **owner)
{
	if (!owner) return TEDER_INVALID;
	if (!*owner) return TEDER_OK;
	teder_engine *engine = *owner;
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code != TEDER_OK) return code;
	if (atomic_load(&engine->lifecycle) != TEDER_CLOSED_STATE) return TEDER_BUSY;
	td_control_join(engine);
	for (uint32_t i = 0; i < engine->config.request_capacity; i++)
		teder_result_release(engine->requests[i].result);
	td_control_dispose(engine);
	td_arena_destroy(engine->arena);
	*owner = NULL;
	return TEDER_OK;
}
teder_code td_engine_attach_result(teder_engine *engine, uint32_t slot, teder_result **owner)
{
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code != TEDER_OK) return code;
	if (!owner) return TEDER_INVALID;
	code = td_handle_check(*owner, TD_RESULT);
	if (code != TEDER_OK) return code;
	if (atomic_load(&engine->lifecycle) != TEDER_LIVE) return TEDER_CLOSED;
	if (slot >= engine->config.request_capacity) return TEDER_NOT_FOUND;
	if (engine->requests[slot].result) return TEDER_BUSY;
	size_t total;
	if ((*owner)->bytes > engine->config.result_bytes_limit ||
		!td_size_add(engine->owned_bytes, (*owner)->bytes, &total) ||
		total > engine->config.engine_bytes_limit) return TEDER_LIMIT;
	engine->requests[slot].result = *owner; engine->owned_bytes = total;
	*owner = NULL;
	return TEDER_OK;
}
teder_code td_engine_take_result(teder_engine *engine, uint32_t slot, teder_result **out)
{
	if (!out) return TEDER_INVALID;
	*out = NULL;
	teder_code code = td_handle_check(engine, TD_ENGINE);
	if (code != TEDER_OK) return code;
	if (slot >= engine->config.request_capacity) return TEDER_NOT_FOUND;
	teder_result *result = engine->requests[slot].result;
	if (!result) return TEDER_NOT_FOUND;
	engine->requests[slot].result = NULL; engine->owned_bytes -= result->bytes;
	*out = result;
	return TEDER_OK;
}
