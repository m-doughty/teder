"""Compare public headers and real library exports with the reviewed ABI 1 inventory."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys


def require_equal(label, expected, actual):
    if expected != actual:
        raise ValueError(f"{label}: missing={sorted(expected - actual)}, extra={sorted(actual - expected)}")


def headers(directory, reference):
    source = "\n".join((directory / n).read_text(encoding="utf-8")
                       for n in ("teder.h", "teder_notcurses.h", "teder/export.h"))
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    require_equal("public includes", {"stdint.h", "teder/export.h", "teder.h"},
                  set(re.findall(r'^#include\s*[<"]([^>"]+)[>"]', source, re.M)))
    require_equal("opaque handles", {"teder_engine", "teder_result", "teder_snapshot", "teder_renderer"},
                  set(re.findall(r"typedef struct (teder_\w+)\s+\1;", source)))
    functions = set(re.findall(r"\b(teder_\w+)\s*\(", source))
    expected = set(re.findall(r"ABI_FUNCTION\(\s*\w+,\s*(teder_\w+)", reference))
    require_equal("function coverage", expected, functions)
    records, fields = set(), set()
    for name, body in re.findall(r"typedef struct (teder_\w+)\s*\{(.*?)\}", source, re.S):
        records.add(name)
        for declaration in body.split(";"):
            if not declaration.strip():
                continue
            names = re.fullmatch(r"\s*(?:const\s+)?\w+\s+(.*?)\s*", declaration, re.S)[1]
            for member in names.split(","):
                fields.add((name, re.fullmatch(r"\s*\*?(\w+)(?:\[\d+\])?\s*", member)[1]))
    require_equal("record coverage", set(re.findall(r"ABI_RECORD\(\s*(\w+)", reference)), records)
    require_equal("field coverage", set(re.findall(r"ABI_FIELD\(\s*(\w+),\s*(\w+)", reference)), fields)
    constants = set()
    for body in re.findall(r"enum\s*\{(.*?)\}", source, re.S):
        constants.update(re.findall(r"\bTEDER_\w+\b", body))
    constants.update(re.findall(r"^#define[ \t]+(TEDER_\w+)[ \t]+[^\n]+", source, re.M))
    constants -= {"TEDER_API", "TEDER_CALL", "TEDER_NOTCURSES_API"}
    require_equal("constant coverage", set(re.findall(r"ABI_VALUE\(\s*(\w+)", reference)), constants)
    return functions


def run(tool, *args):
    if not tool:
        raise ValueError("Required symbol inspection tool is unavailable")
    return subprocess.run([tool, *args], check=True, capture_output=True, text=True).stdout


def exports(binary, nm, objdump):
    with binary.open("rb") as stream:
        magic = stream.read(8)
    if magic.startswith(b"MZ"):
        text = run(objdump or shutil.which("objdump"), "-p", str(binary))
        table = text.split("[Ordinal/Name Pointer] Table", 1)[1]
        names = set(re.findall(r"^\s*\[\s*\d+\].*?\s([A-Za-z_]\w*)\s*$", table, re.M))
        count = int(re.search(r"Export Address Table\s+([0-9a-fA-F]+)", text)[1], 16)
        if len(names) != count:
            raise ValueError("PE contains unnamed, forwarded or unparsed exports")
        return names, False
    archive = magic == b"!<arch>\n"
    if sys.platform == "darwin":
        names = run(nm or shutil.which("nm"), "-gUj", str(binary)).splitlines()
        return {n[1:] for n in names if n.startswith("_")}, archive
    if not archive and not magic.startswith(b"\x7fELF"):
        raise ValueError("Unsupported binary format")
    args = ["-g", "--defined-only", "--format=posix"]
    if not archive:
        args.append("--dynamic")
    text = run(nm or shutil.which("nm"), *args, str(binary))
    return set(re.findall(r"^(\S+) [A-Za-z] ", text, re.M)), archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--renderer", action="store_true")
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--nm")
    parser.add_argument("--objdump")
    args = parser.parse_args()
    reference = Path(__file__).with_name("contract.h").read_text()
    functions = headers(args.headers, reference)
    expected = {n for n in functions if n.startswith("teder_renderer_") == args.renderer}
    if args.binary:
        actual, archive = exports(args.binary, args.nm, args.objdump)
        if archive:
            # Archive internals can be linker-visible; only teder_* are public contracts.
            actual = {n for n in actual if n.startswith("teder_")}
        if args.negative:
            require_equal("negative fixture exports", {"teder_unplanned_export"}, actual)
            try:
                require_equal("exports", expected, actual)
            except ValueError:
                print("Rejected the deliberate unplanned export")
                return 0
            raise ValueError("Unexpected export escaped the audit")
        require_equal("exports", expected, actual)
        print(f"Verified {len(expected)} exports: {args.binary}")
    print("Public ABI inventory coverage passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, IndexError, TypeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
