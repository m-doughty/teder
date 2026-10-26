"""Check authored C formatting, or apply it with --write. Requires clang-format 18."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="check without edits (default)")
    mode.add_argument("--write", action="store_true", help="apply the shared C style")
    parser.add_argument("--clang-format", help="path to a clang-format 18 executable")
    args = parser.parse_args()
    formatter = args.clang_format or shutil.which("clang-format-18") or shutil.which("clang-format")
    if not formatter:
        parser.error("clang-format 18 is required; install it or pass --clang-format")
    try:
        version = subprocess.run([formatter, "--version"], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.error(f"cannot run clang-format: {error}")
    if not re.search(r"\bversion 18\.", version.stdout):
        parser.error(f"expected clang-format 18; got {version.stdout.strip()}")

    # Templates such as version.h.in contain non-C @variables@. Their generated
    # headers, build trees and downloaded dependencies are outside these roots.
    files = sorted(
        path
        for directory in ("src", "include", "tests", "cmake", "examples")
        for path in (ROOT / directory).rglob("*")
        if path.is_file() and not path.is_symlink()
        and path.suffix in (".c", ".h")
    )
    options = ["-i"] if args.write else ["--dry-run", "--Werror", "--ferror-limit=0"]
    failed = False
    for offset in range(0, len(files), 32):
        command = [formatter, f"--style=file:{ROOT / '.clang-format'}", *options]
        result = subprocess.run(command + [str(path) for path in files[offset:offset + 32]])
        failed |= result.returncode != 0
    if failed:
        return 1
    print(f"{'Formatted' if args.write else 'Checked'} {len(files)} C files; {version.stdout.strip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
