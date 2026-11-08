#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "file.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#endif
bool td_file_cancelled(const td_file *s)
{
	return (s->config.cancel && atomic_load_explicit(s->config.cancel, memory_order_relaxed)) ||
	       (s->config.lifecycle &&
	        atomic_load_explicit(s->config.lifecycle, memory_order_relaxed) != TEDER_LIVE);
}

static void checkpoint(td_file *s, unsigned point)
{
	if (s->config.checkpoint)
		s->config.checkpoint(s->config.context, point);
}

#ifdef TD_FILE_FAULTS
#define INFO_FAULT() td_file_fault()
#else
#define INFO_FAULT() false
#endif

teder_code td_path_validate(const char *path, uint32_t bytes)
{
	if (!path || !bytes || memchr(path, 0, bytes))
		return TEDER_INVALID;
	for (uint32_t i = 0; i < bytes;) {
		uint32_t cp = (unsigned char)path[i++], min, more;
		if (cp < 128)
			continue;
		if (cp >= 0xc2 && cp <= 0xdf) {
			cp &= 31;
			min = 128;
			more = 1;
		} else if (cp >= 0xe0 && cp <= 0xef) {
			cp &= 15;
			min = 2048;
			more = 2;
		} else if (cp >= 0xf0 && cp <= 0xf4) {
			cp &= 7;
			min = 65536;
			more = 3;
		} else
			return TEDER_ENCODING;
		if (more > bytes - i)
			return TEDER_ENCODING;
		while (more--) {
			unsigned c = (unsigned char)path[i++];
			if ((c & 192) != 128)
				return TEDER_ENCODING;
			cp = (cp << 6) | (c & 63);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return TEDER_ENCODING;
	}
#ifdef _WIN32
	bool drive = bytes >= 3 &&
	             ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	             path[1] == ':' && (path[2] == '/' || path[2] == '\\');
	bool unc = bytes >= 5 && path[0] == '\\' && path[1] == '\\' && path[2] != '.' &&
	           path[2] != '?' && path[2] != '\\';
	if (!drive && !unc)
		return TEDER_INVALID;
#else
	if (path[0] != '/')
		return TEDER_INVALID;
#endif
	return TEDER_OK;
}

teder_code td_file_open(td_file *s, const char *path, uint32_t bytes, void *native_path)
{
	if (td_file_cancelled(s))
		return TEDER_CANCELLED;
#ifdef _WIN32
	wchar_t *native = native_path;
	int count =
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)bytes, native, (int)bytes);
	if (!count)
		return TEDER_ENCODING;
	native[count] = 0;
	HANDLE file = CreateFileW(native,
	                          GENERIC_READ,
	                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                          NULL,
	                          OPEN_EXISTING,
	                          FILE_ATTRIBUTE_NORMAL,
	                          NULL);
	if (file == INVALID_HANDLE_VALUE)
		return TEDER_IO;
	s->handle = (intptr_t)file;
	s->opened = true;
	checkpoint(s, TD_FILE_OPENED);
	/* FILE_TYPE_UNKNOWN is also GetFileType's failure value; the last error decides. */
	SetLastError(NO_ERROR);
	DWORD type = INFO_FAULT() ? FILE_TYPE_UNKNOWN : GetFileType(file);
	BY_HANDLE_FILE_INFORMATION info = {0};
	teder_code code = TEDER_OK;
	if (type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR)
		code = TEDER_IO;
	else if (type != FILE_TYPE_DISK)
		code = TEDER_UNSUPPORTED;
	else if (INFO_FAULT() || !GetFileInformationByHandle(file, &info))
		code = TEDER_IO;
	else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		code = TEDER_UNSUPPORTED;
	if (code != TEDER_OK) {
		td_file_close(s);
		return code;
	}
	s->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
#else
	(void)bytes;
	(void)native_path;
	int file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (file < 0)
		return TEDER_IO;
	s->handle = file;
	s->opened = true;
	checkpoint(s, TD_FILE_OPENED);
	struct stat info;
	if (INFO_FAULT() || fstat(file, &info)) {
		td_file_close(s);
		return TEDER_IO;
	}
	if (!S_ISREG(info.st_mode) || info.st_size < 0) {
		td_file_close(s);
		return TEDER_UNSUPPORTED;
	}
	s->size = (uint64_t)info.st_size;
#endif
	return TEDER_OK;
}

teder_code td_file_read(td_file *s, void *out, size_t *bytes)
{
	checkpoint(s, TD_FILE_READ);
	if (td_file_cancelled(s)) {
		*bytes = 0;
		return TEDER_CANCELLED;
	}
	size_t count = *bytes < s->config.read_size ? *bytes : s->config.read_size;
#ifdef _WIN32
	DWORD got = 0;
	if (!ReadFile((HANDLE)s->handle, out, (DWORD)count, &got, NULL)) {
		*bytes = 0;
		return TEDER_IO;
	}
#else
	ssize_t got;
	do {
		got = read((int)s->handle, out, count);
	} while (got < 0 && errno == EINTR && !td_file_cancelled(s));
	if (got < 0) {
		*bytes = 0;
		return td_file_cancelled(s) ? TEDER_CANCELLED : TEDER_IO;
	}
#endif
	*bytes = (size_t)got;
	s->cursor += (size_t)got;
	return td_file_cancelled(s) ? TEDER_CANCELLED : TEDER_OK;
}

teder_code td_file_seek(td_file *s, uint64_t offset)
{
	if (td_file_cancelled(s))
		return TEDER_CANCELLED;
	if (offset > s->size || offset > INT64_MAX)
		return TEDER_IO;
#ifdef _WIN32
	LARGE_INTEGER where;
	where.QuadPart = (LONGLONG)offset;
	if (!SetFilePointerEx((HANDLE)s->handle, where, NULL, FILE_BEGIN))
		return TEDER_IO;
#else
	if (lseek((int)s->handle, (off_t)offset, SEEK_SET) < 0)
		return TEDER_IO;
#endif
	s->cursor = offset;
	return TEDER_OK;
}

void td_file_close(td_file *s)
{
	if (!s->opened)
		return;
#ifdef _WIN32
	if (!CloseHandle((HANDLE)s->handle))
		abort();
#else
	/* Linux/Darwin release this owned descriptor before reporting close errors.
	 * Never retry: that number can already belong to another thread's file.
	 * Owned workers are never pthread_cancel'ed. Preserve the diagnostic. */
	if (close((int)s->handle))
		s->close_failed = true;
#endif
	s->opened = false;
	checkpoint(s, TD_FILE_CLOSED);
}
