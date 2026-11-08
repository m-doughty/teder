"""Test tools/ci.py's notcurses source tree, installation and doctest report checks.

Nothing is compiled. Each test makes a throwaway one-commit repository standing in for the
notcurses fork, points ci.py at it by path and commit hash, and replaces CMake with a recorder
whose install step copies the checked-out sources into a fake library and headers. Git runs for
real, with the user's and the system's Git configuration ignored. Requires Python 3 and Git:

    python3 tools/test-ci.py
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parent
# Applies to every Git process a test starts, ci.py's included.
GIT_ENVIRONMENT = {
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_AUTHOR_NAME": "ci.py test", "GIT_AUTHOR_EMAIL": "ci-test@localhost",
    "GIT_COMMITTER_NAME": "ci.py test", "GIT_COMMITTER_EMAIL": "ci-test@localhost",
    "GIT_CONFIG_COUNT": "2",
    "GIT_CONFIG_KEY_0": "init.defaultBranch", "GIT_CONFIG_VALUE_0": "main",
    "GIT_CONFIG_KEY_1": "commit.gpgSign", "GIT_CONFIG_VALUE_1": "false",
}
# The stand-in fork. Like the real one, its .gitignore matches build*, and CMake would glob
# src/lib/*.c.
ORIGIN_FILES = {
    ".gitignore": b"build*\n",
    "src/lib/notcurses.c": b"int notcurses_stand_in(void) { return 1; }\n",
    "include/notcurses/notcurses.h": b"int notcurses_stand_in(void);\n",
    "README.md": b"stand-in\n",
}
LIBRARY = "lib/libnotcurses-core.so"
HEADER = "include/notcurses/notcurses.h"


def load_ci():
    """A fresh copy of tools/ci.py, so a test's replacements never reach another test."""
    spec = importlib.util.spec_from_file_location("ci", TOOLS / "ci.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def git(*args):
    result = subprocess.run(["git", *[str(arg) for arg in args]], capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(str(arg) for arg in args)} failed:\n{result.stderr}")
    return result.stdout.strip()


def flip_byte(path, offset=0):
    data = bytearray(path.read_bytes())
    data[offset] ^= 0x01
    path.write_bytes(bytes(data))


class FakeCMake:
    """Records CMake commands. Installing writes a library made from the configured source's
    src/lib/notcurses.c, so a modified tree would yield a different library, plus its headers
    and a document."""

    def __init__(self):
        self.configured = {}

    def __call__(self, command):
        if command[1] == "--fresh":
            build = command[command.index("-B") + 1]
            prefix = next((argument.partition("=")[2] for argument in command
                           if argument.startswith("-DCMAKE_INSTALL_PREFIX=")), None)
            self.configured[build] = (Path(command[command.index("-S") + 1]), prefix)
        elif command[1] == "--install":
            source, prefix = self.configured[command[2]]
            prefix = Path(prefix)
            for directory in ("lib", "include/notcurses", "share/doc/notcurses"):
                (prefix / directory).mkdir(parents=True, exist_ok=True)
            (prefix / LIBRARY).write_bytes(b"\x7fELF stand-in built from: " +
                                           (source / "src" / "lib" / "notcurses.c").read_bytes())
            shutil.copyfile(source / HEADER, prefix / HEADER)
            shutil.copyfile(source / "README.md", prefix / "share/doc/notcurses/README.md")


class CiTestCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        empty = self.root / "empty-gitconfig"
        empty.write_bytes(b"")
        environment = mock.patch.dict(os.environ, dict(GIT_ENVIRONMENT,
                                                       GIT_CONFIG_GLOBAL=str(empty)))
        environment.start()
        self.addCleanup(environment.stop)

        self.origin = self.root / "origin"
        git("init", "--quiet", self.origin)
        for name, content in ORIGIN_FILES.items():
            (self.origin / name).parent.mkdir(parents=True, exist_ok=True)
            (self.origin / name).write_bytes(content)
        git("-C", self.origin, "add", "--all")
        git("-C", self.origin, "commit", "--quiet", "--message", "stand-in")
        self.revision = git("-C", self.origin, "rev-parse", "HEAD")

        self.ci = load_ci()
        self.ci.NOTCURSES_URL = str(self.origin)
        self.ci.NOTCURSES_REVISION = self.revision
        self.commands = []
        self.cmake = FakeCMake()
        real_run = self.ci.run

        def run(*args):
            command = [str(arg) for arg in args]
            self.commands.append(command)
            if command[0] == "cmake":
                self.cmake(command)
            else:
                real_run(*args)

        self.ci.run = run

    def paths(self, name="build-root"):
        notcurses = self.root / name / "notcurses" / "linux-shared"
        return {"notcurses_source": notcurses / "src", "notcurses_build": notcurses / "build",
                "notcurses_install": notcurses / "install",
                "notcurses_tests": notcurses / "test-build"}

    def build(self, paths, notcurses_tests=False):
        """Run ci.py's notcurses build step; returns its standard output."""
        config = dict(self.ci.CONFIGURATIONS["linux-shared"], cc=sys.executable,
                      notcurses_tests=notcurses_tests)
        self.commands.clear()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.ci.build_notcurses(config, paths)
        return output.getvalue()

    def refused(self, paths, notcurses_tests=False):
        """Run the build step, which must stop; returns its message."""
        with self.assertRaises(SystemExit) as stopped:
            self.build(paths, notcurses_tests)
        self.assertIsInstance(stopped.exception.code, str)
        return stopped.exception.code

    def ran(self, program, *arguments):
        return [command for command in self.commands
                if command[0] == program and all(argument in command for argument in arguments)]

    def stamp(self, paths):
        stamp_path = paths["notcurses_install"] / "ci-stamp.json"
        return json.loads(stamp_path.read_text(encoding="utf-8"))


class SourceTreeTest(CiTestCase):
    """A reused source tree must hold the pinned commit unmodified, and is never cleaned."""

    def built_then_modified(self, change):
        """Build from a clean tree, apply change to the reused tree, and force a rebuild."""
        paths = self.paths()
        self.build(paths)
        change(paths["notcurses_source"])
        (paths["notcurses_install"] / "ci-stamp.json").unlink()
        return paths

    def assert_refused(self, paths, *names):
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_source"]), message)
        self.assertIn(self.revision, message)
        for name in names:
            self.assertIn(name, message)
        self.assertEqual(self.ran("cmake"), [])
        self.assertFalse((paths["notcurses_install"] / "ci-stamp.json").exists())

    def test_clean_tree_is_built_and_stamped_as_the_pin(self):
        paths = self.paths()
        self.build(paths)
        source = paths["notcurses_source"]
        self.assertEqual(git("-C", source, "rev-parse", "HEAD"), self.revision)
        self.assertEqual(len(self.ran("cmake", "--fresh", str(source))), 1)
        self.assertEqual(self.stamp(paths)["revision"], self.revision)

    def test_reused_clean_tree_is_rebuilt(self):
        paths = self.built_then_modified(lambda source: None)
        self.build(paths)
        self.assertEqual(len(self.ran("cmake", "--install")), 1)
        self.assertEqual(self.stamp(paths)["revision"], self.revision)

    def test_edited_and_untracked_files_are_refused(self):
        def change(source):
            with (source / "src/lib/notcurses.c").open("ab") as edited:
                edited.write(b"int local_edit;\n")
            (source / "src/lib/extra.c").write_bytes(b"int extra;\n")

        paths = self.built_then_modified(change)
        self.assert_refused(paths, "src/lib/notcurses.c", "src/lib/extra.c")
        # Nothing was cleaned: a local edit may be deliberate.
        source = paths["notcurses_source"]
        edited = (source / "src/lib/notcurses.c").read_bytes()
        self.assertTrue(edited.endswith(b"int local_edit;\n"))
        self.assertTrue((source / "src/lib/extra.c").is_file())

    def test_tracked_edit_is_refused(self):
        paths = self.built_then_modified(
            lambda source: flip_byte(source / "include/notcurses/notcurses.h"))
        self.assert_refused(paths, "include/notcurses/notcurses.h")

    def test_deleted_file_is_refused(self):
        paths = self.built_then_modified(lambda source: (source / "README.md").unlink())
        self.assert_refused(paths, "README.md")

    def test_untracked_file_is_refused(self):
        paths = self.built_then_modified(
            lambda source: (source / "src/lib/extra.c").write_bytes(b"int extra;\n"))
        self.assert_refused(paths, "src/lib/extra.c")

    def test_ignored_file_is_refused(self):
        def change(source):
            (source / "src/lib/build_extra.c").write_bytes(b"int extra;\n")
            self.assertEqual(git("-C", source, "check-ignore", "src/lib/build_extra.c"),
                             "src/lib/build_extra.c")

        paths = self.built_then_modified(change)
        self.assert_refused(paths, "src/lib/build_extra.c")

    def test_skip_worktree_edit_is_refused(self):
        def change(source):
            git("-C", source, "update-index", "--skip-worktree", "src/lib/notcurses.c")
            flip_byte(source / "src/lib/notcurses.c")
            self.assertEqual(git("-C", source, "status", "--porcelain"), "")

        paths = self.built_then_modified(change)
        self.assert_refused(paths, "skip-worktree", "src/lib/notcurses.c")

    def test_assume_unchanged_edit_is_refused(self):
        def change(source):
            git("-C", source, "update-index", "--assume-unchanged", "src/lib/notcurses.c")
            flip_byte(source / "src/lib/notcurses.c")
            self.assertEqual(git("-C", source, "status", "--porcelain"), "")

        paths = self.built_then_modified(change)
        self.assert_refused(paths, "assume-unchanged", "src/lib/notcurses.c")

    def test_test_build_uses_the_verified_checkout(self):
        paths = self.paths()
        self.build(paths, notcurses_tests=True)
        source, tests = str(paths["notcurses_source"]), str(paths["notcurses_tests"])
        self.assertEqual(len(self.ran("git", "fetch")), 1)
        self.assertEqual(len(self.ran("cmake", "--fresh", source, tests, "-DUSE_DOCTEST=ON")), 1)
        self.assertEqual(len(self.ran("cmake", "--build", tests, "--clean-first",
                                      "notcurses-tester")), 1)
        # A cached installation is not rebuilt, but the test build checks out the pin again.
        self.build(paths, notcurses_tests=True)
        self.assertEqual(self.ran("cmake", "--install"), [])
        self.assertEqual(len(self.ran("git", "fetch")), 1)
        self.assertEqual(len(self.ran("cmake", "--build", tests, "--clean-first")), 1)

    def test_test_build_refuses_a_modified_tree_with_a_cached_installation(self):
        paths = self.paths()
        self.build(paths, notcurses_tests=True)
        flip_byte(paths["notcurses_source"] / "src/lib/notcurses.c")
        message = self.refused(paths, notcurses_tests=True)
        self.assertIn(str(paths["notcurses_source"]), message)
        self.assertIn("src/lib/notcurses.c", message)
        self.assertEqual(self.ran("cmake"), [])
        self.assertTrue((paths["notcurses_install"] / "ci-stamp.json").is_file())


class InstallDigestTest(CiTestCase):
    """A cached installation is reused only if every file matches its recorded digest."""

    def restored(self, change=None):
        """Build and stamp notcurses, then copy only its installation into a new build root,
        as CI's cache restores it, and apply change to the copy."""
        built = self.paths()
        self.build(built)
        paths = self.paths("restored-root")
        shutil.copytree(built["notcurses_install"], paths["notcurses_install"], symlinks=True)
        if change:
            change(paths["notcurses_install"])
        return paths

    def assert_rejected(self, paths, *lines):
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_install"]), message)
        for line in lines:
            self.assertIn(line, message)
        # Neither repaired nor rebuilt.
        self.assertEqual(self.commands, [])
        self.assertTrue((paths["notcurses_install"] / "ci-stamp.json").is_file())
        self.assertFalse(paths["notcurses_source"].exists())

    def test_stamp_records_every_installed_file(self):
        paths = self.paths()
        self.build(paths)
        install = paths["notcurses_install"]
        files = self.stamp(paths)["files"]
        self.assertEqual(sorted(files), [HEADER, LIBRARY, "share/doc/notcurses/README.md"])
        for name, digest in files.items():
            self.assertEqual(digest, "sha256:" + hashlib.sha256((install / name).read_bytes())
                             .hexdigest())

    def test_untouched_restored_installation_is_accepted(self):
        paths = self.restored()
        output = self.build(paths)
        self.assertIn("already installed", output)
        self.assertEqual(self.commands, [])
        self.assertFalse(paths["notcurses_source"].exists())

    def test_one_changed_byte_in_the_library_is_rejected(self):
        paths = self.restored(lambda install: flip_byte(install / LIBRARY, 7))
        self.assert_rejected(paths, f"changed: {LIBRARY}")

    def test_one_changed_byte_in_a_header_is_rejected(self):
        paths = self.restored(lambda install: flip_byte(install / HEADER))
        self.assert_rejected(paths, f"changed: {HEADER}")

    def test_added_file_is_rejected(self):
        paths = self.restored(
            lambda install: (install / "lib/libnotcurses-extra.so").write_bytes(b"extra"))
        self.assert_rejected(paths, "added: lib/libnotcurses-extra.so")

    def test_missing_file_is_rejected(self):
        paths = self.restored(
            lambda install: (install / "share/doc/notcurses/README.md").unlink())
        self.assert_rejected(paths, "missing: share/doc/notcurses/README.md")

    def rewrite_stamp(self, install, change):
        stamp_path = install / "ci-stamp.json"
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
        change(stamp)
        stamp_path.write_text(json.dumps(stamp), encoding="utf-8")

    def assert_rebuilt(self, paths):
        self.build(paths)
        self.assertEqual(len(self.ran("cmake", "--install")), 1)
        self.assertEqual(self.stamp(paths)["revision"], self.revision)
        self.assertIn(LIBRARY, self.stamp(paths)["files"])

    def test_stamp_for_another_configuration_is_rebuilt(self):
        paths = self.restored(lambda install: self.rewrite_stamp(
            install, lambda stamp: stamp.update(flags="-O3")))
        self.assert_rebuilt(paths)

    def test_stamp_without_digests_is_rebuilt(self):
        paths = self.restored(lambda install: self.rewrite_stamp(
            install, lambda stamp: stamp.pop("files")))
        self.assert_rebuilt(paths)

    def test_unreadable_stamp_is_rebuilt(self):
        paths = self.restored(
            lambda install: (install / "ci-stamp.json").write_bytes(b"{not json"))
        self.assert_rebuilt(paths)


class DoctestReportTest(unittest.TestCase):
    """notcurses-tester's exit status is not enough: it exits successfully when its filter
    selects nothing or TERM is unset, so the report must show cases that ran and passed."""

    def setUp(self):
        self.ci = load_ci()

    def report(self, total, passed, failed, assertions=718):
        """A report as doctest 2.4 writes it."""
        return ("[doctest] doctest version is \"2.4.11\"\n"
                "[doctest] run with \"--help\" for options\n"
                "===============================================================================\n"
                f"[doctest] test cases: {total:3} | {passed:3} passed | {failed} failed | "
                "70 skipped\n"
                f"[doctest] assertions: {assertions:3} | {assertions:3} passed | 0 failed |\n"
                "[doctest] Status: SUCCESS!\n")

    def test_passing_cases_are_accepted(self):
        self.ci.check_notcurses_report("resize", self.report(2, 2, 0))

    def test_no_selected_case_is_rejected(self):
        with self.assertRaises(SystemExit) as stopped:
            self.ci.check_notcurses_report("resize", self.report(0, 0, 0))
        self.assertIn("src/tests/resize.cpp", stopped.exception.code)

    def test_failed_case_is_rejected(self):
        with self.assertRaises(SystemExit) as stopped:
            self.ci.check_notcurses_report("autogrow", self.report(2, 1, 1))
        self.assertIn("1 of 2", stopped.exception.code)

    def test_cases_without_assertions_are_rejected(self):
        with self.assertRaises(SystemExit) as stopped:
            self.ci.check_notcurses_report("resize", self.report(1, 1, 0, assertions=0))
        self.assertIn("checked nothing", stopped.exception.code)

    def test_missing_summary_is_rejected(self):
        for report in ("", "TERM wasn't defined, exiting with success\n"):
            with self.assertRaises(SystemExit):
                self.ci.check_notcurses_report("resize", report)


if __name__ == "__main__":
    # Loading tools/ci.py would otherwise write bytecode into the source tree.
    sys.dont_write_bytecode = True
    unittest.main(verbosity=2)
