"""Replay every committed input, then optionally run a fixed CI smoke campaign."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
# Per-target input bound, optional dictionary and smoke iterations. Each sequence
# input creates and destroys a threaded engine, so its smoke run is shorter.
TARGETS = {
    "source": (65552, "flac.dict", 10000),
    "metadata": (65552, "flac.dict", 10000),
    "sequence": (4096, None, 2000),
}


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="limit to one or more targets (default: all)")
    args = parser.parse_args()
    build = args.build.resolve()
    for target in args.target or TARGETS:
        max_len, dictionary, runs = TARGETS[target]
        binary = build / "bin" / ("teder_fuzz_" + target)
        output = build / "fuzz-results" / target
        output.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, LLVM_PROFILE_FILE=str(output / "%p.profraw"),
                   ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        limits = ["-timeout=10", "-rss_limit_mb=1024", "-malloc_limit_mb=64",
                  f"-max_len={max_len}", "-print_final_stats=1", f"-artifact_prefix={output}/"]
        folders, files = inputs(target)
        with (output / "replay.log").open("w") as log:
            subprocess.run([str(binary), *limits, *map(str, files)], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        if args.smoke:
            corpus = output / "corpus"
            corpus.mkdir(exist_ok=True)
            extra = [f"-dict={ROOT / dictionary}"] if dictionary else []
            with (output / "smoke.log").open("w") as log:
                subprocess.run([str(binary), *limits, "-seed=2201", f"-runs={runs}", *extra,
                                str(corpus), *map(str, folders)], env=env,
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        print(f"{target}: replayed {len(files)} inputs"
              f"{f' and passed {runs}-run smoke' if args.smoke else ''}", flush=True)


if __name__ == "__main__":
    main()
