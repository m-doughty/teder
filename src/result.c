#include "internal.h"
#include <assert.h>

typedef struct { td_arena *arena; size_t used; uint64_t limit; teder_code code; } pack;
static void *reserve(pack *p, size_t count, size_t size, size_t alignment)
{
	size_t offset, total;
	if (p->code != TEDER_OK || !count) return NULL;
	if (!td_layout_add(&p->used, count, size, alignment, &offset) ||
		!td_arena_size(p->used, &total) || total > p->limit) { p->code = TEDER_LIMIT; return NULL; }
	if (!p->arena) return NULL;
	void *data = td_arena_alloc(p->arena, count, size, alignment);
	if (!data) p->code = TEDER_INTERNAL;
	return data;
}
static bool utf8(const unsigned char *s, size_t bytes)
{
	for (size_t i = 0; i < bytes;) {
		uint32_t cp = s[i++], minimum;
		size_t more;
		if (cp < 0x80) continue;
		if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; more = 1; minimum = 0x80; }
		else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0xf; more = 2; minimum = 0x800; }
		else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; more = 3; minimum = 0x10000; }
		else return false;
		if (more > bytes - i) return false;
		while (more--) {
			unsigned c = s[i++];
			if ((c & 0xc0) != 0x80) return false;
			cp = (cp << 6) | (c & 0x3f);
		}
		if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
	}
	return true;
}
static teder_blob blob(pack *p, const void *data, uint64_t bytes, bool text)
{
	teder_blob out = {0};
	if (p->code != TEDER_OK || !bytes) return out;
	if (!data) { p->code = TEDER_INVALID; return out; }
	if (bytes > SIZE_MAX) { p->code = TEDER_LIMIT; return out; }
	void *copy = reserve(p, (size_t)bytes, 1, 1);
	if (p->code != TEDER_OK) return out;
	if (text && !utf8(data, (size_t)bytes)) { p->code = TEDER_ENCODING; return out; }
	if (copy) memcpy(copy, data, (size_t)bytes);
	out.data = copy; out.bytes = bytes;
	return out;
}
static teder_text text(pack *p, teder_text input)
{
	teder_blob b = blob(p, input.data, input.bytes, true);
	return (teder_text){(const char *)b.data, (uint32_t)b.bytes};
}
static teder_result *build(pack *p, const td_result_input *in)
{
	teder_result *r = reserve(p, 1, sizeof(*r), _Alignof(teder_result));
	if (p->code != TEDER_OK) return r;
	if (r) { memset(r, 0, sizeof(*r)); r->kind = in->kind; }
	if (in->kind == TEDER_RESULT_PROBE) {
		p->code = td_prefix(in->probe, sizeof(*in->probe));
		if (p->code != TEDER_OK) return r;
		const teder_probe_info *info = in->probe;
		if (info->codec != TEDER_CODEC_FLAC || !info->sample_rate || info->sample_rate > 1048575 ||
			!info->channels || info->channels > 8 || info->bits_per_sample < 4 || info->bits_per_sample > 32 ||
			info->duration_known > 1 || info->seekable > 1 || (!info->duration_known && info->duration_frames) ||
			(info->tag_count && !in->tags) || (info->artwork_count && !in->artwork)) { p->code = TEDER_INVALID; return r; }
		td_tag *tags = reserve(p, info->tag_count, sizeof(*tags), _Alignof(td_tag));
		teder_artwork *art = reserve(p, info->artwork_count, sizeof(*art), _Alignof(teder_artwork));
		if (p->code != TEDER_OK) return r;
		if (r) { r->probe = *info; r->tags = tags; r->artwork = art; }
		for (uint32_t i = 0; i < info->tag_count && p->code == TEDER_OK; i++) {
			td_tag tag;
			tag.key = text(p, in->tags[i].key);
			tag.value = text(p, in->tags[i].value);
			if (tags) tags[i] = tag;
		}
		for (uint32_t i = 0; i < info->artwork_count && p->code == TEDER_OK; i++) {
			p->code = td_prefix(&in->artwork[i], sizeof(*art));
			if (p->code != TEDER_OK) break;
			teder_artwork a = in->artwork[i];
			if (a.reserved) { p->code = TEDER_INVALID; break; }
			a.mime = text(p, a.mime); a.description = text(p, a.description);
			a.encoded = blob(p, a.encoded.data, a.encoded.bytes, false);
			if (art) art[i] = a;
		}
	} else if (in->kind == TEDER_RESULT_DEVICES) {
		if (in->device_count && !in->devices) { p->code = TEDER_INVALID; return r; }
		teder_device_info *devices = reserve(p, in->device_count, sizeof(*devices), _Alignof(teder_device_info));
		if (p->code != TEDER_OK) return r;
		if (r) { r->devices = devices; r->device_count = in->device_count; }
		for (uint32_t i = 0; i < in->device_count && p->code == TEDER_OK; i++) {
			p->code = td_prefix(&in->devices[i], sizeof(*devices));
			if (p->code != TEDER_OK) break;
			teder_device_info d = in->devices[i];
			if (d.backend < TEDER_BACKEND_WASAPI || d.backend > TEDER_BACKEND_PULSE || d.is_default > 1 || !d.key.bytes) {
				p->code = TEDER_INVALID; break;
			}
			d.name = text(p, d.name); d.key = blob(p, d.key.data, d.key.bytes, false);
			if (devices) devices[i] = d;
		}
	} else p->code = TEDER_INVALID;
	return r;
}
teder_code td_result_create(const td_result_input *input, td_allocator allocator, uint64_t limit, teder_result **out)
{
	if (!out) return TEDER_INVALID;
	*out = NULL;
	if (!input || !allocator.allocate || !allocator.release) return TEDER_INVALID;
	pack measure = {NULL, 0, limit, TEDER_OK};
	build(&measure, input);
	if (measure.code != TEDER_OK) return measure.code;
	size_t total;
	if (!td_arena_size(measure.used, &total)) return TEDER_LIMIT;
	td_arena *arena = td_arena_create(allocator, measure.used);
	if (!arena) return TEDER_NO_MEMORY;
	pack copy = {arena, 0, limit, TEDER_OK};
	teder_result *result = build(&copy, input);
	if (copy.code != TEDER_OK || !result) {
		td_arena_destroy(arena);
		return copy.code != TEDER_OK ? copy.code : TEDER_INTERNAL;
	}
	result->header = (td_handle){TD_RESULT, TEDER_ABI_VERSION};
	result->arena = arena; result->bytes = total;
	*out = result;
	return TEDER_OK;
}
static teder_code kind(const teder_result *r, uint32_t expected)
{
	teder_code code = td_handle_check(r, TD_RESULT);
	if (code == TEDER_OK && r->kind != expected) code = TEDER_BAD_STATE;
	return code;
}
teder_code TEDER_CALL teder_result_kind(const teder_result *r, uint32_t *out)
{
	teder_code code = td_handle_check(r, TD_RESULT);
	if (!out) return TEDER_INVALID;
	if (code == TEDER_OK) *out = r->kind;
	return code;
}
teder_code TEDER_CALL teder_result_probe(const teder_result *r, teder_probe_info *out)
{
	teder_code code = kind(r, TEDER_RESULT_PROBE);
	if (code == TEDER_OK) code = td_prefix(out, sizeof(*out));
	if (code == TEDER_OK) memcpy(out, &r->probe, sizeof(*out));
	return code;
}
teder_code TEDER_CALL teder_result_tag(const teder_result *r, uint32_t index, teder_text *key, teder_text *value)
{
	teder_code code = kind(r, TEDER_RESULT_PROBE);
	if (!key || !value || key == value) return TEDER_INVALID;
	if (code != TEDER_OK) return code;
	if (index >= r->probe.tag_count) return TEDER_NOT_FOUND;
	*key = r->tags[index].key; *value = r->tags[index].value;
	return TEDER_OK;
}
teder_code TEDER_CALL teder_result_artwork(const teder_result *r, uint32_t index, teder_artwork *out)
{
	teder_code code = kind(r, TEDER_RESULT_PROBE);
	if (code == TEDER_OK) code = td_prefix(out, sizeof(*out));
	if (code != TEDER_OK) return code;
	if (index >= r->probe.artwork_count) return TEDER_NOT_FOUND;
	memcpy(out, &r->artwork[index], sizeof(*out));
	return TEDER_OK;
}
teder_code TEDER_CALL teder_result_device_count(const teder_result *r, uint32_t *out)
{
	teder_code code = kind(r, TEDER_RESULT_DEVICES);
	if (!out) return TEDER_INVALID;
	if (code == TEDER_OK) *out = r->device_count;
	return code;
}
teder_code TEDER_CALL teder_result_device(const teder_result *r, uint32_t index, teder_device_info *out)
{
	teder_code code = kind(r, TEDER_RESULT_DEVICES);
	if (code == TEDER_OK) code = td_prefix(out, sizeof(*out));
	if (code != TEDER_OK) return code;
	if (index >= r->device_count) return TEDER_NOT_FOUND;
	memcpy(out, &r->devices[index], sizeof(*out));
	return TEDER_OK;
}
void TEDER_CALL teder_result_release(teder_result *r)
{
	if (!r) return;
	assert(td_handle_check(r, TD_RESULT) == TEDER_OK);
	td_arena_destroy(r->arena);
}
