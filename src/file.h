#ifndef TEDER_FILE_H
#define TEDER_FILE_H
#include "teder.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

enum { TD_FILE_OPENED = 1, TD_FILE_READ, TD_FILE_CLOSED };
typedef struct {
	uint32_t read_size; /* 1..4096; each native read checks cancellation */
	const atomic_uint *cancel, *lifecycle;
	void *context;
	void (*checkpoint)(void *, unsigned);
} td_file_config;

typedef struct {
	intptr_t handle;
	uint64_t size, cursor;
	bool opened, close_failed;
	td_file_config config;
} td_file;

/* Caller owns descriptor/config and native path scratch until close returns.
 * No allocation; open borrows a validated NUL-terminated UTF-8 path and native
 * scratch for (path_bytes + 1) wchar_t values. Only regular files are accepted. */
teder_code td_path_validate(const char *, uint32_t);
bool td_file_cancelled(const td_file *);
teder_code td_file_open(td_file *, const char *, uint32_t path_bytes, void *native_path);
teder_code td_file_read(td_file *, void *, size_t *);
teder_code td_file_seek(td_file *, uint64_t);
void td_file_close(td_file *);
#ifdef TD_FILE_FAULTS
/* Test builds only; release builds compile no hook. Open calls this on its own
 * thread before each native file-information call. True fails that call, with the
 * hook having set errno or the Win32 last error as the native call would. */
bool td_file_fault(void);
#endif
#endif
