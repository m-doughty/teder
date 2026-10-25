#include "teder_notcurses.h"
#include <notcurses/notcurses.h>
#include <stddef.h>

teder_code TEDER_CALL teder_renderer_get_build_info(teder_build_info *out)
{
	int major, minor, patch, tweak;
	if (out == NULL) return TEDER_INVALID;
	if (out->struct_size != sizeof(*out) || out->abi_version != TEDER_ABI_VERSION)
		return TEDER_ABI_MISMATCH;
	notcurses_version_components(&major, &minor, &patch, &tweak);
	if (major != 3 || minor != 0 || patch != 17) return TEDER_UNSUPPORTED;
	return teder_get_build_info(out);
}
