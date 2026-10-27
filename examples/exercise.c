#include "demo.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #condition); \
			fprintf(stderr, \
			        "stage=%u outcome=%d transport=%u output=%u last_error=%d\n", \
			        d.stage, \
			        (int)d.outcome, \
			        d.status.transport, \
			        d.status.output_state, \
			        (int)d.status.last_error); \
			failed = 1; \
			goto done; \
		} \
	} while (0)

static bool settled(demo *d)
{
	uint64_t deadline = demo_millis() + 20000;
	do {
		teder_code code = demo_tick(d);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			return false;
		if (code == TEDER_OK && !d->pending && d->stage == 6)
			return true;
		demo_wait(1);
	} while (demo_millis() < deadline);
	return false;
}

static bool command(demo *d, char key, uint64_t frame, teder_code expected)
{
	uint64_t deadline = demo_millis() + 20000;
	teder_code code;
	do {
		code = demo_issue(d, key, frame);
		if (code != TEDER_BUSY)
			break;
		demo_wait(1);
	} while (demo_millis() < deadline);
	return code == TEDER_OK ? settled(d) && d->outcome == expected
	                        : code == expected && d->pending == 0;
}

int main(int argc, char **argv)
{
	bool native = argc == 4 && strcmp(argv[3], "--native") == 0;
	if (argc != 3 && !native) {
		fprintf(stderr, "Usage: teder_exercise ABSOLUTE_FLAC ABSOLUTE_NEXT [--native]\n");
		return 2;
	}
	demo d;
	int failed = 0;
	REQUIRE(demo_start(&d, argv[1], argv[2], native) == TEDER_OK);
	REQUIRE(settled(&d) && d.outcome == TEDER_OK);
	REQUIRE(d.metadata && d.probe.codec == TEDER_CODEC_FLAC);
	REQUIRE(d.probe.artwork_count == 0 && d.prepared_source != 0);
	if (native) {
		uint64_t deadline = demo_millis() + 20000;
		while (d.status.transport != TEDER_ENDED || !d.snapshot) {
			REQUIRE(demo_millis() < deadline);
			REQUIRE(settled(&d));
			demo_wait(1);
		}
		REQUIRE(d.status.output_state == TEDER_OUTPUT_READY);
		REQUIRE(d.status.track == 2 && d.status.current.source == d.prepared_source);
		REQUIRE(d.status.clock_flags & TEDER_SUBMIT_EOF);
		REQUIRE(command(&d, 'n', 0, TEDER_BAD_STATE));
		REQUIRE(d.status.track == 2 && d.status.transport == TEDER_ENDED);
	} else {
		/* Committed track-a/track-b fixtures: 16 stereo frames, 44100 Hz, 16 bit. */
		REQUIRE(d.probe.duration_frames == 16 && d.probe.sample_rate == 44100);
		REQUIRE(d.probe.channels == 2 && d.probe.bits_per_sample == 16);
		REQUIRE(strcmp(demo_note(&d), "Initial file has no artwork") == 0);
		REQUIRE(d.status.transport == TEDER_READY && d.status.next_track == 2);
		REQUIRE(d.status.next_source == d.prepared_source);
		REQUIRE(command(&d, 'c', 0, TEDER_OK) && d.status.next_state == TEDER_NEXT_NONE);
		REQUIRE(command(&d, 'j', 7, TEDER_OK) && d.status.source_frame == 7);
		REQUIRE(command(&d, 's', 0, TEDER_OK) && d.status.source_frame == 0);
		REQUIRE(command(&d, 'm', 0, TEDER_OK) && d.status.mute == 1);
		REQUIRE(command(&d, 'm', 0, TEDER_OK) && d.status.mute == 0);
		for (unsigned i = 0; i < 40; ++i)
			REQUIRE(command(&d, 'm', 0, TEDER_OK) && d.status.mute == (i + 1) % 2);
		teder_target current = d.status.current;
		d.path = "relative-path-is-invalid.flac";
		REQUIRE(command(&d, 'o', 0, TEDER_INVALID));
		REQUIRE(d.status.current.source == current.source &&
		        d.status.current.epoch == current.epoch);
		char missing[32768];
		int bytes = snprintf(missing, sizeof(missing), "%s.missing", argv[1]);
		REQUIRE(bytes > 0 && (size_t)bytes < sizeof(missing));
		d.path = missing;
		REQUIRE(command(&d, 'o', 0, TEDER_IO));
		REQUIRE(d.status.current.source == current.source &&
		        d.status.current.epoch == current.epoch);
		/* Client policy only; physical device loss remains a separate live check. */
		d.status.output_state = TEDER_OUTPUT_LOST;
		REQUIRE(strcmp(demo_note(&d), "Output lost: D selects default; Space resumes") == 0);
		d.status.output_state = TEDER_OUTPUT_NONE;
		d.outcome = TEDER_OK;
		d.status.last_error = TEDER_TOO_LATE;
		REQUIRE(strcmp(demo_note(&d),
		               "Successor missed cutoff; choose a new playback explicitly") == 0);
	}
done:
	failed |= demo_finish(&d);
	if (!failed) {
		REQUIRE(d.engine == NULL && d.metadata == NULL && d.snapshot == NULL);
		/* Close while startup is admitted; finish must join and discard its result. */
		REQUIRE(demo_start(&d, argv[1], NULL, false) == TEDER_OK);
		REQUIRE(demo_issue(&d, 'i', 0) == TEDER_OK);
		failed |= demo_finish(&d);
	}
	if (!failed)
		puts(native ? "native transition, late request and held-loan close passed"
		            : "public lifecycle and client policies passed");
	return failed;
}
