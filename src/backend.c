#include "backend.h"
#include "internal.h"

enum { DEVICE_LIMIT = 128, KEY_HEADER = 16, KEY_LIMIT = KEY_HEADER + 256 };
struct td_backend {
	td_arena *arena;
	td_allocator allocator;
	td_backend_api api;
	td_backend_heap heap;
	ma_context native;
	uint64_t token;
	uint32_t backend, count, key_limit, borrowers;
	teder_code error;
	bool initialized;
	teder_device_info devices[DEVICE_LIMIT];
	unsigned char keys[DEVICE_LIMIT][KEY_LIMIT];
	char names[DEVICE_LIMIT][MA_MAX_DEVICE_NAME_LENGTH + 1];
};
static const ma_backend backends[] = {
    ma_backend_wasapi, ma_backend_coreaudio, ma_backend_alsa, ma_backend_pulseaudio};

teder_code td_backend_plan(size_t heap_limit, size_t *reservation)
{
	if (!reservation)
		return TEDER_INVALID;
	size_t bytes;
	return td_arena_size(sizeof(td_backend), &bytes) && td_size_add(bytes, heap_limit, reservation)
	           ? TEDER_OK
	           : TEDER_LIMIT;
}

teder_code td_backend_open(uint64_t token,
                           uint32_t preferred,
                           size_t heap_limit,
                           td_allocator allocator,
                           const td_backend_api *api,
                           td_backend **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!token || preferred > TEDER_BACKEND_PULSE || !allocator.allocate || !allocator.release ||
	    (api && (!api->init || !api->uninit || !api->enumerate)))
		return TEDER_INVALID;
	size_t reservation;
	teder_code code = td_backend_plan(heap_limit, &reservation);
	if (code != TEDER_OK)
		return code;
	td_arena *arena = td_arena_create(allocator, sizeof(td_backend));
	if (!arena)
		return TEDER_NO_MEMORY;
	td_backend *b = td_arena_alloc(arena, 1, sizeof(*b), _Alignof(td_backend));
	memset(b, 0, sizeof(*b));
	b->arena = arena;
	b->allocator = allocator;
	b->token = token;
	b->api = api ? *api
	             : (td_backend_api){
	                   ma_context_init, ma_context_uninit, ma_context_enumerate_devices, NULL};
	code = td_backend_heap_init(&b->heap, allocator, heap_limit);
	if (code != TEDER_OK) {
		td_arena_destroy(arena);
		return code;
	}
	ma_context_config config = ma_context_config_init();
	config.allocationCallbacks = td_backend_allocator(&b->heap);
	config.pUserData = b->api.context;
	ma_result result = b->api.init(
	    preferred ? &backends[preferred - 1] : backends, preferred ? 1 : 4, &config, &b->native);
	b->initialized = result == MA_SUCCESS;
	code = td_backend_heap_error(&b->heap);
	if (code == TEDER_OK && result != MA_SUCCESS)
		code = result == MA_NO_BACKEND ? TEDER_UNSUPPORTED : TEDER_DEVICE_LOST;
	if (b->initialized)
		for (unsigned i = 0; i < 4; i++)
			if (b->native.backend == backends[i])
				b->backend = i + 1;
	if (code == TEDER_OK && !b->backend)
		code = TEDER_UNSUPPORTED;
	if (code != TEDER_OK) {
		td_backend_close(&b);
		return code;
	}
	*out = b;
	return TEDER_OK;
}

static void token_bytes(unsigned char *out, uint64_t token)
{
	for (unsigned i = 0; i < 8; i++)
		out[i] = (unsigned char)(token >> (i * 8));
}

/* Only supported string members are serialized. No union padding or pointers. */
static uint32_t encode(td_backend *b, const ma_device_id *id, unsigned char *key)
{
	memcpy(key, "TDK1", 4);
	token_bytes(key + 4, b->token);
	key[12] = (unsigned char)b->backend;
	key[15] = 0;
	uint32_t bytes = 0;
	if (b->backend == TEDER_BACKEND_WASAPI) {
		for (unsigned i = 0; i < 64; i++) {
			unsigned value = id->wasapi[i];
			key[KEY_HEADER + bytes++] = (unsigned char)value;
			key[KEY_HEADER + bytes++] = (unsigned char)(value >> 8);
			if (!value)
				break;
		}
		if (id->wasapi[bytes / 2 - 1])
			return 0;
	} else {
		const char *name = b->backend == TEDER_BACKEND_COREAUDIO ? id->coreaudio
		                   : b->backend == TEDER_BACKEND_ALSA    ? id->alsa
		                                                         : id->pulse;
		do {
			key[KEY_HEADER + bytes] = (unsigned char)name[bytes];
		} while (name[bytes++] && bytes < 256);
		if (name[bytes - 1])
			return 0;
	}
	key[13] = (unsigned char)bytes;
	key[14] = (unsigned char)(bytes >> 8);
	return KEY_HEADER + bytes;
}

static ma_bool32
enumerate(ma_context *context, ma_device_type type, const ma_device_info *info, void *user)
{
	(void)context;
	td_backend *b = user;
	if (type != ma_device_type_playback)
		return MA_TRUE;
	if (b->count == DEVICE_LIMIT) {
		b->error = TEDER_LIMIT;
		return MA_FALSE;
	}
	unsigned i = b->count;
	uint32_t bytes = encode(b, &info->id, b->keys[i]);
	size_t length = 0;
	while (length < sizeof(info->name) && info->name[length])
		length++;
	if (!bytes || length == sizeof(info->name) || info->isDefault > 1)
		b->error = TEDER_INVALID;
	else if (bytes > b->key_limit)
		b->error = TEDER_LIMIT;
	if (b->error != TEDER_OK)
		return MA_FALSE;
	memcpy(b->names[i], info->name, length);
	b->devices[i] = (teder_device_info){sizeof(teder_device_info),
	                                    TEDER_ABI_VERSION,
	                                    b->backend,
	                                    info->isDefault,
	                                    {b->names[i], (uint32_t)length},
	                                    {b->keys[i], bytes}};
	b->count++;
	return MA_TRUE;
}

