#ifndef TEDER_TEST_METADATA_H
#define TEDER_TEST_METADATA_H
#include <check.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static FILE *th_meta_open(const char *path, bool write)
{
#ifdef _WIN32
	wchar_t native[1024];
	ck_assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, native, 1024));
	return _wfopen(native, write ? L"wb" : L"rb");
#else
	return fopen(path, write ? "wb" : "rb");
#endif
}

static void th_metadata_remove(const char *path)
{
#ifdef _WIN32
	wchar_t native[1024];
	ck_assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, native, 1024));
	ck_assert(_wremove(native) == 0);
#else
	ck_assert(remove(path) == 0);
#endif
}

/* Independent wire fixtures: existing STREAMINFO plus literal comments/pictures.
 * Payload is deliberately opaque; probing must not decode images or audio. */
typedef struct {
	unsigned char data[16384];
	size_t bytes, comment, picture, description;
} th_metadata_fixture;

static void th_meta_bytes(th_metadata_fixture *f, const void *p, size_t n)
{
	ck_assert(n <= sizeof(f->data) - f->bytes);
	memcpy(f->data + f->bytes, p, n);
	f->bytes += n;
}

static void th_meta_u32(th_metadata_fixture *f, uint32_t n, bool little)
{
	unsigned char b[4];
	for (unsigned i = 0; i < 4; i++)
		b[i] = (unsigned char)(n >> (8 * (little ? i : 3 - i)));
	th_meta_bytes(f, b, 4);
}

static void th_meta_end(th_metadata_fixture *f, size_t start, bool last)
{
	size_t length = f->bytes - start - 4;
	for (unsigned i = 1; i < 4; i++)
		f->data[start + i] = (unsigned char)(length >> (8 * (3 - i)));
	if (last)
		f->data[start] |= 128;
}

static void
th_metadata_make(th_metadata_fixture *f, unsigned tags, unsigned pictures, unsigned padding)
{
	memset(f, 0, sizeof(*f));
	FILE *file = th_meta_open(FIXTURE_DIR "/track-a.flac", false);
	ck_assert(file && fread(f->data, 1, 42, file) == 42 && !fclose(file));
	f->bytes = 42;
	f->data[4] = 0;
	f->comment = f->bytes;
	th_meta_u32(f, 4u << 24, false);
	th_meta_u32(f, 4, true);
	th_meta_bytes(f, "test", 4);
	th_meta_u32(f, tags, true);
	const char *values[] = {
	    "ARTIST=one", "ARTIST=two", "EMPTY=", "NOTE=a\0b", "TITLE=\xd7\xa9", "A=x"};
	const unsigned lengths[] = {10, 10, 6, 8, 8, 3};
	for (unsigned i = 0; i < tags; i++) {
		unsigned entry = i < 5 ? i : 5;
		th_meta_u32(f, lengths[entry], true);
		th_meta_bytes(f, values[entry], lengths[entry]);
	}
	th_meta_end(f, f->comment, !pictures && !padding);
	for (unsigned i = 0; i < pictures; i++) {
		size_t start = f->bytes;
		if (!i)
			f->picture = start;
		th_meta_u32(f, 6u << 24, false);
		th_meta_u32(f, 3 + i, false);
		th_meta_u32(f, 9, false);
		th_meta_bytes(f, "image/png", 9);
		th_meta_u32(f, 2, false);
		if (!i)
			f->description = f->bytes;
		th_meta_bytes(f, "\xd7\xa9", 2);
		th_meta_u32(f, 320 + i, false);
		th_meta_u32(f, 200 + i, false);
		th_meta_u32(f, 24, false);
		th_meta_u32(f, 0, false);
		th_meta_u32(f, 5, false);
		th_meta_bytes(f, "\0\1\xff\0\3", 5);
		th_meta_end(f, start, i + 1 == pictures && !padding);
	}
	for (unsigned i = 0; i < padding; i++)
		th_meta_u32(f, (i + 1 == padding ? 129u : 1u) << 24, false);
}

static void th_metadata_write(const th_metadata_fixture *f, const char *path, size_t bytes)
{
	FILE *file = th_meta_open(path, true);
	ck_assert(file && fwrite(f->data, 1, bytes, file) == bytes && !fclose(file));
}
#endif
