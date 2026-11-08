/* CoreAudio without an audio device. A real miniaudio CoreAudio context is created,
 * then its runtime-linked CoreAudio, AudioUnit and CoreFoundation functions are
 * replaced with fakes that model one output and one input device. Every miniaudio
 * call these paths make is modelled: an unmodelled call, a short property buffer or
 * any call after the AudioUnit was disposed fails the test where it happens. The
 * device listener functions stay real, because context setup and teardown register
 * and remove them on the system object. */
#include "device.h"
#include "support/harness.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <check.h>
#include <stdio.h>
#include <string.h>

#define OK(call) ck_assert((call) == TEDER_OK)

enum { INPUT_ID = 41, OUTPUT_ID = 42, FORMAT_COUNT = 5, RATE_COUNT = 2, KEY_LIMIT = 272 };

static const char output_uid[] = "teder-fake-output";
static const char input_uid[] = "teder-fake-input";
static const char output_name[] = "Fake output";
static const char input_name[] = "Fake input";

/* AudioUnit calls that can be made to fail; each is followed by unit disposal. */
typedef enum {
	FAULT_NONE,
	FAULT_ENABLE_OUTPUT,
	FAULT_ENABLE_INPUT,
	FAULT_CURRENT_DEVICE,
	FAULT_UNIT_FORMAT,
	FAULT_SLICE,
	FAULT_RENDER,
	FAULT_LISTENER,
	FAULT_INITIALIZE,
	FAULT_COUNT
} unit_fault;

static const char *const fault_names[FAULT_COUNT] = {"none",
                                                     "enable output",
                                                     "enable input",
                                                     "current device",
                                                     "unit format",
                                                     "maximum frames",
                                                     "render callback",
                                                     "running listener",
                                                     "initialize"};

static struct {
	ma_context *context;
	unit_fault fault;
	char unit; /* the fake AudioUnit's identity */
	bool live, ended;
	unsigned created, disposed, device_sets, rate_queries, strings, releases;
	UInt32 frames;                         /* the device's buffer frame size */
	AudioObjectPropertySelector fail_read; /* an output property that cannot be read */
	bool fail_size;                        /* setting the buffer frame size fails */
} fake;

typedef struct {
	char text[5];
} fourcc;

static fourcc code_text(UInt32 code)
{
	fourcc out = {{0}};
	for (unsigned i = 0; i < 4; i++) {
		char c = (char)(code >> (24 - 8 * i));
		out.text[i] = c >= ' ' && c <= '~' ? c : '?';
	}
	return out;
}

/* Every fake starts here: nothing may reach CoreAudio once the unit of the current
 * open is disposed. The device then no longer exists for miniaudio. */
static void touch(const char *call)
{
	ck_assert_msg(!fake.ended, "%s called after the AudioUnit was disposed", call);
}

static AudioUnit unit_handle(void)
{
	return (AudioUnit)(void *)&fake.unit;
}

static void live_unit(AudioUnit unit, const char *call)
{
	touch(call);
	ck_assert_msg(unit == unit_handle() && fake.live, "%s on a unit that is not live", call);
}

static AudioStreamRangedDescription
stream_format(Float64 rate, AudioFormatID id, AudioFormatFlags flags, UInt32 bits)
{
	UInt32 frame = 2 * bits / 8;
	return (AudioStreamRangedDescription){
	    .mFormat = {.mSampleRate = rate,
	                .mFormatID = id,
	                .mFormatFlags = flags | kAudioFormatFlagIsPacked,
	                .mBytesPerPacket = frame,
	                .mFramesPerPacket = 1,
	                .mBytesPerFrame = frame,
	                .mChannelsPerFrame = 2,
	                .mBitsPerChannel = bits},
	    .mSampleRateRange = {rate, rate}};
}

typedef union {
	AudioObjectID ids[2];
	AudioObjectID id;
	CFStringRef string;
	UInt32 frames;
	AudioBufferList buffers;
	AudioChannelLayout layout;
	AudioStreamRangedDescription formats[FORMAT_COUNT];
	AudioValueRange ranges[RATE_COUNT];
} property_value;

