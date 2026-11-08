"""Replay every committed input, then optionally run a fixed CI smoke campaign."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
# Exact expectations for generated seeds that must succeed; see fuzz/README.md.
KNOWN = ROOT / "known-good.txt"
KNOWN_CHECKED = "known seed checked: "
# Per-target input bound, optional dictionary and smoke iterations. Each sequence
# input creates and destroys a threaded engine, so its smoke run is shorter. A
# renderer input is a 55-byte header and at most 64 operations of up to 24 bytes.
TARGETS = {
    "source": (65552, "flac.dict", 10000),
    "metadata": (65552, "flac.dict", 10000),
    "sequence": (4096, None, 2000),
    "renderer": (1591, None, 30000),
}
# Built only when the fuzz build is configured with TEDER_WITH_NOTCURSES=ON.
RENDERER_TARGETS = {"renderer"}


def inputs(target):
    """Generated seeds plus preserved, minimized fuzzer findings."""
    folders = [ROOT / "corpus" / target]
    regressions = ROOT / "regressions" / target
    if regressions.is_dir():
        folders.append(regressions)
    files = sorted(path for folder in folders for path in folder.iterdir())
    if not files or any(not path.is_file() for path in files):
        raise RuntimeError(f"invalid or missing corpus: {target}")
    return folders, files


def fnv(data):
    """64-bit FNV-1a, as fz_hash_bytes computes it in fuzz/support.c."""
    state = 0xCBF29CE484222325
    for byte in data:
        state = ((state ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return state


def known_seeds(target, files):
    """Names of the manifest's seeds for target, each proved to be a committed input.

    The harness finds an input's expectations by the hash of its bytes, so an edited seed
    would silently fall back to the broad check; fail here instead."""
    by_hash = {fnv(path.read_bytes()): path for path in files}
    names = set()
    for line in KNOWN.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#") or fields[0] != target:
            continue
        digest = next(field[5:] for field in fields if field.startswith("hash="))
        if int(digest, 16) not in by_hash or by_hash[int(digest, 16)].name != fields[1]:
            raise RuntimeError(f"known-good.txt is stale: no {target} input {fields[1]} "
                               f"with hash {digest}; regenerate it with seeds.py")
        names.add(fields[1])
    return names


def notcurses_enabled(build):
    """Read the renderer option from the build's CMake cache."""
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"not a configured CMake build: {build}")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("TEDER_WITH_NOTCURSES:"):
            return line.partition("=")[2].strip().upper() in ("ON", "1", "TRUE", "YES", "Y")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="limit to one or more targets (default: all)")
    args = parser.parse_args()
    build = args.build.resolve()
    renderer = notcurses_enabled(build)
    for target in args.target or TARGETS:
        if target in RENDERER_TARGETS and not renderer:
            if args.target:
                raise RuntimeError(f"{target} requires a build with TEDER_WITH_NOTCURSES=ON")
            print(f"{target}: not built (TEDER_WITH_NOTCURSES=OFF)", flush=True)
            continue
        max_len, dictionary, runs = TARGETS[target]
        binary = build / "bin" / ("teder_fuzz_" + target)
        output = build / "fuzz-results" / target
        output.mkdir(parents=True, exist_ok=True)
        # %m separates the profiles of instrumented shared libraries from the executable's.
        env = dict(os.environ, LLVM_PROFILE_FILE=str(output / "%p-%m.profraw"),
                   ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   TEDER_FUZZ_KNOWN=str(KNOWN))
        limits = ["-timeout=10", "-rss_limit_mb=1024", "-malloc_limit_mb=64",
                  f"-max_len={max_len}", "-print_final_stats=1", f"-artifact_prefix={output}/"]
        folders, files = inputs(target)
        known = known_seeds(target, files)
        with (output / "replay.log").open("w") as log:
            # The renderer's notcurses context reads standard input on its own thread.
            subprocess.run([str(binary), *limits, *map(str, files)], env=env,
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=300)
        # Every manifest seed must have reached its exact checks, none counted twice.
        log_text = (output / "replay.log").read_text(encoding="utf-8", errors="replace")
        checked = [line[len(KNOWN_CHECKED):].strip() for line in log_text.splitlines()
                   if line.startswith(KNOWN_CHECKED)]
        if sorted(checked) != sorted(known):
            raise RuntimeError(f"{target}: replay checked {sorted(checked)}, "
                               f"expected {sorted(known)}")
        if args.smoke:
            corpus = output / "corpus"
            corpus.mkdir(exist_ok=True)
            extra = [f"-dict={ROOT / dictionary}"] if dictionary else []
            with (output / "smoke.log").open("w") as log:
                subprocess.run([str(binary), *limits, "-seed=2201", f"-runs={runs}", *extra,
                                str(corpus), *map(str, folders)], env=env,
                               stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=300)
        print(f"{target}: replayed {len(files)} inputs"
              f"{f', {len(known)} against exact expectations' if known else ''}"
              f"{f' and passed {runs}-run smoke' if args.smoke else ''}", flush=True)


if __name__ == "__main__":
    main()
