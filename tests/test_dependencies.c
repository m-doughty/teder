#include <miniaudio.h>
#include <FLAC/stream_decoder.h>
#include <kiss_fftr.h>
#include <stdlib.h>
#include <string.h>

/* Link/ownership smoke only: no devices, input files, playback or DSP claims. */
int main(void)
{
	FLAC__StreamDecoder *decoder;
	size_t bytes = 0;
	void *storage;
	kiss_fftr_cfg plan;
	if (strcmp(ma_version_string(), "0.11.25") || strcmp(FLAC__VERSION_STRING, "1.5.0")) return 1;
	decoder = FLAC__stream_decoder_new();
	if (decoder == NULL) return 2;
	FLAC__stream_decoder_delete(decoder);
	if (kiss_fftr_alloc(256, 0, NULL, &bytes) != NULL || bytes == 0) return 3;
	storage = malloc(bytes);
	if (storage == NULL) return 4;
	plan = kiss_fftr_alloc(256, 0, storage, &bytes);
	/* Caller storage owns the complete plan; no kiss_fft_free on that view. */
	if (plan != storage) { free(storage); return 5; }
	free(storage);
	return 0;
}