/* The device model. f32 is listed twice and AC-3 is not PCM, so device information
 * keeps three formats and queries the sample rates once for each. */
static void property(AudioObjectID object,
                     const AudioObjectPropertyAddress *a,
                     property_value *v,
                     UInt32 *bytes)
{
	bool output = object == OUTPUT_ID, scope_out = a->mScope == kAudioObjectPropertyScopeOutput;
	if (object == kAudioObjectSystemObject && a->mScope == kAudioObjectPropertyScopeGlobal) {
		switch (a->mSelector) {
		case kAudioHardwarePropertyDevices:
			v->ids[0] = INPUT_ID;
			v->ids[1] = OUTPUT_ID;
			*bytes = sizeof(v->ids);
			return;
		case kAudioHardwarePropertyDefaultOutputDevice:
		case kAudioHardwarePropertyDefaultInputDevice:
			v->id =
			    a->mSelector == kAudioHardwarePropertyDefaultOutputDevice ? OUTPUT_ID : INPUT_ID;
			*bytes = sizeof(v->id);
			return;
		default:
			break;
		}
	} else if (object == INPUT_ID || object == OUTPUT_ID) {
		switch (a->mSelector) {
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyDeviceNameCFString:
			v->string = (CFStringRef)(const void *)(a->mSelector == kAudioDevicePropertyDeviceUID
			                                            ? (output ? output_uid : input_uid)
			                                            : (output ? output_name : input_name));
			*bytes = sizeof(v->string);
			return;
		case kAudioDevicePropertyStreamConfiguration:
			memset(&v->buffers, 0, sizeof(v->buffers));
			v->buffers.mNumberBuffers = output == scope_out;
			v->buffers.mBuffers[0].mNumberChannels = 2;
			*bytes = sizeof(v->buffers);
			return;
		default:
			break;
		}
		if (output && scope_out)
			switch (a->mSelector) {
			case kAudioDevicePropertyPreferredChannelLayout:
				memset(&v->layout, 0, sizeof(v->layout));
				v->layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
				*bytes = sizeof(v->layout);
				return;
			case kAudioStreamPropertyAvailableVirtualFormats:
				v->formats[0] =
				    stream_format(48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat, 32);
				v->formats[1] = stream_format(
				    48000, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger, 16);
				v->formats[2] =
				    stream_format(44100, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat, 32);
				v->formats[3] = stream_format(48000, kAudioFormatAC3, 0, 0);
				v->formats[4] = stream_format(
				    48000, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger, 32);
				*bytes = sizeof(v->formats);
				return;
			case kAudioDevicePropertyAvailableNominalSampleRates:
				v->ranges[0] = (AudioValueRange){44100, 48000};
				v->ranges[1] = (AudioValueRange){96000, 96000};
				*bytes = sizeof(v->ranges);
				return;
			case kAudioDevicePropertyBufferFrameSizeRange:
				v->ranges[0] = (AudioValueRange){14, 4096};
				*bytes = sizeof(v->ranges[0]);
				return;
			case kAudioDevicePropertyBufferFrameSize:
				v->frames = fake.frames;
				*bytes = sizeof(v->frames);
				return;
			default:
				break;
			}
	}
	ck_abort_msg("unmodelled property '%s' scope '%s' on object %u",
	             code_text(a->mSelector).text,
	             code_text(a->mScope).text,
	             (unsigned)object);
}

static OSStatus get_size(AudioObjectID object,
                         const AudioObjectPropertyAddress *address,
                         UInt32 qualifier_size,
                         const void *qualifier,
                         UInt32 *size)
{
	(void)qualifier_size;
	(void)qualifier;
	touch("AudioObjectGetPropertyDataSize");
	property_value value;
	property(object, address, &value, size);
	return noErr;
}

