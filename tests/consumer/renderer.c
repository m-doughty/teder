#define _GNU_SOURCE
#include <teder_notcurses.h>
#include <notcurses/notcurses.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(void)
{
#ifdef _WIN32
	FreeConsole();
	FILE *output = fopen("NUL", "w");
#else
	if (setsid() < 0)
		return 1;
	FILE *output = fopen("/dev/null", "w");
#endif
	if (!output)
		return 2;
	notcurses_options opts = {.termtype = "xterm-256color",
	                          .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_ALTERNATE_SCREEN |
	                                   NCOPTION_NO_QUIT_SIGHANDLERS | NCOPTION_NO_WINCH_SIGHANDLER |
	                                   NCOPTION_DRAIN_INPUT};
	struct notcurses *nc = notcurses_core_init(&opts, output);
	if (!nc) {
		fclose(output);
		return 3;
	}
	teder_renderer_config config = {.struct_size = sizeof(config),
	                                .abi_version = TEDER_ABI_VERSION,
	                                .foreground_rgb = 0x123456,
	                                .background_rgb = 0x654321};
	teder_renderer *renderer = NULL;
	int failed = teder_renderer_create(&config, &renderer) != TEDER_OK;
	struct ncplane *plane = notcurses_stdplane(nc);
	teder_rectangle rect = {0, 0, 1, 1};
	if (!failed)
		failed = teder_renderer_configure(renderer, &config) != TEDER_OK ||
		         teder_renderer_draw(renderer, NULL, plane, &rect) != TEDER_OK;
	nccell cell = {0};
	if (!failed)
		failed = ncplane_at_yx_cell(plane, 0, 0, &cell) < 0 ||
		         ncchannels_bg_rgb(cell.channels) != config.background_rgb ||
		         ncchannels_fg_rgb(cell.channels) != config.foreground_rgb;
	nccell_release(plane, &cell);
	teder_renderer_destroy(renderer);
	failed |= notcurses_stop(nc) != 0;
	failed |= fclose(output) != 0;
	return failed;
}
