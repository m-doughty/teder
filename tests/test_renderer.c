#include "teder_notcurses.h"
int main(void)
{
	teder_build_info info = {0};
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	return teder_renderer_get_build_info(&info) == TEDER_OK && info.capabilities == 0 ? 0 : 1;
}
