#include "metadata.h"
#include "flac_heap.h"
#include <FLAC/metadata.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	td_file file;
	td_flac_heap heap;
	teder_probe_info info;
	td_tag tags[TD_METADATA_TAGS];
	teder_artwork artwork[TD_METADATA_ARTWORK];
	teder_code error;
} metadata;

static teder_code layout(uint32_t bytes, size_t *size, size_t *path, size_t *native)
{
	*size = sizeof(metadata);
	if (!bytes || bytes > 32767 || !td_layout_add(size, bytes + 1, 1, 1, path) ||
	    !td_layout_add(size, bytes + 1, sizeof(wchar_t), _Alignof(wchar_t), native))
		return TEDER_LIMIT;
	return TEDER_OK;
}

teder_code td_metadata_plan(uint32_t bytes, size_t *work)
{
	if (!work)
		return TEDER_INVALID;
	size_t size, path, native;
	teder_code code = layout(bytes, &size, &path, &native);
	if (code == TEDER_OK &&
	    (!td_arena_size(size, &size) || !td_size_add(size, 8 * 1024 * 1024, work)))
		code = TEDER_LIMIT;
	return code;
}

static size_t read_data(void *out, size_t size, size_t count, FLAC__IOHandle handle)
{
	metadata *m = handle;
	if (!size || !count || m->error != TEDER_OK)
		return 0;
	size_t bytes, done = 0;
	if (!td_size_mul(size, count, &bytes)) {
		m->error = TEDER_LIMIT;
		return 0;
	}
	while (done < bytes && m->error == TEDER_OK) {
		size_t got = bytes - done;
		m->error = td_file_read(&m->file, (unsigned char *)out + done, &got);
		done += got;
		if (!got && m->error == TEDER_OK)
			m->error = TEDER_CORRUPT;
	}
	return done / size;
}

static int seek_data(FLAC__IOHandle handle, FLAC__int64 offset, int whence)
{
	metadata *m = handle;
	uint64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m->file.cursor : m->file.size;
	uint64_t magnitude = offset < 0 ? (uint64_t)(-(offset + 1)) + 1 : (uint64_t)offset;
	if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) ||
	    (offset < 0 ? magnitude > base : magnitude > UINT64_MAX - base))
		m->error = TEDER_CORRUPT;
	uint64_t position = offset < 0 ? base - magnitude : base + magnitude;
	if (position > m->file.size || position > INT64_MAX)
		m->error = TEDER_CORRUPT;
	if (m->error == TEDER_OK)
		m->error = td_file_seek(&m->file, position);
	return m->error == TEDER_OK ? 0 : -1;
}

static FLAC__int64 tell_data(FLAC__IOHandle handle)
{
	metadata *m = handle;
	return m->file.cursor <= INT64_MAX ? (FLAC__int64)m->file.cursor : -1;
}

static teder_code comments(metadata *m, const FLAC__StreamMetadata *block)
{
	const FLAC__StreamMetadata_VorbisComment *v = &block->data.vorbis_comment;
	if (v->num_comments > TD_METADATA_TAGS)
		return TEDER_LIMIT;
	if (!td_utf8(v->vendor_string.entry, v->vendor_string.length))
		return TEDER_ENCODING;
	uint64_t length = 8ull + v->vendor_string.length;
	m->info.tag_count = v->num_comments;
	for (uint32_t i = 0; i < v->num_comments; i++) {
		const FLAC__StreamMetadata_VorbisComment_Entry *entry = &v->comments[i];
		const unsigned char *equal = memchr(entry->entry, '=', entry->length);
		if (!equal || equal == entry->entry)
			return TEDER_CORRUPT;
		uint32_t key = (uint32_t)(equal - entry->entry);
		for (uint32_t k = 0; k < key; k++)
			if (entry->entry[k] < 0x20 || entry->entry[k] > 0x7d)
				return TEDER_CORRUPT;
		m->tags[i] = (td_tag){{(const char *)entry->entry, key},
		                      {(const char *)equal + 1, entry->length - key - 1}};
		length += 4ull + entry->length;
	}
	return length == block->length ? TEDER_OK : TEDER_CORRUPT;
}

static teder_code picture(metadata *m, const FLAC__StreamMetadata *block)
{
	if (m->info.artwork_count == TD_METADATA_ARTWORK)
		return TEDER_LIMIT;
	const FLAC__StreamMetadata_Picture *p = &block->data.picture;
	size_t mime = strlen(p->mime_type), description = strlen((const char *)p->description);
	/* The upstream API exposes these as C strings. Reject embedded NULs or
	 * incorrect lengths instead of silently returning truncated text. */
	if (32ull + mime + description + p->data_length != block->length)
		return TEDER_CORRUPT;
	for (size_t i = 0; i < mime; i++)
		if ((unsigned char)p->mime_type[i] < 0x20 || (unsigned char)p->mime_type[i] > 0x7e)
			return TEDER_ENCODING;
	if (!td_utf8(p->description, description))
		return TEDER_ENCODING;
	m->artwork[m->info.artwork_count++] =
	    (teder_artwork){.struct_size = sizeof(teder_artwork),
	                    .abi_version = TEDER_ABI_VERSION,
	                    .picture_type = p->type,
	                    .width = p->width,
	                    .height = p->height,
	                    .depth = p->depth,
	                    .colors = p->colors,
	                    .mime = {p->mime_type, (uint32_t)mime},
	                    .description = {(const char *)p->description, (uint32_t)description},
	                    .encoded = {p->data, p->data_length}};
	return TEDER_OK;
}

