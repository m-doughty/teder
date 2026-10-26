#ifndef TEDER_METADATA_H
#define TEDER_METADATA_H
#include "internal.h"
#include "file.h"

enum { TD_METADATA_TAGS = 1024, TD_METADATA_ARTWORK = 16, TD_METADATA_BLOCKS = 256 };
typedef struct {
	uint32_t flags;
	size_t work_bytes;
	uint64_t result_bytes;
	td_file_config file;
} td_metadata_config;

/* Worker-only. Reserve work_bytes + result_bytes before calling. The plan
 * includes one scratch arena and an 8 MiB codec cap; no allocation scales with
 * audio duration. Config/allocator contexts outlive this call and its result.
 * Success returns one detached result owner. Failure leaves *out NULL.
 * All file/codec/scratch resources are released before either return path. */
teder_code td_metadata_plan(uint32_t path_bytes, size_t *work_bytes);
teder_code td_metadata_read(
    const char *, uint32_t, const td_metadata_config *, td_allocator, teder_result **out);
#endif