static OSStatus get_data(AudioObjectID object,
                         const AudioObjectPropertyAddress *address,
                         UInt32 qualifier_size,
                         const void *qualifier,
                         UInt32 *size,
                         void *data)
{
	(void)qualifier_size;
	(void)qualifier;
	touch("AudioObjectGetPropertyData");
	property_value value;
	UInt32 bytes;
	property(object, address, &value, &bytes);
	if (object == OUTPUT_ID && fake.fail_read && address->mSelector == fake.fail_read)
		return kAudioHardwareBadDeviceError;
	ck_assert_msg(*size >= bytes,
	              "property '%s' read into %u bytes, needs %u",
	              code_text(address->mSelector).text,
	              (unsigned)*size,
	              (unsigned)bytes);
	memcpy(data, &value, bytes);
	*size = bytes;
	if (address->mSelector == kAudioDevicePropertyDeviceUID ||
	    address->mSelector == kAudioDevicePropertyDeviceNameCFString)
		fake.strings++;
	if (address->mSelector == kAudioDevicePropertyAvailableNominalSampleRates)
		fake.rate_queries++;
	return noErr;
}

static OSStatus set_data(AudioObjectID object,
                         const AudioObjectPropertyAddress *address,
                         UInt32 qualifier_size,
                         const void *qualifier,
                         UInt32 size,
                         const void *data)
{
	(void)qualifier_size;
	(void)qualifier;
	touch("AudioObjectSetPropertyData");
	ck_assert_msg(object == OUTPUT_ID &&
	                  address->mSelector == kAudioDevicePropertyBufferFrameSize &&
	                  address->mScope == kAudioObjectPropertyScopeOutput && size == sizeof(UInt32),
	              "unmodelled property '%s' set on object %u",
	              code_text(address->mSelector).text,
	              (unsigned)object);
	if (fake.fail_size)
		return kAudioHardwareUnspecifiedError;
	memcpy(&fake.frames, data, sizeof(fake.frames));
	return noErr;
}

/* Device properties hand out these strings; nothing else may reach these functions. */
static Boolean
fake_string(CFStringRef string, char *buffer, CFIndex size, CFStringEncoding encoding)
{
	touch("CFStringGetCString");
	const char *text = (const char *)(const void *)string;
	ck_assert(text == output_uid || text == input_uid || text == output_name || text == input_name);
	ck_assert(encoding == kCFStringEncodingUTF8);
	size_t length = strlen(text);
	if (size < 0 || length >= (size_t)size)
		return false;
	memcpy(buffer, text, length + 1);
	return true;
}

static void fake_release(CFTypeRef value)
{
	touch("CFRelease");
	ck_assert(value == output_uid || value == input_uid || value == output_name ||
	          value == input_name);
	fake.releases++;
}

static AudioComponent find_next(AudioComponent component,
                                const AudioComponentDescription *description)
{
	(void)component;
	(void)description;
	ck_abort_msg("AudioComponentFindNext after context setup");
}

static OSStatus instance_new(AudioComponent component, AudioComponentInstance *unit)
{
	touch("AudioComponentInstanceNew");
	ck_assert(component == (AudioComponent)fake.context->coreaudio.component);
	ck_assert_msg(!fake.live, "a second AudioUnit while one is live");
	fake.live = true;
	fake.created++;
	*unit = unit_handle();
	return noErr;
}

static OSStatus dispose(AudioComponentInstance unit)
{
	live_unit(unit, "AudioComponentInstanceDispose");
	fake.live = false;
	fake.ended = true;
	fake.disposed++;
	return noErr;
}

static OSStatus unit_start(AudioUnit unit)
{
	live_unit(unit, "AudioOutputUnitStart");
	ck_abort_msg("AudioOutputUnitStart is not modelled");
}

static OSStatus unit_stop(AudioUnit unit)
{
	live_unit(unit, "AudioOutputUnitStop");
	ck_abort_msg("AudioOutputUnitStop is not modelled");
}

