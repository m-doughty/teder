#include "teder.h"
#include "teder_version.h"
#include <float.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "teder requires IEEE binary32 float");

teder_code TEDER_CALL teder_get_build_info(teder_build_info *out)
{
	teder_build_info info = {0};
	if (out == NULL)
		return TEDER_INVALID;
	if (out->struct_size != sizeof(*out) || out->abi_version != TEDER_ABI_VERSION)
		return TEDER_ABI_MISMATCH;
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	info.version_major = TEDER_VERSION_MAJOR;
	info.version_minor = TEDER_VERSION_MINOR;
	info.version_patch = TEDER_VERSION_PATCH;
	/* WHY: compiled dependencies do not establish tested playback capabilities. */
	memcpy(out, &info, sizeof(info));
	return TEDER_OK;
}