static teder_code collect(metadata *m, FLAC__Metadata_Iterator *iterator)
{
	uint64_t bytes = 4;
	unsigned blocks = 0;
	bool seen_comments = false;
	do {
		if (td_file_cancelled(&m->file))
			return TEDER_CANCELLED;
		if (++blocks > TD_METADATA_BLOCKS)
			return TEDER_LIMIT;
		const FLAC__StreamMetadata *b = FLAC__metadata_iterator_get_block(iterator);
		bytes += 4ull + b->length;
		teder_code code = TEDER_OK;
		if (b->type == FLAC__METADATA_TYPE_STREAMINFO) {
			if (blocks != 1 || b->length != 34)
				return TEDER_CORRUPT;
			const FLAC__StreamMetadata_StreamInfo *s = &b->data.stream_info;
			if (!s->sample_rate || s->sample_rate > 1048575 || !s->channels || s->channels > 8 ||
			    s->bits_per_sample < 4 || s->bits_per_sample > 32)
				return TEDER_UNSUPPORTED;
			m->info = (teder_probe_info){.struct_size = sizeof(m->info),
			                             .abi_version = TEDER_ABI_VERSION,
			                             .codec = TEDER_CODEC_FLAC,
			                             .sample_rate = s->sample_rate,
			                             .channels = s->channels,
			                             .bits_per_sample = s->bits_per_sample,
			                             .duration_frames = s->total_samples,
			                             .duration_known = s->total_samples != 0,
			                             .seekable = 1};
		} else if (b->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
			if (seen_comments)
				return TEDER_CORRUPT;
			seen_comments = true;
			code = comments(m, b);
		} else if (b->type == FLAC__METADATA_TYPE_PICTURE)
			code = picture(m, b);
		if (code != TEDER_OK)
			return code;
	} while (FLAC__metadata_iterator_next(iterator));
	return bytes == m->file.cursor ? TEDER_OK : TEDER_CORRUPT;
}

teder_code td_metadata_read(const char *path,
                            uint32_t bytes,
                            const td_metadata_config *config,
                            td_allocator allocator,
                            teder_result **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!config || config->flags & ~TEDER_PROBE_ARTWORK || config->file.read_size > 4096 ||
	    !allocator.allocate || !allocator.release)
		return TEDER_INVALID;
	if (bytes > 32767)
		return TEDER_LIMIT;
	teder_code code = td_path_validate(path, bytes);
	if (code != TEDER_OK)
		return code;
	size_t size, name, native, total;
	code = layout(bytes, &size, &name, &native);
	if (code != TEDER_OK || !td_arena_size(size, &total) || total > config->work_bytes)
		return TEDER_LIMIT;
	td_file file = {.config = config->file};
	if (td_file_cancelled(&file))
		return TEDER_CANCELLED;
	td_arena *arena = td_arena_create(allocator, size);
	if (!arena)
		return TEDER_NO_MEMORY;
	unsigned char *base = td_arena_alloc(arena, size, 1, _Alignof(max_align_t));
	memset(base, 0, size);
	metadata *m = (metadata *)base;
	m->file = file;
	if (!m->file.config.read_size)
		m->file.config.read_size = 4096;
	memcpy(base + name, path, bytes);
	td_flac_heap_init(&m->heap, allocator, config->work_bytes - total);
	m->error = td_file_open(&m->file, (const char *)base + name, bytes, base + native);
	unsigned char magic[4];
	if (m->error == TEDER_OK && read_data(magic, 1, 4, m) == 4 && memcmp(magic, "fLaC", 4))
		m->error = TEDER_UNSUPPORTED;
	td_flac_heap *previous = td_flac_scope(&m->heap);
	FLAC__Metadata_Chain *chain = NULL;
	FLAC__Metadata_Iterator *iterator = NULL;
	if (m->error == TEDER_OK) {
		chain = FLAC__metadata_chain_new();
		iterator = FLAC__metadata_iterator_new();
		FLAC__IOCallbacks io = {.read = read_data, .seek = seek_data, .tell = tell_data};
		if (!chain || !iterator || !FLAC__metadata_chain_read_with_callbacks(chain, m, io)) {
			if (m->error == TEDER_OK)
				m->error = TEDER_CORRUPT;
		} else {
			FLAC__metadata_iterator_init(iterator, chain);
			m->error = collect(m, iterator);
		}
	}
	if (m->heap.error != TEDER_OK)
		m->error = m->heap.error;
	if (m->error == TEDER_OK) {
		if (!(config->flags & TEDER_PROBE_ARTWORK))
			m->info.artwork_count = 0;
		td_result_input input = {
		    .kind = TEDER_RESULT_PROBE, .probe = &m->info, .tags = m->tags, .artwork = m->artwork};
		m->error = td_result_create(&input, allocator, config->result_bytes, out);
	}
	if (iterator)
		FLAC__metadata_iterator_delete(iterator);
	if (chain)
		FLAC__metadata_chain_delete(chain);
	td_flac_scope(previous);
	if (m->heap.live || m->heap.bytes)
		abort();
	td_file_close(&m->file);
	code = td_file_cancelled(&m->file) ? TEDER_CANCELLED : m->error;
	if (code == TEDER_OK && m->file.close_failed)
		code = TEDER_IO;
	td_arena_destroy(arena);
	if (code != TEDER_OK && *out) {
		teder_result_release(*out);
		*out = NULL;
	}
	return code;
}