static OSStatus render(AudioUnit unit,
                       AudioUnitRenderActionFlags *flags,
                       const AudioTimeStamp *time,
                       UInt32 bus,
                       UInt32 frames,
                       AudioBufferList *data)
{
	(void)flags;
	(void)time;
	(void)bus;
	(void)frames;
	(void)data;
	live_unit(unit, "AudioUnitRender");
	ck_abort_msg("AudioUnitRender is not modelled");
}

static OSStatus fail(unit_fault fault)
{
	return fake.fault == fault
	           ? (fault == FAULT_CURRENT_DEVICE ? kAudioHardwareBadDeviceError
	                                            : kAudioUnitErr_FailedInitialization)
	           : noErr;
}

static OSStatus
add_listener(AudioUnit unit, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void *user)
{
	(void)user;
	live_unit(unit, "AudioUnitAddPropertyListener");
	ck_assert(id == kAudioOutputUnitProperty_IsRunning && proc);
	return fail(FAULT_LISTENER);
}

static OSStatus initialize(AudioUnit unit)
{
	live_unit(unit, "AudioUnitInitialize");
	return fail(FAULT_INITIALIZE);
}

static OSStatus get_info(AudioUnit unit,
                         AudioUnitPropertyID id,
                         AudioUnitScope scope,
                         AudioUnitElement element,
                         UInt32 *size,
                         Boolean *writable)
{
	live_unit(unit, "AudioUnitGetPropertyInfo");
	ck_assert_msg(id == kAudioUnitProperty_AudioChannelLayout && scope == kAudioUnitScope_Input &&
	                  element == 0,
	              "unmodelled unit property %u",
	              (unsigned)id);
	*size = sizeof(AudioChannelLayout);
	if (writable)
		*writable = false;
	return noErr;
}

static OSStatus get_property(AudioUnit unit,
                             AudioUnitPropertyID id,
                             AudioUnitScope scope,
                             AudioUnitElement element,
                             void *data,
                             UInt32 *size)
{
	live_unit(unit, "AudioUnitGetProperty");
	ck_assert(element == 0);
	if (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output) {
		ck_assert(*size >= sizeof(AudioStreamBasicDescription));
		AudioStreamRangedDescription current =
		    stream_format(48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat, 32);
		memcpy(data, &current.mFormat, sizeof(current.mFormat));
		*size = sizeof(current.mFormat);
		return fail(FAULT_UNIT_FORMAT);
	}
	ck_assert_msg(id == kAudioUnitProperty_AudioChannelLayout && scope == kAudioUnitScope_Input &&
	                  *size >= sizeof(AudioChannelLayout),
	              "unmodelled unit property %u",
	              (unsigned)id);
	AudioChannelLayout layout = {.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo};
	memcpy(data, &layout, sizeof(layout));
	*size = sizeof(layout);
	return noErr;
}

static OSStatus set_property(AudioUnit unit,
                             AudioUnitPropertyID id,
                             AudioUnitScope scope,
                             AudioUnitElement element,
                             const void *data,
                             UInt32 size)
{
	live_unit(unit, "AudioUnitSetProperty");
	switch (id) {
	case kAudioOutputUnitProperty_EnableIO: {
		ck_assert(size == sizeof(UInt32));
		UInt32 enabled;
		memcpy(&enabled, data, sizeof(enabled));
		bool output = scope == kAudioUnitScope_Output;
		/* Playback enables the output bus (0) and disables the input bus (1). */
		ck_assert(enabled == (output ? 1u : 0u) && element == (output ? 0u : 1u));
		return fail(output ? FAULT_ENABLE_OUTPUT : FAULT_ENABLE_INPUT);
	}
	case kAudioOutputUnitProperty_CurrentDevice: {
		ck_assert(size == sizeof(AudioObjectID) && scope == kAudioUnitScope_Global);
		AudioObjectID device;
		memcpy(&device, data, sizeof(device));
		ck_assert(device == OUTPUT_ID);
		fake.device_sets++;
		return fail(FAULT_CURRENT_DEVICE);
	}
	case kAudioUnitProperty_StreamFormat:
		ck_assert(size == sizeof(AudioStreamBasicDescription) && scope == kAudioUnitScope_Input &&
		          element == 0);
		return noErr;
	case kAudioUnitProperty_MaximumFramesPerSlice:
		ck_assert(size == sizeof(UInt32) && scope == kAudioUnitScope_Global);
		return fail(FAULT_SLICE);
	case kAudioUnitProperty_SetRenderCallback:
		ck_assert(size == sizeof(AURenderCallbackStruct) && scope == kAudioUnitScope_Global);
		return fail(FAULT_RENDER);
	default:
		ck_abort_msg("unmodelled unit property %u set", (unsigned)id);
	}
}

