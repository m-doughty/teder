#include <teder.h>
#include <teder_notcurses.h>
int main(void)
{
	teder_build_info info = {0};
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	if (teder_get_build_info(&info) != TEDER_OK) return 1;
	return info.abi_version == 1 && info.capabilities == 0 && info.backend_mask == 0 ? 0 : 2;
}
