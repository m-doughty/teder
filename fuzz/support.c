#include "support.h"
#include <stdarg.h>
#include <unistd.h>

static char trace_log[4 << 20];
static atomic_size_t trace_used;

void fz_trace(const char *format, ...)
{
	char line[256];
	va_list arguments;
	va_start(arguments, format);
	int length = vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (length <= 0)
		return;
	size_t bytes = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;
	size_t at = atomic_fetch_add(&trace_used, bytes);
	if (at <= sizeof(trace_log) - bytes)
		memcpy(trace_log + at, line, bytes);
}

/* Callers flush after joining traced threads, or while aborting. */
void fz_trace_flush(void)
{
	size_t used = atomic_exchange(&trace_used, 0);
	bool truncated = used > sizeof(trace_log);
	fwrite(trace_log, 1, truncated ? sizeof(trace_log) : used, stderr);
	if (truncated)
		fputs("[trace truncated]\n", stderr);
}

typedef union {
	max_align_t alignment;
	struct {
		fz_input *owner;
		size_t size;
	} value;
} allocation;

static void *allocate(void *context, size_t size, const char *site)
{
	(void)site;
	fz_input *f = context;
	FZ_CHECK(size && size <= 32 * 1024 * 1024 && f->bytes <= 32 * 1024 * 1024 - size);
	allocation *p = malloc(sizeof(*p) + size);
	FZ_CHECK(p);
	p->value.owner = f;
	p->value.size = size;
	f->live++;
	f->bytes += size;
	if (f->bytes > f->peak)
		f->peak = f->bytes;
	return p + 1;
}

static void release(void *context, void *pointer)
{
	if (!pointer)
		return;
	fz_input *f = context;
	allocation *p = (allocation *)pointer - 1;
	FZ_CHECK(p->value.owner == f && f->live && f->bytes >= p->value.size);
	f->bytes -= p->value.size;
	f->live--;
	free(p);
}

td_allocator fz_allocator(fz_input *f)
{
	return (td_allocator){f, allocate, release};
}

uint32_t fz_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool fz_begin(fz_input *f, const uint8_t *data, size_t size)
{
	if (size < FZ_HEADER || size > FZ_MAX_INPUT)
		return false;
	memset(f, 0, sizeof(*f));
	atomic_init(&f->cancel, 0);
	memcpy(f->control, data, FZ_HEADER);
	f->cancel_at = (data[2] & 128) ? 1 + data[3] : 4096;
	strcpy(f->path, "/tmp/teder-fuzz-XXXXXX");
	int fd = mkstemp(f->path);
	FZ_CHECK(fd >= 0);
	for (size_t offset = FZ_HEADER; offset < size;) {
		ssize_t written = write(fd, data + offset, size - offset);
		FZ_CHECK(written > 0);
		offset += (size_t)written;
	}
	FZ_CHECK(close(fd) == 0);
	return true;
}

void fz_checkpoint(void *context, unsigned point)
{
	fz_input *f = context;
	if (point == TD_FILE_OPENED)
		f->opened++;
	if (point == TD_FILE_CLOSED)
		f->closed++;
	if (point == TD_FILE_READ && ++f->reads >= f->cancel_at)
		atomic_store(&f->cancel, 1);
}

void fz_remove(fz_input *f)
{
	FZ_CHECK(f->opened == f->closed && unlink(f->path) == 0);
	f->path[0] = 0;
}

void fz_end(fz_input *f)
{
	if (f->path[0])
		fz_remove(f);
	FZ_CHECK(!f->live && !f->bytes);
}

void fz_touch(const void *data, size_t size)
{
	FZ_CHECK(!size || data);
	const volatile unsigned char *p = data;
	for (size_t i = 0; i < size; i++)
		(void)p[i];
}

uint64_t fz_hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *p = data;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ p[i]) * UINT64_C(0x100000001b3);
	return hash;
}

uint64_t fz_hash_u32(uint64_t hash, uint32_t value)
{
	const unsigned char bytes[4] = {(unsigned char)value,
	                                (unsigned char)(value >> 8),
	                                (unsigned char)(value >> 16),
	                                (unsigned char)(value >> 24)};
	return fz_hash_bytes(hash, bytes, sizeof(bytes));
}

uint64_t fz_hash_f32(uint64_t hash, float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return fz_hash_u32(hash, bits);
}

enum { KNOWN_MAX = 1 << 16 };
static char known_text[KNOWN_MAX];
static bool known_loaded;

static void known_load(const char *path)
{
	FILE *file = fopen(path, "rb");
	FZ_CHECK(file);
	size_t size = fread(known_text, 1, sizeof(known_text) - 1, file);
	FZ_CHECK(!ferror(file) && feof(file) && size);
	FZ_CHECK(fclose(file) == 0);
	known_text[size] = 0;
	known_loaded = true;
}

bool fz_expect_find(const char *target, const uint8_t *data, size_t size, fz_expect *out)
{
	const char *path = getenv("TEDER_FUZZ_KNOWN");
	if (!path || !*path)
		return false;
	if (!known_loaded)
		known_load(path);
	char want[40];
	snprintf(want,
	         sizeof(want),
	         " hash=0x%016llx",
	         (unsigned long long)fz_hash_bytes(FZ_HASH_INIT, data, size));
	size_t target_bytes = strlen(target);
	for (const char *line = known_text; *line;) {
		const char *end = strchr(line, '\n');
		size_t bytes = end ? (size_t)(end - line) : strlen(line);
		const char *next = line + bytes + (end ? 1 : 0);
		if (*line != '#' && bytes > target_bytes + 1 && !strncmp(line, target, target_bytes) &&
		    line[target_bytes] == ' ') {
			const char *name = line + target_bytes + 1;
			const char *fields = memchr(name, ' ', (size_t)(line + bytes - name));
			FZ_CHECK(fields && (size_t)(fields - name) < sizeof(out->name));
			size_t field_bytes = (size_t)(line + bytes - fields);
			FZ_CHECK(field_bytes < sizeof(out->fields));
			memcpy(out->fields, fields, field_bytes);
			out->fields[field_bytes] = 0;
			if (strstr(out->fields, want)) {
				memcpy(out->name, name, (size_t)(fields - name));
				out->name[fields - name] = 0;
				return true;
			}
		}
		line = next;
	}
	return false;
}

/* Returns the token after " key=" or NULL. Keys never contain '='. */
static const char *known_token(const fz_expect *e, const char *key)
{
	char pattern[32];
	int length = snprintf(pattern, sizeof(pattern), " %s=", key);
	FZ_CHECK(length > 0 && (size_t)length < sizeof(pattern));
	const char *at = strstr(e->fields, pattern);
	return at ? at + length : NULL;
}

bool fz_expect_has(const fz_expect *e, const char *key)
{
	return known_token(e, key) != NULL;
}

uint64_t fz_expect_value(const fz_expect *e, const char *key)
{
	const char *token = known_token(e, key);
	if (!token) {
		fprintf(stderr, "%s: manifest entry has no %s\n", e->name, key);
		FZ_CHECK(token);
	}
	char *end;
	unsigned long long value = strtoull(token, &end, 0);
	FZ_CHECK(end != token && (*end == 0 || *end == ' '));
	return value;
}

void fz_expect_passed(const fz_expect *e)
{
	fprintf(stderr, "known seed checked: %s\n", e->name);
}
