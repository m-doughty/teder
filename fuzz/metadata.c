#include "support.h"
#include "metadata.h"

/* Documented hostile-input outcomes. INVALID would mean the result builder
 * rejected the reader's own output; the allocator never returns NULL.
 * A generated seed with exact expectations in known-good.txt must instead succeed. */
static bool rejected(const fz_input *f, teder_code code)
{
	if (code == TEDER_CANCELLED)
		return atomic_load(&f->cancel) != 0;
	return code == TEDER_CORRUPT || code == TEDER_UNSUPPORTED || code == TEDER_IO ||
	       code == TEDER_LIMIT || code == TEDER_ENCODING;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fz_input f;
	if (!fz_begin(&f, data, size))
		return 0;
	fz_expect known;
	bool exact = fz_expect_find("metadata", data, size, &known);
	td_metadata_config c = {.flags = (f.control[0] & 1) ? 0 : TEDER_PROBE_ARTWORK,
	                        .result_bytes = (f.control[1] & 1) ? 128 : 1024 * 1024,
	                        .file = {.read_size = (f.control[1] & 2) ? 1 : 4096,
	                                 .cancel = &f.cancel,
	                                 .context = &f,
	                                 .checkpoint = fz_checkpoint}};
	uint32_t bytes = (uint32_t)strlen(f.path);
	FZ_CHECK(td_metadata_plan(bytes, &c.work_bytes) == TEDER_OK);
	if (f.control[1] & 4)
		c.work_bytes /= 2;
	teder_result *r = NULL;
	teder_code code = td_metadata_read(f.path, bytes, &c, fz_allocator(&f), &r);
	FZ_CHECK(f.peak <= c.work_bytes + c.result_bytes);
	fz_remove(&f); /* All codec/file scratch has gone; only the result may remain. */
	if (code == TEDER_OK) {
		FZ_CHECK(r && f.live == 1 && f.bytes == r->bytes && r->bytes <= c.result_bytes);
		teder_probe_info info = FZ_INIT(teder_probe_info);
		uint32_t kind;
		FZ_CHECK(teder_result_kind(r, &kind) == TEDER_OK && kind == TEDER_RESULT_PROBE);
		FZ_CHECK(teder_result_probe(r, &info) == TEDER_OK && info.codec == TEDER_CODEC_FLAC);
		FZ_CHECK(info.tag_count <= TD_METADATA_TAGS && info.artwork_count <= TD_METADATA_ARTWORK);
		FZ_CHECK(c.flags || !info.artwork_count);
		/* Digest of every tag and picture as a length-prefixed byte sequence. */
		uint64_t digest = FZ_HASH_INIT;
		teder_text key, value;
		for (uint32_t i = 0; i < info.tag_count; i++) {
			FZ_CHECK(teder_result_tag(r, i, &key, &value) == TEDER_OK);
			fz_touch(key.data, key.bytes);
			fz_touch(value.data, value.bytes);
			digest = fz_hash_u32(digest, key.bytes);
			digest = fz_hash_bytes(digest, key.data, key.bytes);
			digest = fz_hash_u32(digest, value.bytes);
			digest = fz_hash_bytes(digest, value.data, value.bytes);
		}
		FZ_CHECK(teder_result_tag(r, info.tag_count, &key, &value) == TEDER_NOT_FOUND);
		teder_artwork art = FZ_INIT(teder_artwork);
		for (uint32_t i = 0; i < info.artwork_count; i++) {
			FZ_CHECK(teder_result_artwork(r, i, &art) == TEDER_OK);
			fz_touch(art.mime.data, art.mime.bytes);
			fz_touch(art.description.data, art.description.bytes);
			fz_touch(art.encoded.data, art.encoded.bytes);
			digest = fz_hash_u32(digest, art.picture_type);
			digest = fz_hash_u32(digest, art.width);
			digest = fz_hash_u32(digest, art.height);
			digest = fz_hash_u32(digest, art.depth);
			digest = fz_hash_u32(digest, art.colors);
			digest = fz_hash_u32(digest, art.mime.bytes);
			digest = fz_hash_bytes(digest, art.mime.data, art.mime.bytes);
			digest = fz_hash_u32(digest, art.description.bytes);
			digest = fz_hash_bytes(digest, art.description.data, art.description.bytes);
			digest = fz_hash_u32(digest, art.encoded.bytes);
			digest = fz_hash_bytes(digest, art.encoded.data, art.encoded.bytes);
		}
		FZ_CHECK(teder_result_artwork(r, info.artwork_count, &art) == TEDER_NOT_FOUND);
		if (exact)
			FZ_CHECK(info.sample_rate == fz_expect_value(&known, "rate") &&
			         info.channels == fz_expect_value(&known, "channels") &&
			         info.bits_per_sample == fz_expect_value(&known, "bits") &&
			         info.duration_known &&
			         info.duration_frames == fz_expect_value(&known, "duration") &&
			         info.tag_count == fz_expect_value(&known, "tags") &&
			         info.artwork_count == fz_expect_value(&known, "artwork") &&
			         digest == fz_expect_value(&known, "digest"));
		teder_result_release(r);
	} else
		FZ_CHECK(!r && !exact && rejected(&f, code));
	fz_end(&f);
	if (exact)
		fz_expect_passed(&known);
	return 0;
}
