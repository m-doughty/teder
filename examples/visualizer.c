#define _GNU_SOURCE
#include "demo.h"
#include <teder_notcurses.h>
#include <notcurses/notcurses.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t interrupted;
static void interrupt(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

static void filename(char out[256], const char *path)
{
	const char *name = path;
	for (const char *p = path; *p; ++p)
		if (*p == '/' || *p == '\\')
			name = p + 1;
	size_t n = strlen(name);
	if (n > 255) {
		n = 255;
		while (((unsigned char)name[n] & 0xc0) == 0x80)
			--n;
	}
	for (size_t i = 0; i < n; ++i)
		out[i] = (unsigned char)name[i] < 32 || name[i] == 127 ? '?' : name[i];
	out[n] = 0;
}

static int draw(struct notcurses *nc, teder_renderer *renderer, const demo *d)
{
	struct ncplane *plane = notcurses_stdplane(nc);
	unsigned rows, cols;
	ncplane_dim_yx(plane, &rows, &cols);
	ncplane_erase(plane);
	ncplane_set_fg_rgb(plane, 0xd9e5ee);
	ncplane_set_bg_rgb(plane, 0x101820);
	if (rows < 14 || cols < 76) {
		ncplane_putstr_yx(plane, 0, 0, "Resize to at least 76 x 14; Q quits");
		return notcurses_render(nc);
	}
	char name[256];
	filename(name, d->status.track == 2 && d->next ? d->next : d->path);
	ncplane_putstr_yx(plane, 1, 2, "teder / live spectrum");
	ncplane_putstr_yx(plane, 2, 2, name);
	const char *states[] = {
	    "starting", "empty", "ready", "playing", "paused", "draining", "ended", "failed"};
	double seconds =
	    d->status.source_rate ? (double)d->status.source_frame / d->status.source_rate : 0;
	ncplane_printf_yx(plane,
	                  3,
	                  2,
	                  "%s  |  submitted %.1fs  |  %u Hz / %u ch  |  next %s",
	                  states[d->status.transport <= TEDER_FAILED ? d->status.transport : 0],
	                  seconds,
	                  d->status.output_rate,
	                  d->status.output_channels,
	                  d->status.next_source ? "ready" : "none");
	teder_renderer_config config = DEMO_INIT(teder_renderer_config);
	config.mode = TEDER_RENDER_SPECTRUM;
	config.foreground_rgb = 0x40d6b0;
	config.background_rgb = 0x101820;
	config.peak_rgb = 0xffb454;
	config.floor_db = -75;
	teder_rectangle rect = {5, 2, rows - 10, cols - 14};
	if (teder_renderer_configure(renderer, &config) != TEDER_OK ||
	    teder_renderer_draw(renderer, d->snapshot, plane, &rect) != TEDER_OK)
		return -1;
	config.mode = TEDER_RENDER_LEVELS;
	rect.x = (int32_t)cols - 10;
	rect.columns = 8;
	if (teder_renderer_configure(renderer, &config) != TEDER_OK ||
	    teder_renderer_draw(renderer, d->snapshot, plane, &rect) != TEDER_OK)
		return -1;
	ncplane_putstr_yx(
	    plane, (int)rows - 5, 2, "low                       spectrum                       high");
	ncplane_putstr_yx(plane, (int)rows - 5, (int)cols - 9, "L   R");
	ncplane_putstr_yx(plane, (int)rows - 4, 2, demo_note(d));
	ncplane_printf_yx(plane,
	                  (int)rows - 3,
	                  2,
	                  "code %d / engine %d | mute %u | underrun %llu | analysis drops %llu",
	                  (int)d->outcome,
	                  (int)d->status.last_error,
	                  d->status.mute,
	                  (unsigned long long)d->status.underrun_frames,
	                  (unsigned long long)d->status.analysis_dropped_frames);
	ncplane_putstr_yx(
	    plane,
	    (int)rows - 2,
	    2,
	    "Space play/pause | arrows seek | M mute | D device | N next | S stop | Q quit");
	return notcurses_render(nc);
}

static int run(int argc, char **argv)
{
	bool check = argc >= 2 && strcmp(argv[1], "--check") == 0;
	int offset = check ? 2 : 1;
	if (argc < offset + 1 || argc > offset + 2) {
		fprintf(stderr, "Usage: teder_visualizer [--check] ABSOLUTE_FLAC [ABSOLUTE_NEXT]\n");
		return 2;
	}
	FILE *output = stdout;
	if (check) {
#ifdef _WIN32
		FreeConsole();
		output = fopen("NUL", "w");
#else
		if (setsid() < 0)
			return 1;
		output = fopen("/dev/null", "w");
#endif
		if (!output)
			return 1;
	}
	notcurses_options opts = {.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_QUIT_SIGHANDLERS};
	if (check) {
		opts.termtype = "xterm-256color";
		opts.flags |=
		    NCOPTION_NO_ALTERNATE_SCREEN | NCOPTION_NO_WINCH_SIGHANDLER | NCOPTION_DRAIN_INPUT;
	}
	signal(SIGINT, interrupt);
	signal(SIGTERM, interrupt);
	struct notcurses *nc = notcurses_core_init(&opts, output);
	teder_renderer *renderer = NULL;
	demo d = {0};
	int failed = 1;
	if (!nc)
		goto done;
	teder_renderer_config config = DEMO_INIT(teder_renderer_config);
	if (teder_renderer_create(&config, &renderer) != TEDER_OK ||
	    demo_start(&d, argv[offset], argc > offset + 1 ? argv[offset + 1] : NULL, !check) !=
	        TEDER_OK)
		goto done;
	uint64_t deadline = demo_millis() + 20000;
	failed = 0;
	while (d.status.lifecycle != TEDER_CLOSED_STATE) {
		teder_code code = demo_tick(&d);
		if (code != TEDER_OK && code != TEDER_AGAIN) {
			failed = 1;
			break;
		}
		if (draw(nc, renderer, &d) < 0) {
			failed = 1;
			break;
		}
		ncinput input = {0};
		uint32_t key = check ? 0 : notcurses_get_nblock(nc, &input);
		bool quit = interrupted || key == 'q' || key == NCKEY_ESC || key == (uint32_t)-1 ||
		            key == 3 || ((key == 'c' || key == 'C') && (input.modifiers & NCKEY_MOD_CTRL));
		if (check && (demo_millis() >= deadline || (d.stage == 6 && !d.pending))) {
			failed |= d.stage != 6 || d.pending != 0 || d.outcome != TEDER_OK ||
			          d.status.transport != TEDER_READY;
			quit = true;
		}
		if (quit && !d.closing) {
			failed |= teder_request_close(d.engine) != TEDER_OK;
			d.closing = true;
		}
		if (!d.closing && d.stage == 6 && !d.pending && input.evtype != NCTYPE_RELEASE) {
			if (key == NCKEY_LEFT || key == NCKEY_RIGHT) {
				uint64_t step = (uint64_t)d.status.source_rate * 5;
				uint64_t frame = d.status.source_frame;
				frame = key == NCKEY_LEFT ? (frame > step ? frame - step : 0) : frame + step;
				if ((d.status.clock_flags & TEDER_DURATION_KNOWN) &&
				    frame > d.status.duration_frames)
					frame = d.status.duration_frames;
				demo_issue(&d, 'j', frame);
			} else if (key && key < 128 && strchr(" dmnsc", (int)key))
				demo_issue(&d, (char)key, 0);
		}
		demo_wait(30);
	}
done:
	teder_renderer_destroy(renderer);
	if (nc)
		failed |= notcurses_stop(nc) != 0;
	/* Terminal ownership ends before any blocking final join. */
	failed |= demo_finish(&d);
	if (check)
		failed |= fclose(output) != 0;
	else
		printf("teder demo closed: %s\n", failed ? "error" : "clean");
	return failed;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
	char storage[4][32768];
	char *utf8[5] = {"teder_visualizer"};
	if (argc > 5)
		return 2;
	for (int i = 1; i < argc; ++i) {
		if (!WideCharToMultiByte(CP_UTF8,
		                         WC_ERR_INVALID_CHARS,
		                         argv[i],
		                         -1,
		                         storage[i - 1],
		                         sizeof(storage[0]),
		                         NULL,
		                         NULL))
			return 2;
		utf8[i] = storage[i - 1];
	}
	return run(argc, utf8);
}
#else
int main(int argc, char **argv)
{
	return run(argc, argv);
}
#endif
