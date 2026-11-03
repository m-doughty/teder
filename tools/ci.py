"""Build and test one CI configuration with the same commands in GitHub Actions and locally.

Linux configurations build the pinned m-doughty/notcurses fork with their own compiler and
sanitizer flags, then build teder Debug with -Werror and the renderer and run CTest. linux-shared
and linux-static also install teder and run the installed consumers against that notcurses.
linux-fuzz builds the static libFuzzer harnesses, renderer included, against a notcurses with
fuzzer and coverage instrumentation, then replays the corpora and runs fuzz/run.py's smoke.
macos-asan builds and tests the shared core library with Apple Clang, ASan and UBSan.

--phase build fetches sources and needs the network; --phase test runs offline from that build.
tools/run-ci-docker.py runs a Linux configuration in a pinned Ubuntu 24.04 container.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NOTCURSES_URL = "https://github.com/m-doughty/notcurses"
NOTCURSES_REVISION = "fd67f2bafec21365f72a418797799d2051376fbe"
# The C library alone, shared, without multimedia, tools, tests or generated documentation.
NOTCURSES_OPTIONS = (
    "-DUSE_MULTIMEDIA=none", "-DUSE_CXX=OFF", "-DUSE_DOCTEST=OFF", "-DUSE_PANDOC=OFF",
    "-DUSE_POC=OFF", "-DUSE_STATIC=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_EXECUTABLES=OFF",
    "-DBUILD_FFI_LIBRARY=OFF",
)
# cmake/Platform.cmake instruments teder and its bundled dependencies with these sanitizers.
# notcurses gets the same instrumentation, so one sanitizer runtime serves the whole process.
SANITIZER_FLAGS = {
    "none": "",
    "address": "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer",
    "thread": "-fsanitize=thread -fno-omit-frame-pointer",
}
SANITIZER_PACKAGES = ("libasan8", "libtsan2", "libclang-rt-18-dev", "llvm-18")
# Fuzzing gives notcurses libFuzzer feedback and source coverage, as cmake/Fuzzing.cmake does
# for teder. The shared library links its own profile runtime and writes its own profile.
FUZZ_FLAGS = "-fsanitize=fuzzer-no-link -fprofile-instr-generate -fcoverage-mapping"
FUZZ_LINK_FLAGS = "-fprofile-instr-generate"
FUZZ_OPTIONS = ("-DCMAKE_C_FLAGS_DEBUG=-O1 -g", "-DBUILD_TESTING=OFF", "-DTEDER_BUILD_EXAMPLES=OFF",
                "-DTEDER_BUILD_FUZZERS=ON")
PARALLEL = "4"


def configuration(cc, cxx, shared, sanitizer, renderer, consumers, host="linux", fuzz=False):
    return {"cc": cc, "cxx": cxx, "shared": shared, "sanitizer": sanitizer,
            "renderer": renderer, "consumers": consumers, "host": host, "fuzz": fuzz}


CONFIGURATIONS = {
    "linux-shared": configuration("gcc", "g++", "ON", "none", renderer=True, consumers=True),
    "linux-static": configuration("gcc", "g++", "OFF", "none", renderer=True, consumers=True),
    "linux-asan": configuration("clang-18", "clang++-18", "ON", "address", renderer=True, consumers=False),
    "linux-tsan": configuration("gcc", "g++", "ON", "thread", renderer=True, consumers=False),
    "linux-fuzz": configuration("clang-18", "clang++-18", "OFF", "address", renderer=True,
                                consumers=False, fuzz=True),
    "macos-asan": configuration("cc", "c++", "ON", "address", renderer=False, consumers=False, host="darwin"),
}


def run(*args):
    command = [str(arg) for arg in args]
    print("+ " + shlex.join(command), flush=True)
    try:
        result = subprocess.run(command)
    except OSError as error:
        raise SystemExit(f"ci.py: cannot run {command[0]}: {error}")
    if result.returncode != 0:
        print(f"ci.py: {command[0]} failed with status {result.returncode}: {shlex.join(command)}",
              file=sys.stderr, flush=True)
        # A signal-terminated child reports a negative status; exit as a shell would.
        raise SystemExit(result.returncode if result.returncode > 0 else 128 - result.returncode)


def capture(*args):
    command = [str(arg) for arg in args]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"ci.py: {shlex.join(command)} failed: {error}")
    return result.stdout.strip()


def show_versions(config):
    commands = [("uname", "-a"), (config["cc"], "--version"), (config["cxx"], "--version"),
                ("cmake", "--version"), ("ninja", "--version"), (sys.executable, "--version")]
    if config["host"] == "linux":
        commands.append(("ldd", "--version"))
    for command in commands:
        output = capture(*command)
        if command[0] == "ldd":
            output = output.splitlines()[0]
        print(f"$ {shlex.join(command)}\n{output}", flush=True)
    if config["host"] == "linux":
        for package in SANITIZER_PACKAGES:
            result = subprocess.run(["dpkg-query", "-W", "-f", "${db:Status-Abbrev}|${Version}", package],
                                    capture_output=True, text=True)
            status, _, version = result.stdout.partition("|")
            installed = result.returncode == 0 and status.startswith("ii")
            print(f"{package}: {version if installed else 'not installed'}", flush=True)


def notcurses_library(prefix):
    for directory in ("lib", "lib64"):
        library = prefix / directory / "libnotcurses-core.so"
        if library.is_file():
            return library
    return None


def build_notcurses(config, paths):
    flags = link_flags = SANITIZER_FLAGS[config["sanitizer"]]
    if config["fuzz"]:
        flags, link_flags = f"{flags} {FUZZ_FLAGS}", f"{link_flags} {FUZZ_LINK_FLAGS}"
    # The stamp lives in the installed prefix, which CI restores from its cache without the
    # source or build trees. Any change of revision, compiler, flags or options rebuilds.
    stamp = {"url": NOTCURSES_URL, "revision": NOTCURSES_REVISION, "cc": config["cc"],
             "compiler": capture(config["cc"], "--version").splitlines()[0],
             "flags": flags, "link_flags": link_flags, "options": list(NOTCURSES_OPTIONS)}
    stamp_path = paths["notcurses_install"] / "ci-stamp.json"
    if stamp_path.is_file() and notcurses_library(paths["notcurses_install"]) \
            and (paths["notcurses_install"] / "include" / "notcurses" / "notcurses.h").is_file():
        try:
            cached = json.loads(stamp_path.read_text(encoding="utf-8"))
        except ValueError:
            cached = None
        if cached == stamp:
            print(f"notcurses {NOTCURSES_REVISION} is already installed in "
                  f"{paths['notcurses_install']} with this configuration", flush=True)
            return
    if stamp_path.exists():
        stamp_path.unlink()

    source = paths["notcurses_source"]
    source.mkdir(parents=True, exist_ok=True)
    if not (source / ".git").exists():
        run("git", "init", "--quiet", source)
    # No tag holds the pinned commit, so fetch it by its full hash and verify the checkout.
    run("git", "-C", source, "fetch", "--quiet", "--depth", "1", NOTCURSES_URL, NOTCURSES_REVISION)
    run("git", "-C", source, "-c", "advice.detachedHead=false", "checkout", "--quiet", "--detach",
        "FETCH_HEAD")
    head = capture("git", "-C", source, "rev-parse", "HEAD")
    if head != NOTCURSES_REVISION:
        raise SystemExit(f"ci.py: notcurses checkout is {head}, expected {NOTCURSES_REVISION}")

    # --fresh discards any cache an earlier compiler or flag set left in a reused build tree.
    run("cmake", "--fresh", "-S", source, "-B", paths["notcurses_build"], "-G", "Ninja",
        f"-DCMAKE_C_COMPILER={config['cc']}", "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_INSTALL_PREFIX={paths['notcurses_install']}", f"-DCMAKE_C_FLAGS={flags}",
        f"-DCMAKE_SHARED_LINKER_FLAGS={link_flags}", *NOTCURSES_OPTIONS)
    run("cmake", "--build", paths["notcurses_build"], "--parallel", PARALLEL)
    run("cmake", "--install", paths["notcurses_build"])
    if not notcurses_library(paths["notcurses_install"]):
        raise SystemExit(f"ci.py: no shared libnotcurses-core in {paths['notcurses_install']}")
    with stamp_path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(stamp, indent=2) + "\n")


def build(config, paths):
    if config["renderer"]:
        build_notcurses(config, paths)
        renderer = ["-DTEDER_WITH_NOTCURSES=ON",
                    f"-DTEDER_NOTCURSES_PREFIX={paths['notcurses_install']}",
                    f"-DTEDER_NOTCURSES_REVISION={NOTCURSES_REVISION}"]
    else:
        renderer = ["-DTEDER_WITH_NOTCURSES=OFF"]
    # libFLAC's upstream configure checks need a C++ compiler; it matches the C compiler.
    run("cmake", "-S", ROOT, "-B", paths["build"], "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_C_COMPILER={config['cc']}", f"-DCMAKE_CXX_COMPILER={config['cxx']}",
        f"-DBUILD_SHARED_LIBS={config['shared']}", "-DTEDER_WERROR=ON",
        f"-DTEDER_SANITIZER={config['sanitizer']}", *renderer,
        *(FUZZ_OPTIONS if config["fuzz"] else ()))
    run("cmake", "--build", paths["build"], "--parallel", PARALLEL)


def test(config, paths):
    if not (paths["build"] / "CMakeCache.txt").is_file():
        raise SystemExit(f"ci.py: no build in {paths['build']}; run --phase build first")
    if config["fuzz"]:
        # Replays every committed input, then runs each target's fixed smoke campaign.
        run(sys.executable, ROOT / "fuzz" / "run.py", paths["build"], "--smoke")
        return
    ctest = ["ctest", "--output-on-failure", "--no-tests=error", "-j", PARALLEL]
    launcher = []
    if config["sanitizer"] == "thread" and config["host"] == "linux":
        # High-entropy ASLR can map binaries outside ThreadSanitizer's address ranges. setarch -R
        # disables randomization for the test processes only, without an architecture argument,
        # so the command is the same on x86_64 and aarch64; host settings stay unchanged.
        launcher = ["setarch", "-R"]
    run(*launcher, *ctest, "--test-dir", paths["build"])
    if not config["consumers"]:
        return

    library = notcurses_library(paths["notcurses_install"])
    if not library:
        raise SystemExit(f"ci.py: no shared libnotcurses-core in {paths['notcurses_install']}")
    run("cmake", "--install", paths["build"], "--prefix", paths["install"])
    # Static teder packages need the renderer's notcurses library path from the consumer.
    run("cmake", "-S", ROOT / "tests" / "consumer", "-B", paths["consumer"], "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_C_COMPILER={config['cc']}",
        f"-DCMAKE_PREFIX_PATH={paths['install']}",
        f"-DTEDER_NOTCURSES_PREFIX={paths['notcurses_install']}",
        f"-DTEDER_NOTCURSES_LIBRARY={library}")
    run("cmake", "--build", paths["consumer"], "--parallel", PARALLEL)
    run(*ctest, "--test-dir", paths["consumer"])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("configuration", choices=sorted(CONFIGURATIONS))
    parser.add_argument("--build-root", type=Path, default=ROOT / "build-ci",
                        help="directory for every source, build and install tree (default: build-ci)")
    parser.add_argument("--phase", choices=("build", "test", "all"), default="all",
                        help="build (needs the network), test (offline) or both (default)")
    args = parser.parse_args()
    config = CONFIGURATIONS[args.configuration]
    if not sys.platform.startswith(config["host"]):
        if config["host"] == "linux":
            parser.error(f"{args.configuration} needs Linux; elsewhere use tools/run-ci-docker.py")
        parser.error(f"{args.configuration} needs macOS")

    root = args.build_root.resolve()
    work = root / args.configuration
    notcurses = root / "notcurses" / args.configuration
    paths = {"build": work / "build", "install": work / "install", "consumer": work / "consumer",
             "notcurses_source": notcurses / "src", "notcurses_build": notcurses / "build",
             "notcurses_install": notcurses / "install"}
    show_versions(config)
    if args.phase in ("build", "all"):
        build(config, paths)
    if args.phase in ("test", "all"):
        test(config, paths)
    print(f"ci.py: {args.configuration} {args.phase} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
