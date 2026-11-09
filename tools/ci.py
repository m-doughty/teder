"""Build and test one CI configuration with the same commands in GitHub Actions and locally.

Linux configurations build the pinned m-doughty/notcurses fork with their own compiler and
sanitizer flags, then build teder Debug with -Werror and the renderer and run CTest. linux-shared
and linux-static also install teder and run the installed consumers against that notcurses.
linux-shared first runs the fork's own resize and autogrow doctest cases, from a separate test
build of the same checkout.
linux-fuzz builds the static libFuzzer harnesses, renderer included, against a notcurses with
fuzzer and coverage instrumentation, then replays the corpora and runs fuzz/run.py's smoke.
macos-asan builds and tests the shared core library with Apple Clang, ASan and UBSan.

The notcurses source tree must hold the pinned commit unmodified, and a reused notcurses
installation must match the SHA-256 digests its stamp recorded; either difference stops the run.
Without a matching stamp, notcurses is installed again into an emptied prefix. ci.py empties
only a prefix holding its stamp. It never follows a link in a prefix, and stops at one it could
not check: a link to a directory, or one whose target leaves the prefix.
--phase build fetches sources and needs the network; --phase test runs offline from that build.
--cache-key prints the digest that CI's cache key for the notcurses installation includes.
tools/run-ci-docker.py runs a Linux configuration in a pinned Ubuntu 24.04 container.
tools/test-ci.py tests the source tree and installation checks.
"""
import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path
import posixpath
import re
import shlex
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NOTCURSES_URL = "https://github.com/m-doughty/notcurses"
NOTCURSES_REVISION = "cd27fd8578cd990cf0bd44a9332e5d2891122a35"
# The C library alone, shared, without multimedia, tools, tests or generated documentation.
NOTCURSES_OPTIONS = (
    "-DUSE_MULTIMEDIA=none", "-DUSE_CXX=OFF", "-DUSE_DOCTEST=OFF", "-DUSE_PANDOC=OFF",
    "-DUSE_POC=OFF", "-DUSE_STATIC=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_EXECUTABLES=OFF",
    "-DBUILD_FFI_LIBRARY=OFF",
)
# Written into the notcurses installation prefix; every other file there is digested.
NOTCURSES_STAMP = "ci-stamp.json"
# The test build turns on the C++ wrapper and doctest, which the installed build leaves off.
# Only its notcurses-tester target is built: the geometry tester needs static libraries.
NOTCURSES_TEST_OPTIONS = (
    "-DUSE_MULTIMEDIA=none", "-DUSE_CXX=ON", "-DUSE_DOCTEST=ON", "-DUSE_PANDOC=OFF",
    "-DUSE_POC=OFF", "-DUSE_STATIC=OFF", "-DBUILD_TESTING=ON", "-DBUILD_EXECUTABLES=OFF",
    "-DBUILD_FFI_LIBRARY=OFF",
)
# The fork's own cases for the plane geometry teder's renderer relies on. Selecting by source
# file runs every case in these files, including cases added after this list was written.
NOTCURSES_TEST_SOURCES = ("resize", "autogrow")
# The fork's CI runs its tester headless with this terminal; the locale is the hosted runners'.
# notcurses-tester exits successfully without running anything when TERM is unset.
NOTCURSES_TEST_ENVIRONMENT = {"TERM": "xterm", "COLORTERM": "truecolor", "LANG": "C.UTF-8",
                              "LC_ALL": "C.UTF-8"}
NOTCURSES_TEST_TIMEOUT = 300
NOTCURSES_TEST_PACKAGES = ("doctest-dev",)
DOCTEST_CASES = re.compile(
    r"^\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", re.MULTILINE)
DOCTEST_ASSERTIONS = re.compile(r"^\[doctest\] assertions:\s*(\d+)\s*\|", re.MULTILINE)
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


def configuration(cc, cxx, shared, sanitizer, renderer, consumers, host="linux", fuzz=False,
                  notcurses_tests=False):
    return {"cc": cc, "cxx": cxx, "shared": shared, "sanitizer": sanitizer,
            "renderer": renderer, "consumers": consumers, "host": host, "fuzz": fuzz,
            "notcurses_tests": notcurses_tests}