/* Replaces every runtime-linked function the device paths use. Uninit still uses
 * the real listener removal and library handles. */
static void install(ma_context *c)
{
	memset(&fake, 0, sizeof(fake));
	fake.context = c;
	fake.frames = 512;
	ck_assert(c->coreaudio.AudioObjectGetPropertyData && c->coreaudio.AudioUnitSetProperty &&
	          c->coreaudio.AudioComponentInstanceDispose && c->coreaudio.component);
	c->coreaudio.CFStringGetCString = (ma_proc)fake_string;
	c->coreaudio.CFRelease = (ma_proc)fake_release;
	c->coreaudio.AudioObjectGetPropertyData = (ma_proc)get_data;
	c->coreaudio.AudioObjectGetPropertyDataSize = (ma_proc)get_size;
	c->coreaudio.AudioObjectSetPropertyData = (ma_proc)set_data;
	c->coreaudio.AudioComponentFindNext = (ma_proc)find_next;
	c->coreaudio.AudioComponentInstanceDispose = (ma_proc)dispose;
	c->coreaudio.AudioComponentInstanceNew = (ma_proc)instance_new;
	c->coreaudio.AudioOutputUnitStart = (ma_proc)unit_start;
	c->coreaudio.AudioOutputUnitStop = (ma_proc)unit_stop;
	c->coreaudio.AudioUnitAddPropertyListener = (ma_proc)add_listener;
	c->coreaudio.AudioUnitGetPropertyInfo = (ma_proc)get_info;
	c->coreaudio.AudioUnitGetProperty = (ma_proc)get_property;
	c->coreaudio.AudioUnitSetProperty = (ma_proc)set_property;
	c->coreaudio.AudioUnitInitialize = (ma_proc)initialize;
	c->coreaudio.AudioUnitRender = (ma_proc)render;
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	ck_assert(count == 1 && backends[0] == ma_backend_coreaudio);
	ma_result result = ma_context_init(backends, count, config, c);
	if (result == MA_SUCCESS) {
		ck_assert(c->backend == ma_backend_coreaudio);
		install(c);
	}
	return result;
}

typedef struct {
	th_tracker tracker;
	td_backend *backend;
	td_arena *arena;
	td_pcm_pool pool;
	td_device *device;
	unsigned char key[KEY_LIMIT];
	uint32_t key_bytes;
} fixture;

/* A CoreAudio context with fakes, the first enumerated output's key and a PCM pool.
 * Nothing allocates through the tracker after this until the caller acts. */
