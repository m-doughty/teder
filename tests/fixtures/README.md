# Reference audio

These original synthetic sample sequences and generated `.pcm`/`.flac` files
are dedicated to the public domain under
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/). No recordings are included.
The generator uses Python's standard library and no codec implementation.
It writes one FLAC frame with independent verbatim subframes, following
[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html#section-9).

| Pair of files | Frames | Channels | Signed PCM | Rate | TITLE |
|---|---:|---:|---|---:|---|
| `track-a` | 16 | 2, interleaved L/R | 16-bit little-endian | 44100 Hz | track-a |
| `track-b` | 16 | 2, interleaved L/R | 16-bit little-endian | 44100 Hz | track-b |
| `mono-24` | 16 | 1 | 24-bit little-endian | 48000 Hz | mono-24 |

`reference.h` contains separately maintained literal integer expectations.
Integer tolerance is zero; float samples divide by `2^(bits-1)` and are exactly
representable in binary32. Track B starts at concatenated frame 16. The sink
self-test crosses that boundary inside a 13-frame callback, without padding.
`mono-24` includes both signed endpoints. Each file has STREAMINFO, one TITLE
comment, header/frame CRCs and a PCM MD5. Tests check raw PCM separately from
libFLAC decoding, then verify samples, format, metadata and the decoded MD5.
This small corpus does not cover every supported FLAC encoding or corruption.

Regenerate with `python tests/fixtures/generate.py` from the package root.
Its printed hashes must match `SHA256SUMS`. Review changes against the literal
expectations; never derive expected samples by decoding the generated FLAC.
Python is needed only for regeneration, not configuration or normal testing.