CONFIGURATIONS = {
    # One configuration per hosted Linux architecture runs the fork's own cases.
    "linux-shared": configuration("gcc", "g++", "ON", "none", renderer=True, consumers=True,
                                  notcurses_tests=True),
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
        packages = SANITIZER_PACKAGES
        if config["notcurses_tests"]:
            packages += NOTCURSES_TEST_PACKAGES
        for package in packages:
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


def listing(lines, limit=20):
    """The first lines of a long report, and how many more there are."""
    shown = [f"  {line}" for line in lines[:limit]]
    if len(lines) > limit:
        shown.append(f"  and {len(lines) - limit} more")
    return "\n".join(shown)


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as data:
        for block in iter(lambda: data.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def link_leaves(prefix, link, target):
    """Whether a symbolic link at link, under prefix, has a target outside prefix.

    The target is resolved lexically against the link's directory, without following this or
    any other link: an absolute target, or a relative one that climbs out with "..", leaves.
    ci.py runs on Linux and macOS, where targets are POSIX paths. A backslash or a drive, which
    Windows would read as a separator or a root, counts as leaving too."""
    if "\\" in target or ntpath.splitdrive(target)[0]:
        return True
    root = posixpath.normpath(os.path.abspath(prefix))
    resolved = posixpath.normpath(posixpath.join(os.path.abspath(link.parent), target))
    return resolved != root and not resolved.startswith(root + "/")


def install_digests(prefix):
    """Map each path under prefix but the stamp, as a relative POSIX path, to its digest.

    Every regular file is hashed, whatever the platform names the library, its versioned
    aliases or its import library. A symbolic link whose target stays inside the prefix
    records that target, which is hashed under its own path; a dangling one records it too.
    Links are never followed, so a link whose target leaves the prefix, a link to a
    directory, or a linked prefix would hide what lies behind it from the digests; each
    stops the run."""
    def unreadable(error):
        raise SystemExit(f"ci.py: cannot read the notcurses installation in {prefix}: {error}")

    if prefix.is_symlink():
        raise SystemExit(f"ci.py: the notcurses installation {prefix} is a link. ci.py does not "
                         "follow links, so it cannot check what the link points to. Install "
                         "into a directory instead.")
    digests = {}
    for directory, folders, files in os.walk(prefix, onerror=unreadable):
        for name in folders + files:
            path = Path(directory) / name
            relative = path.relative_to(prefix).as_posix()
            if relative == NOTCURSES_STAMP:
                continue
            if path.is_symlink():
                target = os.readlink(path)
                if link_leaves(prefix, path, target):
                    raise SystemExit(f"ci.py: {path} in the notcurses installation is a link to "
                                     f"{target}, outside {prefix}. ci.py does not follow links, "
                                     "so it cannot check what the link points to; the "
                                     "installation must hold it itself.")
                if path.is_dir():
                    raise SystemExit(f"ci.py: {path} in the notcurses installation is a link to a "
                                     "directory. ci.py does not follow links, so it cannot check "
                                     "the files there; the installation must hold them itself.")
                digests[relative] = "link:" + target
            elif path.is_file():
                digests[relative] = "sha256:" + file_sha256(path)
            elif not path.is_dir():
                raise SystemExit(f"ci.py: {path} in the notcurses installation is not a file, "
                                 "directory or link")
    return dict(sorted(digests.items()))


def recorded_digests(stamp_path, stamp):
    """The digests a stamp for this configuration recorded, or None if notcurses must be built.

    A missing, unreadable or older stamp, or one for another configuration, means a rebuild."""
    if not stamp_path.is_file():
        return None
    try:
        cached = json.loads(stamp_path.read_text(encoding="utf-8"))
    except ValueError:
        return None
    if not isinstance(cached, dict):
        return None
    files = cached.pop("files", None)
    if cached != stamp or not isinstance(files, dict) or not files:
        return None
    if not all(isinstance(key, str) and isinstance(value, str) for key, value in files.items()):
        return None
    return files


def verify_install(prefix, recorded):
    """Stop unless every installed file matches the digest the stamp recorded for it."""
    actual = install_digests(prefix)
    if actual == recorded:
        return
    differences = [f"changed: {path}" for path in sorted(recorded.keys() & actual.keys())
                   if recorded[path] != actual[path]]
    differences += [f"missing: {path}" for path in sorted(recorded.keys() - actual.keys())]
    differences += [f"added: {path}" for path in sorted(actual.keys() - recorded.keys())]
    raise SystemExit(f"ci.py: the notcurses installation in {prefix} differs from the SHA-256 "
                     f"digests in its {NOTCURSES_STAMP}:\n{listing(differences)}\n"
                     f"ci.py does not reuse or repair it. Remove {prefix} to rebuild it.")


def write_stamp(stamp_path, stamp):
    with stamp_path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(stamp, indent=2) + "\n")


def empty_install(prefix):
    """Empty a notcurses installation prefix that ci.py made, before installing into it again.

    ci.py made a prefix that holds its stamp, whatever the stamp says. A missing or empty
    prefix needs nothing. Any other prefix stops the run untouched. Links inside are removed,
    never followed."""
    if prefix.is_symlink() or (prefix.exists() and not prefix.is_dir()):
        raise SystemExit(f"ci.py: the notcurses installation {prefix} is a link or a file, not a "
                         "directory ci.py made. Remove it to rebuild notcurses there.")
    try:
        if not prefix.exists() or not any(prefix.iterdir()):
            return
    except OSError as error:
        raise SystemExit(f"ci.py: cannot read the notcurses installation in {prefix}: {error}")
    stamp_path = prefix / NOTCURSES_STAMP
    if stamp_path.is_symlink() or not stamp_path.is_file():
        raise SystemExit(f"ci.py: {prefix} holds files but no {NOTCURSES_STAMP}, so ci.py did "
                         "not install them. It empties only a notcurses installation it made. "
                         f"Remove {prefix} to rebuild it.")
    try:
        shutil.rmtree(prefix)
    except OSError as error:
        raise SystemExit(f"ci.py: cannot empty the notcurses installation {prefix}: {error}")


def checkout_notcurses(source):
    """Check out the pinned commit in source and stop unless the tree matches it exactly."""
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
    # A reused tree keeps local edits and new files across the checkout, and the build would
    # carry them under the pinned revision. Ignored files count too: the fork's CMake globs
    # src/lib/*.c, and its .gitignore matches names such as build*. Skip-worktree and
    # assume-unchanged entries would hide edits from status. Never clean the tree here: a
    # local edit there may be deliberate.
    status = capture("git", "-C", source, "status", "--porcelain", "--untracked-files=all",
                     "--ignored").splitlines()
    differences = [line.strip() for line in status]
    # ls-files -v tags an ordinary tracked file H; S is skip-worktree, lowercase is
    # assume-unchanged.
    for entry in capture("git", "-C", source, "ls-files", "-v").splitlines():
        tag, _, name = entry.partition(" ")
        if tag != "H":
            flag = "assume-unchanged" if tag.islower() else "skip-worktree" if tag == "S" else tag
            differences.append(f"{flag} in the index: {name}")
    if differences:
        raise SystemExit(f"ci.py: the notcurses source tree {source} differs from "
                         f"{NOTCURSES_REVISION}:\n{listing(differences)}\n"
                         f"ci.py builds only the unmodified pinned commit, and does not clean "
                         f"this tree. Keep local changes elsewhere, or remove {source} to fetch "
                         "it again.")


def notcurses_flags(config):
    flags = link_flags = SANITIZER_FLAGS[config["sanitizer"]]
    if config["fuzz"]:
        flags, link_flags = f"{flags} {FUZZ_FLAGS}", f"{link_flags} {FUZZ_LINK_FLAGS}"
    return flags, link_flags


def notcurses_stamp(config):
    """What a notcurses installation's stamp records about its build, apart from its files.

    Any change of revision, compiler, flags or options gives a different stamp."""
    flags, link_flags = notcurses_flags(config)
    return {"url": NOTCURSES_URL, "revision": NOTCURSES_REVISION, "cc": config["cc"],
            "compiler": capture(config["cc"], "--version").splitlines()[0],
            "flags": flags, "link_flags": link_flags, "options": list(NOTCURSES_OPTIONS)}


def notcurses_cache_key(config):
    """The SHA-256 digest of the stamp a notcurses build in config writes, apart from its files.

    CI's cache key for the installation includes it, so a new compiler changes the key exactly
    when it changes the stamp, and a key that hits restores an installation whose stamp
    matches unless the installation itself changed."""
    stamp = json.dumps(notcurses_stamp(config), sort_keys=True)
    return hashlib.sha256(stamp.encode("utf-8")).hexdigest()


def install_notcurses(config, paths):
    """Build and install notcurses unless its stamp matches; True if this checked it out."""
    install = paths["notcurses_install"]
    # The stamp lives in the installed prefix, which CI restores from its cache without the
    # source or build trees. Any change of revision, compiler, flags or options rebuilds. A
    # matching stamp's digests must then match every installed file, or the run stops.
    stamp = notcurses_stamp(config)
    stamp_path = install / NOTCURSES_STAMP
    recorded = recorded_digests(stamp_path, stamp)
    if recorded is not None:
        verify_install(install, recorded)
        print(f"notcurses {NOTCURSES_REVISION} is already installed in {install} with this "
              f"configuration, and its {len(recorded)} files match their digests", flush=True)
        return False

    checkout_notcurses(paths["notcurses_source"])
    # Install into an empty prefix, so the stamp's digests cover only what this install
    # writes. Until they are recorded, a stamp without digests marks the prefix as ci.py's:
    # an install that stops part way is emptied and rebuilt by the next run.
    empty_install(install)
    install.mkdir(parents=True, exist_ok=True)
    write_stamp(stamp_path, stamp)
    # --fresh discards any cache an earlier compiler or flag set left in a reused build tree.
    run("cmake", "--fresh", "-S", paths["notcurses_source"], "-B", paths["notcurses_build"],
        "-G", "Ninja", f"-DCMAKE_C_COMPILER={config['cc']}", "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_INSTALL_PREFIX={install}", f"-DCMAKE_C_FLAGS={stamp['flags']}",
        f"-DCMAKE_SHARED_LINKER_FLAGS={stamp['link_flags']}", *NOTCURSES_OPTIONS)
    run("cmake", "--build", paths["notcurses_build"], "--parallel", PARALLEL)
    run("cmake", "--install", paths["notcurses_build"])
    if not notcurses_library(install):
        raise SystemExit(f"ci.py: no shared libnotcurses-core in {install}")
    if not (install / "include" / "notcurses" / "notcurses.h").is_file():
        raise SystemExit(f"ci.py: no notcurses/notcurses.h in {install / 'include'}")
    write_stamp(stamp_path, dict(stamp, files=install_digests(install)))
    return True


def build_notcurses_tests(config, paths, flags, link_flags):
    """Build notcurses-tester from the verified checkout, from scratch on every run."""
    tests = paths["notcurses_tests"]
    run("cmake", "--fresh", "-S", paths["notcurses_source"], "-B", tests, "-G", "Ninja",
        f"-DCMAKE_C_COMPILER={config['cc']}", f"-DCMAKE_CXX_COMPILER={config['cxx']}",
        "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS={link_flags}", f"-DCMAKE_SHARED_LINKER_FLAGS={link_flags}",
        *NOTCURSES_TEST_OPTIONS)
    run("cmake", "--build", tests, "--parallel", PARALLEL, "--clean-first", "--target",
        "notcurses-tester")


def build_notcurses(config, paths):
    checked_out = install_notcurses(config, paths)
    if config["notcurses_tests"]:
        # A cached installation skips the checkout, but the test build always needs one.
        if not checked_out:
            checkout_notcurses(paths["notcurses_source"])
        build_notcurses_tests(config, paths, *notcurses_flags(config))


def check_notcurses_report(name, report):
    """Stop unless a doctest report shows cases from the file that passed and checked something.

    A fork case returns early, passing, when notcurses cannot start."""
    cases, assertions = DOCTEST_CASES.search(report), DOCTEST_ASSERTIONS.search(report)
    if not cases or not assertions:
        raise SystemExit(f"ci.py: notcurses-tester reported no {name} results")
    total, passed, failed = (int(count) for count in cases.groups())
    if total == 0:
        raise SystemExit(f"ci.py: notcurses-tester selected no cases from src/tests/{name}.cpp")
    if failed or passed != total:
        raise SystemExit(f"ci.py: {failed} of {total} notcurses {name} cases failed")
    if int(assertions.group(1)) == 0:
        raise SystemExit(f"ci.py: the notcurses {name} cases checked nothing")


def run_notcurses_tests(paths):
    """Run every doctest case in each listed fork test file, headless, without a terminal."""
    tests = paths["notcurses_tests"]
    tester = tests / "notcurses-tester"
    if not tester.is_file():
        raise SystemExit(f"ci.py: no notcurses-tester in {tests}; run --phase build first")
    environment = dict(os.environ, **NOTCURSES_TEST_ENVIRONMENT)
    for name in NOTCURSES_TEST_SOURCES:
        report = tests / f"notcurses-tests-{name}.log"
        terminal = tests / f"notcurses-tests-{name}-terminal.log"
        if report.exists():
            report.unlink()
        command = [str(tester), "-p", str(paths["notcurses_source"] / "data"), "--no-colors=true",
                   f"--out={report}", f"--source-file=*src/tests/{name}.cpp"]
        print("+ " + shlex.join(command), flush=True)
        # Rendered frames go to a log rather than the console. A new session has no controlling
        # terminal, so notcurses cannot open /dev/tty, and stdin is a pipe closed at once.
        with terminal.open("wb") as output:
            try:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output,
                                           stderr=subprocess.PIPE, env=environment,
                                           start_new_session=True)
            except OSError as error:
                raise SystemExit(f"ci.py: cannot run {tester}: {error}")
            try:
                _, errors = process.communicate(timeout=NOTCURSES_TEST_TIMEOUT)
            except subprocess.TimeoutExpired:
                # The tester leads its own process group; end any helper it started too.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate()
                raise SystemExit(f"ci.py: notcurses {name} cases did not finish in "
                                 f"{NOTCURSES_TEST_TIMEOUT} seconds; output is in {terminal}")
        sys.stderr.write(errors.decode("utf-8", errors="replace"))
        text = report.read_text(encoding="utf-8", errors="replace") if report.is_file() else ""
        print(text, end="", flush=True)
        if process.returncode != 0:
            print(f"ci.py: notcurses {name} cases failed with status {process.returncode}; "
                  f"output is in {terminal}", file=sys.stderr, flush=True)
            raise SystemExit(process.returncode if process.returncode > 0
                             else 128 - process.returncode)
        check_notcurses_report(name, text)


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
    if config["notcurses_tests"]:
        run_notcurses_tests(paths)
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
    parser.add_argument("--cache-key", action="store_true",
                        help="print the digest of the notcurses stamp this configuration writes, "
                             "which CI's cache key includes, and exit without building")
    args = parser.parse_args()
    config = CONFIGURATIONS[args.configuration]
    if not sys.platform.startswith(config["host"]):
        if config["host"] == "linux":
            parser.error(f"{args.configuration} needs Linux; elsewhere use tools/run-ci-docker.py")
        parser.error(f"{args.configuration} needs macOS")
    if args.cache_key:
        if not config["renderer"]:
            parser.error(f"{args.configuration} builds no notcurses, so it has no cache key")
        print(notcurses_cache_key(config), flush=True)
        return 0

    root = args.build_root.resolve()
    work = root / args.configuration
    notcurses = root / "notcurses" / args.configuration
    paths = {"build": work / "build", "install": work / "install", "consumer": work / "consumer",
             "notcurses_source": notcurses / "src", "notcurses_build": notcurses / "build",
             "notcurses_install": notcurses / "install",
             "notcurses_tests": notcurses / "test-build"}
    show_versions(config)
    if args.phase in ("build", "all"):
        build(config, paths)
    if args.phase in ("test", "all"):
        test(config, paths)
    print(f"ci.py: {args.configuration} {args.phase} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
