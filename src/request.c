#include "internal.h"

void td_limit_close(teder_engine *e)
{
	e->status.last_error = TEDER_LIMIT;
	teder_request_close(e);
}
teder_code td_submit(teder_engine *e, uint32_t operation, const void *input, uint32_t bytes, uint64_t *out)
{
	if (!out) return TEDER_INVALID;
	*out = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK) return code;
	if ((bytes && !input) || operation < TEDER_OP_OPEN || operation > TEDER_OP_ANALYSIS) return TEDER_INVALID;
	if (bytes > e->config.max_input_bytes) return TEDER_LIMIT;
	if (atomic_load(&e->lifecycle) != TEDER_LIVE) return TEDER_CLOSED;
	if (!e->runtime.execute) return TEDER_UNSUPPORTED;
	if (!td_try_lock(e)) return TEDER_BUSY;
	if (atomic_load(&e->lifecycle) != TEDER_LIVE) { code = TEDER_CLOSED; goto done; }
	if (e->last_request == UINT64_MAX) { td_limit_close(e); code = TEDER_LIMIT; goto done; }
	code = TEDER_LIMIT;
	for (uint32_t i = 0; i < e->config.request_capacity; i++) {
		td_request_slot *s = &e->requests[i];
		if (s->info.state || s->result) continue;
		if (bytes) memcpy(s->input, input, bytes);
		s->input_bytes = bytes; s->cancelled = false; s->committed = false;
		s->info = (teder_request_info){.struct_size = sizeof(s->info), .abi_version = TEDER_ABI_VERSION,
			.request = ++e->last_request, .operation = operation, .state = TEDER_QUEUED};
		*out = s->info.request; code = TEDER_OK;
		break;
	}
done:
	td_unlock(e);
	if (code == TEDER_OK) td_signal(e);
	return code;
}
static td_request_slot *find(teder_engine *e, uint64_t id)
{
	if (id) for (uint32_t i = 0; i < e->config.request_capacity; i++)
		if (e->requests[i].info.request == id) return &e->requests[i];
	return NULL;
}
static teder_code read_ready(teder_engine *e, void *out, size_t size)
{
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code == TEDER_OK) code = td_prefix(out, size);
	if (code != TEDER_OK) return code;
	return td_try_lock(e) ? TEDER_OK : TEDER_AGAIN;
}
teder_code TEDER_CALL teder_request_get(teder_engine *e, uint64_t id, teder_request_info *out)
{
	teder_code code = read_ready(e, out, sizeof(*out));
	if (code != TEDER_OK) return code;
	td_request_slot *s = find(e, id);
	if (s) memcpy(out, &s->info, sizeof(*out));
	td_unlock(e);
	return s ? TEDER_OK : TEDER_NOT_FOUND;
}
teder_code TEDER_CALL teder_request_poll(teder_engine *e, teder_request_info *out)
{
	teder_code code = read_ready(e, out, sizeof(*out));
	if (code != TEDER_OK) return code;
	td_request_slot *oldest = NULL;
	for (uint32_t i = 0; i < e->config.request_capacity; i++) {
		td_request_slot *s = &e->requests[i];
		if (s->info.state == TEDER_TERMINAL && (!oldest || s->completed < oldest->completed)) oldest = s;
	}
	if (oldest) memcpy(out, &oldest->info, sizeof(*out));
	td_unlock(e);
	return oldest ? TEDER_OK : TEDER_AGAIN;
}
static teder_code mutate_ready(teder_engine *e)
{
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK) return code;
	return td_try_lock(e) ? TEDER_OK : TEDER_BUSY;
}
teder_code TEDER_CALL teder_request_cancel(teder_engine *e, uint64_t id)
{
	teder_code code = mutate_ready(e);
	if (code != TEDER_OK) return code;
	td_request_slot *s = find(e, id);
	code = !s ? TEDER_NOT_FOUND : s->info.state == TEDER_TERMINAL ? TEDER_TOO_LATE : TEDER_OK;
	if (code == TEDER_OK) s->cancelled = true;
	td_unlock(e);
	return code;
}
teder_code TEDER_CALL teder_request_take_result(teder_engine *e, uint64_t id, teder_result **out)
{
	if (!out) return TEDER_INVALID;
	*out = NULL;
	teder_code code = mutate_ready(e);
	if (code != TEDER_OK) return code;
	td_request_slot *s = find(e, id);
	code = !s ? TEDER_NOT_FOUND : s->info.state != TEDER_TERMINAL ? TEDER_BUSY : TEDER_NOT_FOUND;
	if (s && s->info.state == TEDER_TERMINAL && s->result) {
		*out = s->result; s->result = NULL; e->owned_bytes -= (*out)->bytes; code = TEDER_OK;
	}
	td_unlock(e);
	return code;
}
teder_code TEDER_CALL teder_request_ack(teder_engine *e, uint64_t id)
{
	teder_code code = mutate_ready(e);
	if (code != TEDER_OK) return code;
	td_request_slot *s = find(e, id);
	teder_result *discard = NULL;
	code = !s ? TEDER_NOT_FOUND : s->info.state != TEDER_TERMINAL ? TEDER_BUSY : TEDER_OK;
	if (code == TEDER_OK) {
		discard = s->result; s->result = NULL;
		if (discard) {
			size_t bytes = discard->bytes;
			teder_result_release(discard); e->owned_bytes -= bytes;
		}
		s->info = (teder_request_info){0}; s->input_bytes = 0;
	}
	td_unlock(e);
	return code;
}
teder_code td_emit(teder_engine *e, const teder_event *input)
{
	teder_code code = td_prefix(input, sizeof(*input));
	if (code != TEDER_OK) return code;
	td_lock(e);
	if (atomic_load(&e->lifecycle) == TEDER_CLOSED_STATE) { td_unlock(e); return TEDER_CLOSED; }
	if (e->status.event_sequence == UINT64_MAX) { td_limit_close(e); td_unlock(e); return TEDER_LIMIT; }
	teder_event event = *input; event.sequence = ++e->status.event_sequence;
	if (e->event_count == e->config.event_capacity) {
		e->event_head = (e->event_head + 1) % e->config.event_capacity; e->event_count--;
		if (e->status.events_dropped != UINT64_MAX) e->status.events_dropped++;
	}
	size_t index = ((size_t)e->event_head + e->event_count) % e->config.event_capacity;
	e->events[index] = event; e->event_count++;
	td_unlock(e);
	return TEDER_OK;
}
teder_code TEDER_CALL teder_event_poll(teder_engine *e, teder_event *out)
{
	teder_code code = read_ready(e, out, sizeof(*out));
	if (code != TEDER_OK) return code;
	code = e->event_count ? TEDER_OK : TEDER_AGAIN;
	if (code == TEDER_OK) {
		*out = e->events[e->event_head]; e->event_count--;
		e->event_head = (e->event_head + 1) % e->config.event_capacity;
	}
	td_unlock(e);
	return code;
}
