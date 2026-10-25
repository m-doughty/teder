"""CC0-1.0: original synthetic samples; no encoder/decoder dependency.
Regenerate committed fixtures only. Expected C samples are maintained separately.
FLAC verbatim framing follows RFC 9639 sections 8 and 9.
"""
from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).resolve().parent


def crc(data, width, polynomial):
    value = 0
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = (value << 1) ^ (polynomial if value & (1 << (width - 1)) else 0)
            value &= (1 << width) - 1
    return value.to_bytes(width // 8, "big")


def fixture(name, bits, rate, samples):
    channels, frames = len(samples[0]), len(samples)
    pcm = b"".join(x.to_bytes(bits // 8, "little", signed=True) for row in samples for x in row)
    stream = struct.pack(">HH", frames, frames) + bytes(6)
    stream += ((rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | frames).to_bytes(8, "big")
    stream += hashlib.md5(pcm).digest()
    title = ("TITLE=" + name).encode("utf-8")
    comment = struct.pack("<I", 5) + b"teder" + struct.pack("<II", 1, len(title)) + title
    header = bytes([0xff, 0xf8, 0x60 | {44100: 9, 48000: 10}[rate],
                    ((channels - 1) << 4) | {16: 8, 24: 12}[bits], 0, frames - 1])
    frame = header + crc(header, 8, 0x07)
    for channel in range(channels):
        frame += b"\x02" + b"".join(row[channel].to_bytes(bits // 8, "big", signed=True) for row in samples)
    frame += crc(frame, 16, 0x8005)
    flac = b"fLaC\x00\x00\x00\x22" + stream + b"\x84" + len(comment).to_bytes(3, "big") + comment + frame
    for suffix, data in [("pcm", pcm), ("flac", flac)]:
        (ROOT / f"{name}.{suffix}").write_bytes(data)
        print(hashlib.sha256(data).hexdigest(), f"{name}.{suffix}")


if __name__ == "__main__":
    fixture("track-a", 16, 44100, [(i * 1000 - 8000, 8000 - i * 1000) for i in range(16)])
    fixture("track-b", 16, 44100, [(16000 + i * 500, -16000 - i * 500) for i in range(16)])
    fixture("mono-24", 24, 48000, [(-8388608,), (8388607,)] + [(i * 100000 - 600000,) for i in range(14)])