static void init(fixture *f)
{
	ck_assert(th_tracker_init(&f->tracker));
	td_allocator a = th_allocator(&f->tracker);
	td_backend_api api = {context_init, ma_context_uninit, ma_context_enumerate_devices, f};
	teder_code code =
	    td_backend_open(1, TEDER_BACKEND_COREAUDIO, 8 * 1024 * 1024, a, &api, &f->backend);
	ck_assert_msg(code == TEDER_OK, "CoreAudio context returned %d", code);
	teder_result *r;
	OK(td_backend_enumerate(f->backend, 65536, KEY_LIMIT, &r));
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	teder_device_info entry = {.struct_size = sizeof(entry), .abi_version = TEDER_ABI_VERSION};
	OK(teder_result_device(r, 0, &entry));
	ck_assert(count == 1 && entry.is_default && entry.backend == TEDER_BACKEND_COREAUDIO);
	ck_assert(entry.name.bytes == strlen(output_name) &&
	          !memcmp(entry.name.data, output_name, entry.name.bytes));
	ck_assert(entry.key.bytes <= sizeof(f->key));
	memcpy(f->key, entry.key.data, entry.key.bytes);
	f->key_bytes = (uint32_t)entry.key.bytes;
	teder_result_release(r);
	size_t bytes = 0;
	td_pcm_layout layout;
	ck_assert(td_pcm_plan(&bytes, 128, &layout));
	f->arena = td_arena_create(a, bytes);
	ck_assert_ptr_nonnull(f->arena);
	td_pcm_init(&f->pool, td_arena_alloc(f->arena, bytes, 1, _Alignof(max_align_t)), &layout, 128);
	ck_assert(fake.strings == fake.releases && !fake.created);
}

