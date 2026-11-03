"""Generate the small CC0 regression corpus without a codec dependency."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
# The fixture generator is imported from the source tree; never cache bytecode there.
sys.dont_write_bytecode = True


def block(kind, payload, last=False):
    return bytes([kind | (128 if last else 0)]) + len(payload).to_bytes(3, "big") + payload


def comments(entries):
    return struct.pack("<I", 5) + b"teder" + struct.pack("<I", len(entries)) + b"".join(
        struct.pack("<I", len(entry)) + entry for entry in entries)


def picture(description=b"cover"):
    mime, image = b"image/png", b"\x00\x01\xff\x00\x03"
    return (struct.pack(">II", 3, len(mime)) + mime + struct.pack(">I", len(description))
            + description + struct.pack(">IIIII", 320, 200, 24, 0, len(image)) + image)


def multichannel():
    spec = importlib.util.spec_from_file_location("reference", ROOT / "tests/fixtures/generate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = [[(-2147483648 if i == 0 else 2147483647 if i == 1 else (i - c) * 10000000)
             for c in range(8)] for i in range(16)]
    pcm = b"".join(x.to_bytes(4, "little", signed=True) for row in rows for x in row)
    info = (struct.pack(">HH", 16, 16) + bytes(6)
            + ((44100 << 44) | (7 << 41) | (31 << 36) | 16).to_bytes(8, "big")
            + hashlib.md5(pcm).digest())
    header = bytes([255, 248, 105, 112, 0, 15])
    frame = header + module.crc(header, 8, 7)
    for c in range(8):
        frame += b"\x02" + b"".join(row[c].to_bytes(4, "big", signed=True) for row in rows)
    frame += module.crc(frame, 16, 0x8005)
    return b"fLaC" + block(0, info, True) + frame


def generate(output):
    source, metadata = output / "source", output / "metadata"
    source.mkdir(parents=True, exist_ok=True)
    metadata.mkdir(parents=True, exist_ok=True)

    def save(folder, name, body, controls=bytes(16)):
        (folder / name).write_bytes(controls + body)

    tracks = {name: (ROOT / f"tests/fixtures/{name}.flac").read_bytes()
              for name in ("track-a", "track-b", "mono-24")}
    tracks["eight-32"] = multichannel()
    for name, body in tracks.items():
        for mode in range(4):
            control = bytearray(16)
            control[0], control[1], control[4], control[8] = mode, mode, mode & 1, 7
            save(source, f"{name}-{mode}", body, control)
    save(source, "truncated", tracks["track-a"][:-1])
    save(source, "empty", b"")
    save(source, "bad-crc", tracks["track-a"][:-1] + b"\xff")
    prefix = b"fLaC" + block(0, tracks["track-a"][8:42])
    tagset = [b"ARTIST=one", b"ARTIST=two", b"EMPTY=", b"NOTE=a\0b", "TITLE=ש".encode()]
    cases = {
        "tags-picture": prefix + block(4, comments(tagset)) + block(6, picture(), True),
        "tag-limit": prefix + block(4, comments([b"A=x"] * 1024), True),
        "tag-over": prefix + block(4, comments([b"A=x"] * 1025), True),
        "invalid-utf8": prefix + block(4, comments([b"TITLE=\xff"]), True),
        "nul-picture": prefix + block(6, picture(b"a\0b"), True),
        "block-over": prefix + block(1, b"") * 256 + block(1, b"", True),
        "truncated": prefix + b"\x84\xff\xff\xff",
        "empty": b"",
    }
    for name, body in cases.items():
        save(metadata, name, body)
    for mode in range(3):
        control = bytearray(16)
        control[mode] = (1, 1, 128)[mode]
        save(metadata, f"control-{mode}", cases["tags-picture"], control)


# Sequence opcodes and path selectors, matching fuzz/sequence.c.
(OPEN, TRANSPORT, SEEK, GAIN, PREPARE, CLEAR, PROBE, DEVICES, SELECT, ANALYSIS, GET, POLL,
 CANCEL, TAKE, ACK, EVENT, ACQUIRE, RELEASE_LOAN, RELEASE_RESULT, RENDER, LOSE, SETTLE, WAIT,
 STATUS) = range(24)
SHORT, LONG, MIXED, EMPTY, TEXT, DIRECTORY, MISSING, RELATIVE, NUL, UTF8, NO_BYTES, OVERSIZED = (
    range(12))
PLAY, PAUSE, STOP = 1, 2, 3


def header(capacity=8, events=8, pcm=2, slots=4, loans=4, spans=2, bands=2, engine=0,
           small_results=False, small_inputs=False, platform=0, fail_at=0):
    """Configuration header decoded by LLVMFuzzerTestOneInput."""
    fail = (128 | ((fail_at - 1) >> 8 & 7), (fail_at - 1) & 255) if fail_at else (0, 0)
    return bytes([(capacity - 1) | (events - 1) << 3,
                  pcm | (slots - 1) << 2 | (loans - 1) << 4 | (spans - 1) << 6,
                  bands | engine << 2 | small_results << 4 | small_inputs << 5,
                  platform, *fail, 0, 0])


def ops(*steps):
    return b"".join(bytes(step) for step in steps)


def render(times=4, frames=255):
    return [(RENDER, frames)] * times


def sequences(output):
    folder = output / "sequence"
    folder.mkdir(parents=True, exist_ok=True)
    def start(index=0):
        return [(SELECT, 0), (SETTLE, index)]

    seeds = {
        "play": ops((DEVICES, 0), (SETTLE, 0), (TAKE, 0), (SELECT, 1), (SETTLE, 1),
                    (OPEN, 1, SHORT), (SETTLE, 2), *render(8), (SETTLE, 2), (EVENT, 0),
                    (EVENT, 0), (POLL, 0), (ACK, 0), (ACK, 1), (ACK, 2), *render(16)),
        "transport": ops(*start(), (OPEN, 0, LONG), (SETTLE, 1), (TRANSPORT, PLAY, 0),
                         *render(2), (SETTLE, 2), (TRANSPORT, PAUSE, 0), (GAIN, 1 | 8),
                         *render(2), (SETTLE, 3), (SETTLE, 4), (TRANSPORT, PLAY, 0), *render(2),
                         (TRANSPORT, STOP, 0), (SETTLE, 5), (SETTLE, 6), (STATUS, 0)),
        "gapless": ops(*start(), (OPEN, 0, SHORT), (SETTLE, 1), (PREPARE, 0, 0, MIXED),
                       (SETTLE, 2), (TRANSPORT, PLAY, 0), (SETTLE, 3),
                       *[(RENDER, 15), (WAIT, 40)] * 16, (EVENT, 0), (EVENT, 0), (STATUS, 0),
                       (PREPARE, 1, 0, EMPTY), (SETTLE, 4), *render(8)),
        "gapless-playing": ops(*start(), (OPEN, 1, LONG), (SETTLE, 1), (PREPARE, 0, 0, SHORT),
                               (SETTLE, 2), *[(RENDER, 63), (WAIT, 20)] * 36, (EVENT, 0),
                               (EVENT, 0), (CLEAR, 0, 0), (SETTLE, 3)),
        "analysis": ops((ANALYSIS, 2 | 32 | 64, 7, 4), (SETTLE, 0), *start(1), (OPEN, 1, LONG),
                        (SETTLE, 2), *render(12), (WAIT, 255), (ACQUIRE, 0), (ACQUIRE, 0),
                        (RELEASE_LOAN, 0), (ANALYSIS, 1, 0, 0), (SETTLE, 3), *render(4),
                        (ACQUIRE, 0), (ANALYSIS, 0, 0, 0), (SETTLE, 4), (ACQUIRE, 0)),
        "probe": ops(*[step for path in range(7)
                       for step in ((PROBE, 1, path), (SETTLE, path), (TAKE, path))],
                     (RELEASE_RESULT, 0)),
        "seek": ops(*start(), (OPEN, 0, LONG), (SETTLE, 1), (SEEK, 1, 0, 0), (SETTLE, 2),
                    (SEEK, 2, 0, 0), (SETTLE, 3), (SEEK, 0, 0, 0), (SETTLE, 4),
                    (TRANSPORT, PLAY, 0), *render(4), (SEEK, 5, 0, 77), (SETTLE, 6), *render(4)),
        "loss": ops(*start(), (OPEN, 1, LONG), (SETTLE, 1), *render(2), (LOSE, 0), *render(2),
                    (WAIT, 255), (STATUS, 0), (EVENT, 0), (SELECT, 0), (SETTLE, 2),
                    (TRANSPORT, PLAY, 0), *render(4), (SETTLE, 3)),
        "cancel": ops((OPEN, 0, LONG), (CANCEL, 0), (PROBE, 1, LONG), (CANCEL, 1),
                      (DEVICES, 0), (CANCEL, 2), (SETTLE, 0), (SETTLE, 1), (SETTLE, 2),
                      (TAKE, 2), (ACK, 0), (ACK, 1), (ACK, 2)),
        "capacity": ops((DEVICES, 0), (DEVICES, 0), (SETTLE, 0), (ACK, 0), (DEVICES, 0),
                        (GET, 0), (GET, 1)),
        "invalid": ops(*[(OPEN, 0, path) for path in range(RELATIVE, OVERSIZED + 1)],
                       (OPEN, 6, SHORT), (TRANSPORT, 0, 0), (TRANSPORT, 4, 0), (SEEK, 4, 0, 0),
                       (GAIN, 4), (GAIN, 6), (GAIN, 16), (PROBE, 2, SHORT), (SELECT, 4),
                       (SELECT, 5), (SELECT, 3, 1),
                       *[(ANALYSIS, 128 | 2, violation, 6) for violation in range(12)],
                       (GET, 255), (TAKE, 254), (ACK, 252), (CANCEL, 250)),
        "faults": ops((DEVICES, 0), (SETTLE, 0), (TAKE, 0), *start(1), (OPEN, 1, SHORT),
                      (SETTLE, 2), (PROBE, 1, SHORT), (SETTLE, 3), (TAKE, 3), *render(4)),
    }
    configs = {"capacity": header(capacity=1), "faults": header(fail_at=40)}
    for name, body in seeds.items():
        (folder / name).write_bytes(configs.get(name, header()) + body)
    tight = header(engine=1, small_results=True, small_inputs=True)
    (folder / "tight").write_bytes(tight + seeds["probe"] + seeds["play"])
    failing = header(platform=1 | 2 << 2)
    (folder / "device-failure").write_bytes(failing + seeds["play"])
    for fail_at in (1, 3, 12, 90, 300):
        (folder / f"fault-{fail_at}").write_bytes(header(fail_at=fail_at) + seeds["play"])

    # Every live-engine fault is followed by unrelated operations and repeated reads.
    follow = ops((DEVICES, 0), (SETTLE, 1), (GAIN, 1), (SETTLE, 2), (GET, 0), (GET, 0))
    failures = {
        "oom-create-engine": (1, follow),
        "oom-create-snapshots": (2, follow),
        "oom-open": (3, ops((OPEN, 0, SHORT), (SETTLE, 0)) + follow),
        "oom-analysis": (3, ops((ANALYSIS, 2, 7, 4), (SETTLE, 0)) + follow),
        "oom-select-context": (3, ops((SELECT, 0), (SETTLE, 0)) + follow),
        "oom-select-device": (5, ops((SELECT, 0), (SETTLE, 0)) + follow),
        "oom-prepare": (19, ops((OPEN, 0, SHORT), (SETTLE, 0),
                                (PREPARE, 0, 0, MIXED), (SETTLE, 1),
                                (DEVICES, 0), (SETTLE, 2), (GAIN, 1), (SETTLE, 3),
                                (GET, 1), (GET, 1))),
    }
    for name, (fail_at, body) in failures.items():
        (folder / name).write_bytes(header(fail_at=fail_at) + body)


# Renderer opcodes, glyphs, floor-table indexes and record faults, matching fuzz/renderer.c.
(R_DRAW, R_PUT, R_BASE, R_STYLE, R_FLAGS, R_RESIZE, R_PUBLISH, R_ACQUIRE, R_RELEASE, R_DISOWN,
 R_CONFIGURE, R_RECREATE, R_BUILD_INFO) = range(13)
ASCII, WIDE, FAMILY, COMBINING, NO_FILL = 0, 2, 3, 4, 8
CURSOR = 0x80  # put at the cursor, after moving it to y, x when both are on the plane
F_DEFAULT, F_NEG_ZERO, F_24, F_TINY, F_HUGE, F_NAN, F_INF, F_NEG_INF, F_POSITIVE = (
    0, 1, 4, 5, 6, 7, 8, 9, 10)
SIZE, ABI, NULL_RECORD, FG_ALPHA, BG_ALPHA, PEAK_ALPHA, RESERVED, MODE = range(8, 16)
RAW, NEAR, NONE, NEWEST, OLDEST = 0x55, 0, 0, 1, 2


def style(mask=2, fg=0x40ABCDEF, bg=0x40987654):
    return struct.pack("<HII", mask, fg, bg)


def config(mode=0, floor=F_DEFAULT, fault=0, value=0, fg=0x33CC66, bg=0x112233, peak=0xFF9900,
           raw=None):
    colors = b"".join(color.to_bytes(3, "little") for color in (fg, bg, peak))
    return (bytes([fault | value << 4, mode]) + colors + bytes([floor if raw is None else 0xE0])
            + struct.pack("<I", raw or 0))


def xorshift(state):
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    return state ^ (state << 5) & 0xFFFFFFFF


def bands(count=8, generator=0, seed=1, rms=(0.5, 0.125), peak=(0.75, 0.25)):
    return struct.pack("<HBI4f", count, generator, seed, *rms, *peak)


def tie_band():
    """A seed whose first extremes-table band is -27 dB, an exact half at 45 rows."""
    return next(seed for seed in range(1, 1000) if xorshift(seed) % 12 == 5)


def scene(rows=6, cols=8, fill=ASCII, cfg=None, fail=False, loans=2, initial=None):
    pool = loans - 1 | (4 if initial is not None else 0)
    return (bytes([rows - 1, cols - 1, 2 | 3 << 4, fill]) + style() + (cfg or config())
            + bytes([fail, pool]) + (initial if initial is not None else bands(0)))


def draw(y, x, rows, cols, modes=RAW, renderer=0, plane=0, rect=0, snapshot=NEWEST):
    return (bytes([R_DRAW, renderer | plane << 2 | rect << 4 | snapshot << 6, modes])
            + struct.pack("<iiII", y, x, rows, cols))


def put(glyph, y, x, mask=0, fg=0x40102030, bg=0x40405060):
    return bytes([R_PUT, glyph, y, x]) + style(mask, fg, bg)


def renderers(output):
    folder = output / "renderer"
    folder.mkdir(parents=True, exist_ok=True)
    full = draw(0, 0, 6, 8)
    levels = config(mode=2, floor=F_24)
    bad = [config(fault=fault, value=value) for fault, value in (
        (SIZE, 0), (SIZE, 2), (SIZE, 5), (ABI, 0), (ABI, 3), (NULL_RECORD, 0), (FG_ALPHA, 7),
        (BG_ALPHA, 0), (PEAK_ALPHA, 3), (RESERVED, 15), (MODE, 0), (MODE, 15))]
    bad += [config(floor=floor) for floor in (F_NAN, F_INF, F_NEG_INF, F_POSITIVE)]
    bad.append(config(raw=0x7FC00001))
    seeds = {
        "spectrum": (scene(initial=bands()), full + draw(1, 1, 4, 4) + draw(0, 7, 6, 1)),
        "levels": (scene(cfg=levels, initial=bands()), draw(0, 0, 4, 2) + draw(0, 2, 4, 1)
                   + draw(0, 3, 6, 5)),
        "null-snapshot": (scene(initial=bands()), draw(0, 0, 6, 8, snapshot=NONE)),
        "null-arguments": (scene(initial=bands()), draw(0, 0, 6, 8, plane=3)
                           + draw(0, 0, 6, 8, rect=3) + draw(0, 0, 6, 8, renderer=3)
                           + draw(0, 0, 0, 0, plane=3, rect=3)),
        "zero-bands": (scene(initial=bands(0)), full),
        "extreme-rects": (scene(initial=bands()), b"".join(draw(*rect) for rect in (
            (-2**31, -2**31, 2**32 - 1, 2**32 - 1), (2**31 - 1, 2**31 - 1, 2**32 - 1, 2**32 - 1),
            (-2**31, -2**31, 1, 1), (0, 0, 0, 2**32 - 1), (0, 0, 1, 0), (-1, -1, 4, 4),
            (5, 7, 2**31, 2**31))) + draw(0, 0, 0, 0, modes=0xAA)),
        "wide-left": (scene(), put(WIDE, 2, 2) + draw(1, 3, 3, 2)),
        "wide-right": (scene(), put(WIDE, 2, 2) + draw(1, 1, 3, 2)),
        "wide-inside": (scene(fill=WIDE, initial=bands()), draw(0, 0, 6, 8) + draw(0, 2, 6, 2)),
        "graphemes": (scene(fill=NO_FILL, initial=bands()), put(FAMILY, 0, 0)
                      + put(COMBINING, 5, 7) + put(WIDE, 0, 6) + bytes([R_BASE, FAMILY])
                      + style(8) + draw(1, 1, 4, 5)),
        "resize": (scene(initial=bands()), full + bytes([R_RESIZE, 2, 3]) + full
                   + bytes([R_RESIZE, 47, 95]) + draw(-3, -3, 60, 120)),
        "scroll-autogrow": (scene(initial=bands()), bytes([R_FLAGS, 2]) + put(ASCII, 2, 8)
                            + bytes([R_FLAGS, 3]) + put(ASCII, 5, 9) + put(WIDE, 5, 9)
                            + draw(0, 0, 7, 10) + bytes([R_FLAGS, 1]) + full),
        "disowned": (scene(loans=3, initial=bands()), bytes([R_PUBLISH]) + bands(64, 1, 7)
                     + bytes([R_ACQUIRE, R_DISOWN, R_PUBLISH]) + bands() + bytes([R_ACQUIRE])
                     + draw(0, 0, 6, 8, snapshot=OLDEST) + full + bytes([R_RELEASE, 0]) + full),
        "loan-limit": (scene(loans=1, initial=bands()),
                       bytes([R_ACQUIRE, R_RELEASE, 0, R_ACQUIRE]) + full
                       + bytes([R_RELEASE, 0, R_ACQUIRE]) + full),
        "malformed": (scene(cfg=bad[0]),
                      full + b"".join(bytes([R_RECREATE, 0]) + cfg for cfg in bad) + full),
        "configure-failure": (scene(initial=bands()),
                              b"".join(bytes([R_CONFIGURE, 0]) + cfg for cfg in bad)
                              + full + bytes([R_CONFIGURE, 3]) + levels + full
                              + bytes([R_CONFIGURE, 0]) + levels + full),
        "no-memory": (scene(fail=True, initial=bands()), full + bytes([R_RECREATE, 1]) + config()
                      + full + bytes([R_RECREATE, 6]) + config() + full),
        "build-info": (scene(), bytes([R_BUILD_INFO, 0, R_BUILD_INFO, 5, R_BUILD_INFO, 5 | 2 << 3,
                                       R_BUILD_INFO, 5 | 4 << 3, R_BUILD_INFO, 6, R_BUILD_INFO,
                                       6 | 3 << 3, R_BUILD_INFO, 7])),
        "floors": (scene(initial=bands(256, 3, 99)), b"".join(
            bytes([R_RECREATE, 0]) + config(mode=mode, floor=floor) + full + draw(0, 0, 6, 300)
            for mode, floor in ((1, F_TINY), (2, F_TINY), (1, F_HUGE), (2, F_HUGE),
                                (1, F_NEG_ZERO)))
            + bytes([R_RECREATE, 0]) + config(mode=2, raw=0xC2700000) + full),
        "band-extremes": (scene(initial=bands(256, 2, 3, (0.0, 3e38), (1e-45, 1.0))),
                          full + draw(0, 0, 6, 1) + draw(0, 0, 6, 1000)
                          + bytes([R_CONFIGURE, 0]) + levels + full + draw(0, 0, 6, 1)),
        "rounding-tie": (scene(rows=45, cols=4, initial=bands(1, 2, tie_band())),
                         draw(0, 0, 45, 1) + draw(0, 1, 45, 3)),
        "style-base": (scene(fill=COMBINING, initial=bands()),
                       bytes([R_STYLE]) + style(0x1F, 0x48FF00FF, 0x7000FF00)
                       + bytes([R_BASE, WIDE]) + style(1) + draw(4, 4, 3, 3, modes=NEAR)
                       + draw(2, 2, 2, 2)),
        # A scrolling plane fills its last row, then scrolls twice: once for a wide glyph
        # that doesn't fit, once for a narrow one past the edge. Draws and resizes follow.
        "scroll-cursor": (scene(initial=bands()), bytes([R_FLAGS, 1]) + put(ASCII, 5, 7)
                          + put(WIDE | CURSOR, 5, 7) + put(WIDE | CURSOR, 7, 9)
                          + b"".join(put(1 | CURSOR, 7, 9) for _ in range(5)) + full
                          + bytes([R_RESIZE, 7, 7]) + draw(0, 0, 8, 8)
                          + bytes([R_RESIZE, 5, 5]) + full),
    }
    for name, (head, body) in seeds.items():
        (folder / name).write_bytes(head + body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    generate(parser.parse_args().output)
    sequences(parser.parse_args().output)
    renderers(parser.parse_args().output)