teder_code
td_backend_enumerate(td_backend *b, uint64_t limit, uint32_t key_limit, teder_result **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!b || !b->initialized)
		return TEDER_BAD_STATE;
	teder_code code = td_backend_heap_error(&b->heap);
	if (code != TEDER_OK)
		return code;
	b->count = 0;
	b->error = TEDER_OK;
	b->key_limit = key_limit;
	ma_result result = b->api.enumerate(&b->native, enumerate, b);
	code = td_backend_heap_error(&b->heap);
	if (code != TEDER_OK)
		return code;
	if (b->error != TEDER_OK)
		return b->error;
	if (result != MA_SUCCESS)
		return TEDER_DEVICE_LOST;
	td_result_input input = {
	    .kind = TEDER_RESULT_DEVICES, .device_count = b->count, .devices = b->devices};
	return td_result_create(&input, b->allocator, limit, out);
}

teder_code
td_backend_key_decode(uint64_t engine_token, uint32_t backend, teder_blob key, ma_device_id *out)
{
	if (!out || !key.data || key.bytes < KEY_HEADER || key.bytes > KEY_LIMIT)
		return TEDER_INVALID;
	const unsigned char *p = key.data;
	uint32_t bytes = p[13] | (uint32_t)p[14] << 8;
	if (memcmp(p, "TDK1", 4) || p[15] || !bytes || key.bytes != KEY_HEADER + bytes ||
	    p[12] < TEDER_BACKEND_WASAPI || p[12] > TEDER_BACKEND_PULSE)
		return TEDER_INVALID;
	ma_device_id id = {0};
	for (uint32_t i = 0; i < bytes;) {
		unsigned value = p[KEY_HEADER + i++];
		if (p[12] == TEDER_BACKEND_WASAPI) {
			if (bytes > 128 || bytes % 2)
				return TEDER_INVALID;
			value |= (unsigned)p[KEY_HEADER + i++] << 8;
			id.wasapi[i / 2 - 1] = (ma_wchar_win32)value;
		} else {
			char *name = p[12] == TEDER_BACKEND_COREAUDIO ? id.coreaudio
			             : p[12] == TEDER_BACKEND_ALSA    ? id.alsa
			                                              : id.pulse;
			name[i - 1] = (char)value;
		}
		if ((!value) != (i == bytes))
			return TEDER_INVALID;
	}
	unsigned char token[8];
	token_bytes(token, engine_token);
	if (!engine_token || memcmp(token, p + 4, 8) || (backend && p[12] != backend))
		return TEDER_STALE;
	*out = id;
	return TEDER_OK;
}

teder_code td_backend_key(td_backend *b, teder_blob key, ma_device_id *out)
{
	return b ? td_backend_key_decode(b->token, b->backend, key, out) : TEDER_INVALID;
}

teder_code td_backend_error(td_backend *b)
{
	return td_backend_heap_error(&b->heap);
}

teder_code td_backend_borrow(td_backend *b, teder_blob key, td_backend_loan *out)
{
	if (!b || !out || !b->initialized)
		return TEDER_INVALID;
	if (b->borrowers == UINT32_MAX)
		return TEDER_LIMIT;
	teder_code code = td_backend_error(b);
	if (code != TEDER_OK)
		return code;
	ma_device_id id;
	if (!key.data && !key.bytes) {
		ma_device_info info;
		/* ALSA's NULL query leaves info.id empty. Name its configured default PCM
		 * explicitly so the native wrapper copies the ID used for the later open. */
		ma_device_id alsa_default = {.alsa = "default"};
		const ma_device_id *requested = b->backend == TEDER_BACKEND_ALSA ? &alsa_default : NULL;
		ma_result result =
		    ma_context_get_device_info(&b->native, ma_device_type_playback, requested, &info);
		code = td_backend_error(b);
		if (code != TEDER_OK || result != MA_SUCCESS)
			return code != TEDER_OK ? code : TEDER_DEVICE_LOST;
		unsigned char encoded[KEY_LIMIT];
		uint32_t bytes = encode(b, &info.id, encoded);
		code = td_backend_key(b, (teder_blob){encoded, bytes}, &id);
	} else
		code = td_backend_key(b, key, &id);
	if (code != TEDER_OK)
		return code;
	*out = (td_backend_loan){&b->native, id, b->allocator, b->backend};
	b->borrowers++;
	return TEDER_OK;
}

void td_backend_return(td_backend *b)
{
	b->borrowers--;
}

teder_code td_backend_close(td_backend **owner)
{
	if (!owner || !*owner)
		return TEDER_INVALID;
	td_backend *b = *owner;
	if (b->borrowers)
		return TEDER_BUSY;
	ma_result result = MA_SUCCESS;
	if (b->initialized) {
		result = b->api.uninit(&b->native);
		b->initialized = false;
	}
	teder_code code = td_backend_heap_uninit(&b->heap);
	if (code != TEDER_OK)
		return code;
	td_arena_destroy(b->arena);
	*owner = NULL;
	return result == MA_SUCCESS ? TEDER_OK : TEDER_DEVICE_LOST;
}