/* The backend is closed: every allocation it made must have been released. */
static void release_fixture(fixture *f)
{
	td_pcm_clear(&f->pool);
	td_arena_destroy(f->arena);
	ck_assert(th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
}

static void close_backend(fixture *f, const char *what, size_t fail, size_t sites)
{
	teder_code code = td_backend_close(&f->backend);
	ck_assert_msg(code == TEDER_OK,
	              "%s: backend close returned %d after failing allocation %zu of %zu (%s)",
	              what,
	              code,
	              fail,
	              sites,
	              f->tracker.failed_site ? f->tracker.failed_site : "none failed");
	release_fixture(f);
}

static teder_blob key_of(fixture *f, bool keyed)
{
	return keyed ? (teder_blob){f->key, f->key_bytes} : (teder_blob){0};
}

static teder_code open_output(fixture *f, bool keyed)
{
	fake.ended = false; /* a new open may use CoreAudio again */
	return td_device_open(f->backend, &f->pool, key_of(f, keyed), 0, &f->device);
}

/* teder's default-device lookup, or miniaudio's information for the output's UID. */
static teder_code lookup(fixture *f, bool keyed, ma_device_info *info)
{
	if (!keyed) {
		td_backend_loan loan;
		teder_code code = td_backend_borrow(f->backend, (teder_blob){0}, &loan);
		if (code == TEDER_OK) {
			ck_assert(!strcmp(loan.id.coreaudio, output_uid));
			td_backend_return(f->backend);
		}
		return code;
	}
	ma_device_id id = {0};
	memcpy(id.coreaudio, output_uid, sizeof(output_uid));
	ma_result result = ma_context_get_device_info(fake.context, ma_device_type_playback, &id, info);
	teder_code code = td_backend_error(f->backend);
	return code != TEDER_OK ? code : result == MA_SUCCESS ? TEDER_OK : TEDER_DEVICE_LOST;
}

static void check_info(const ma_device_info *info)
{
	static const ma_format formats[] = {ma_format_f32, ma_format_s16, ma_format_s32};
	static const ma_uint32 rates[] = {48000, 44100, 96000};
	ck_assert(!strcmp(info->name, output_name) && !strcmp(info->id.coreaudio, output_uid) &&
	          info->isDefault);
	ck_assert_msg(info->nativeDataFormatCount == 9, "%u formats", info->nativeDataFormatCount);
	for (unsigned i = 0; i < 9; i++)
		ck_assert(info->nativeDataFormats[i].format == formats[i / 3] &&
		          info->nativeDataFormats[i].sampleRate == rates[i % 3] &&
		          info->nativeDataFormats[i].channels == 2 && !info->nativeDataFormats[i].flags);
}

/* Device information keeps the stream descriptions while it queries each format's
 * sample rates. Fail every allocation of a lookup in turn, including each of those
 * queries: the lookup reports NO_MEMORY and the backend still closes cleanly. */
static void info_failures(bool keyed)
{
	const char *what = keyed ? "output information" : "default output lookup";
	fixture f = {0};
	init(&f);
	ma_device_info info;
	size_t before = f.tracker.attempts;
	OK(lookup(&f, keyed, &info));
	size_t sites = f.tracker.attempts - before;
	if (keyed)
		check_info(&info);
	ck_assert_msg(fake.rate_queries == 3, "%u sample-rate queries", fake.rate_queries);
	ck_assert(fake.strings == fake.releases && !fake.created);
	OK(td_backend_close(&f.backend));
	release_fixture(&f);
	ck_assert(sites >= 5); /* at least the layout, the formats and three rate lists */
	for (size_t fail = 1; fail <= sites; fail++) {
		fixture fault = {0};
		init(&fault);
		fault.tracker.fail_at = fault.tracker.attempts + fail;
		teder_code code = lookup(&fault, keyed, &info);
		ck_assert_msg(code == TEDER_NO_MEMORY,
		              "%s: failing allocation %zu of %zu returned %d",
		              what,
		              fail,
		              sites,
		              code);
		ck_assert(fake.strings == fake.releases);
		close_backend(&fault, what, fail, sites);
	}
	printf("%s: allocation failures checked=%zu\n", what, sites);
}

START_TEST(info_default)
{
	info_failures(false);
}
END_TEST

START_TEST(info_keyed)
{
	info_failures(true);
}
END_TEST

static void check_closed_unit(const char *what, unsigned created)
{
	ck_assert_msg(fake.created == created && fake.disposed == created && !fake.live,
	              "%s: %u units created, %u disposed",
	              what,
	              fake.created,
	              fake.disposed);
	ck_assert_msg(fake.strings == fake.releases,
	              "%s: %u strings, %u released",
	              what,
	              fake.strings,
	              fake.releases);
}

/* Fail every allocation of a device open in turn, by key or through the default
 * lookup. Each open reports NO_MEMORY, disposes any unit it created exactly once
 * and leaves the backend closable. */
static void init_failures(bool keyed)
{
	const char *what = keyed ? "keyed open" : "default open";
	fixture f = {0};
	init(&f);
	size_t before = f.tracker.attempts;
	OK(open_output(&f, keyed));
	size_t sites = f.tracker.attempts - before;
	td_device_info info;
	OK(td_device_get_info(f.device, &info));
	ck_assert(info.backend == TEDER_BACKEND_COREAUDIO && info.rate == 48000 && info.channels == 2 &&
	          info.period_frames == 480 && info.native_rate == 48000 && info.native_channels == 2 &&
	          info.native_format == ma_format_f32);
	ck_assert(fake.device_sets == 1 && fake.frames == 480 && fake.live);
	OK(td_device_close(&f.device));
	check_closed_unit(what, 1);
	OK(td_backend_close(&f.backend));
	release_fixture(&f);
	for (size_t fail = 1; fail <= sites; fail++) {
		fixture fault = {0};
		init(&fault);
		fault.tracker.fail_at = fault.tracker.attempts + fail;
		teder_code code = open_output(&fault, keyed);
		ck_assert_msg(code == TEDER_NO_MEMORY && !fault.device,
		              "%s: failing allocation %zu of %zu returned %d",
		              what,
		              fail,
		              sites,
		              code);
		check_closed_unit(what, fake.created);
		ck_assert(fake.created <= 1);
		close_backend(&fault, what, fail, sites);
	}
	printf("%s: allocation failures checked=%zu\n", what, sites);
}

START_TEST(init_default)
{
	init_failures(false);
}
END_TEST

START_TEST(init_keyed)
{
	init_failures(true);
}
END_TEST

/* One AudioUnit call fails during device initialization. The open reports the
 * error, the unit is disposed once, nothing reaches CoreAudio after that (touch),
 * and the same backend then opens and closes the device normally. */
static void unit_failure(unit_fault fault, bool keyed)
{
	fixture f = {0};
	init(&f);
	fake.fault = fault;
	teder_code code = open_output(&f, keyed);
	ck_assert_msg(code == TEDER_DEVICE_LOST && !f.device,
	              "%s failure (%s open) returned %d",
	              fault_names[fault],
	              keyed ? "keyed" : "default",
	              code);
	check_closed_unit(fault_names[fault], 1);
	ck_assert(fake.device_sets == (fault >= FAULT_CURRENT_DEVICE ? 1u : 0u));
	fake.fault = FAULT_NONE;
	OK(open_output(&f, keyed));
	OK(td_device_close(&f.device));
	check_closed_unit(fault_names[fault], 2);
	OK(td_backend_close(&f.backend));
	release_fixture(&f);
}

/* The output vanished between lookup and initialization: selecting it fails. */
START_TEST(current_device)
{
	unit_failure(FAULT_CURRENT_DEVICE, true);
	unit_failure(FAULT_CURRENT_DEVICE, false);
}
END_TEST

START_TEST(unit_failures)
{
	for (unit_fault fault = FAULT_ENABLE_OUTPUT; fault < FAULT_COUNT; fault++)
		if (fault != FAULT_CURRENT_DEVICE)
			for (unsigned keyed = 0; keyed < 2; keyed++)
				unit_failure(fault, keyed);
}
END_TEST

/* The output vanishes while initialization sizes its buffer: reading the allowed
 * range, or the size in use after setting it, fails. The open reports the error,
 * disposes its unit once and makes no CoreAudio call after that (touch). The same
 * backend then opens the output normally. */
static void buffer_failure(AudioObjectPropertySelector selector, bool keyed)
{
	const char *what =
	    selector == kAudioDevicePropertyBufferFrameSizeRange ? "frame size range" : "frame size";
	fixture f = {0};
	init(&f);
	fake.fail_read = selector;
	teder_code code = open_output(&f, keyed);
	ck_assert_msg(code == TEDER_DEVICE_LOST && !f.device,
	              "%s read failure (%s open) returned %d",
	              what,
	              keyed ? "keyed" : "default",
	              code);
	check_closed_unit(what, 1);
	ck_assert(fake.device_sets == 1);
	fake.fail_read = 0;
	OK(open_output(&f, keyed));
	OK(td_device_close(&f.device));
	check_closed_unit(what, 2);
	OK(td_backend_close(&f.backend));
	release_fixture(&f);
}

START_TEST(buffer_size)
{
	for (unsigned keyed = 0; keyed < 2; keyed++) {
		buffer_failure(kAudioDevicePropertyBufferFrameSizeRange, keyed);
		buffer_failure(kAudioDevicePropertyBufferFrameSize, keyed);
	}
	/* A rejected size alone is not a failure: the device keeps the size it has. */
	fixture f = {0};
	init(&f);
	fake.fail_size = true;
	OK(open_output(&f, true));
	td_device_info info;
	OK(td_device_get_info(f.device, &info));
	ck_assert(info.period_frames == 512 && fake.frames == 512);
	OK(td_device_close(&f.device));
	check_closed_unit("rejected size", 1);
	OK(td_backend_close(&f.backend));
	release_fixture(&f);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "info-default"))
		test = info_default;
	else if (!strcmp(argv[1], "info-keyed"))
		test = info_keyed;
	else if (!strcmp(argv[1], "init-default"))
		test = init_default;
	else if (!strcmp(argv[1], "init-keyed"))
		test = init_keyed;
	else if (!strcmp(argv[1], "current-device"))
		test = current_device;
	else if (!strcmp(argv[1], "unit-failures"))
		test = unit_failures;
	else if (!strcmp(argv[1], "buffer-size"))
		test = buffer_size;
	else
		return 2;
	Suite *suite = suite_create("coreaudio");
	TCase *tc = tcase_create(argv[1]);
	tcase_add_test(tc, test);
	suite_add_tcase(suite, tc);
	SRunner *runner = srunner_create(suite);
	srunner_set_fork_status(runner, CK_NOFORK);
	srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
