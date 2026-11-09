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
ALIAS = "lib/libnotcurses-core.so.3"
HEADER = "include/notcurses/notcurses.h"
DOCUMENT = "share/doc/notcurses/README.md"
PARTIAL = "lib/partial-install.o"


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
    and a document.

    Optionally the install names the library through a versioned file and a link to it, as
    real installations do (alias); installs the documents through a link to a directory,
    absolute or relative to the link's directory (linked_documents); or stops part way, as an
    interrupted build would, after writing one file (interrupt)."""

    def __init__(self):
        self.configured = {}
        self.alias = False
        self.linked_documents = None
        self.interrupt = False

    def __call__(self, command):
        if command[1] == "--fresh":
            build = command[command.index("-B") + 1]
            prefix = next((argument.partition("=")[2] for argument in command
                           if argument.startswith("-DCMAKE_INSTALL_PREFIX=")), None)
            self.configured[build] = (Path(command[command.index("-S") + 1]), prefix)
        elif command[1] == "--install":
            source, prefix = self.configured[command[2]]
            prefix = Path(prefix)
            for directory in ("lib", "include/notcurses", "share/doc"):
                (prefix / directory).mkdir(parents=True, exist_ok=True)
            if self.interrupt:
                (prefix / PARTIAL).write_bytes(b"partial")
                raise SystemExit("cmake --install was interrupted")
            documents = prefix / "share/doc/notcurses"
            if self.linked_documents:
                (documents.parent / self.linked_documents).mkdir(parents=True, exist_ok=True)
                if not documents.is_symlink():
                    documents.symlink_to(self.linked_documents, target_is_directory=True)
            else:
                documents.mkdir(exist_ok=True)
            library = b"\x7fELF stand-in built from: " + (source / "src" / "lib" /
                                                          "notcurses.c").read_bytes()
            if self.alias:
                (prefix / ALIAS).write_bytes(library)
                if (prefix / LIBRARY).exists() or (prefix / LIBRARY).is_symlink():
                    (prefix / LIBRARY).unlink()
                (prefix / LIBRARY).symlink_to(Path(ALIAS).name)
            else:
                (prefix / LIBRARY).write_bytes(library)
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
        """Build from a clean tree, apply change to the reused tree, and remove the
        installation to force a rebuild."""
        paths = self.paths()
        self.build(paths)
        change(paths["notcurses_source"])
        shutil.rmtree(paths["notcurses_install"])
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

    def rebuilt_without_stale_file(self, miss):
        """Leave a file no current install writes, apply miss to the stamp, and rebuild. The
        install must go into an emptied prefix, so neither it nor the new stamp keeps the file."""
        stale = "include/notcurses/obsolete.h"

        def change(install):
            (install / stale).write_bytes(b"stale")
            miss(install)

        paths = self.restored(change)
        self.assert_rebuilt(paths)
        self.assertEqual(sorted(self.stamp(paths)["files"]), [HEADER, LIBRARY, DOCUMENT])
        self.assertFalse((paths["notcurses_install"] / stale).exists())

    def test_stamp_for_another_configuration_rebuilds_into_an_empty_prefix(self):
        self.rebuilt_without_stale_file(lambda install: self.rewrite_stamp(
            install, lambda stamp: stamp.update(flags="-O3")))

    def test_stamp_without_digests_rebuilds_into_an_empty_prefix(self):
        self.rebuilt_without_stale_file(lambda install: self.rewrite_stamp(
            install, lambda stamp: stamp.pop("files")))

    def test_unreadable_stamp_rebuilds_into_an_empty_prefix(self):
        self.rebuilt_without_stale_file(
            lambda install: (install / "ci-stamp.json").write_bytes(b"{not json"))

    def test_empty_prefix_is_installed(self):
        paths = self.paths()
        paths["notcurses_install"].mkdir(parents=True)
        self.build(paths)
        self.assertEqual(sorted(self.stamp(paths)["files"]), [HEADER, LIBRARY, DOCUMENT])

    def test_prefix_without_a_stamp_is_refused_untouched(self):
        """ci.py empties only a prefix it made, which holds its stamp."""
        paths = self.restored(lambda install: (install / "ci-stamp.json").unlink())
        install = paths["notcurses_install"]
        before = sorted(path.relative_to(install) for path in install.rglob("*"))
        message = self.refused(paths)
        self.assertIn(str(install), message)
        self.assertIn("ci-stamp.json", message)
        self.assertEqual(self.ran("cmake"), [])
        self.assertEqual(sorted(path.relative_to(install) for path in install.rglob("*")), before)

    def test_prefix_holding_only_a_directory_link_is_refused(self):
        """A link in place before the first install would carry the library outside the
        prefix, where the stamp's digests never reach."""
        paths = self.paths()
        install = paths["notcurses_install"]
        install.mkdir(parents=True)
        external = self.root / "external-lib"
        external.mkdir()
        (external / "keep").write_bytes(b"keep")
        (install / "lib").symlink_to(external, target_is_directory=True)
        message = self.refused(paths)
        self.assertIn(str(install), message)
        self.assertEqual(self.ran("cmake"), [])
        self.assertTrue((install / "lib").is_symlink())
        self.assertEqual([path.name for path in external.iterdir()], ["keep"])

    def test_emptying_removes_links_without_following_them(self):
        external = self.root / "external-documents"
        external.mkdir()
        (external / "keep").write_bytes(b"keep")

        def change(install):
            (install / "share/extra").symlink_to(external, target_is_directory=True)
            self.rewrite_stamp(install, lambda stamp: stamp.update(flags="-O3"))

        paths = self.restored(change)
        self.assert_rebuilt(paths)
        self.assertFalse(os.path.lexists(paths["notcurses_install"] / "share/extra"))
        self.assertEqual([path.name for path in external.iterdir()], ["keep"])

    def test_interrupted_install_is_emptied_and_rebuilt(self):
        """The stamp, without digests, marks the prefix as ci.py's before the install starts,
        so an install that stops part way is emptied and rebuilt on the next run."""
        paths = self.paths()
        install = paths["notcurses_install"]
        self.cmake.interrupt = True
        with self.assertRaises(SystemExit):
            self.build(paths)
        self.assertTrue((install / PARTIAL).is_file())
        self.assertNotIn("files", self.stamp(paths))
        self.cmake.interrupt = False
        self.build(paths)
        self.assertEqual(sorted(self.stamp(paths)["files"]), [HEADER, LIBRARY, DOCUMENT])
        self.assertFalse((install / PARTIAL).exists())

    def linked_prefix(self, change):
        """A prefix that is a link to a stamped installation elsewhere, after change."""
        elsewhere = self.paths("elsewhere")
        self.build(elsewhere)
        change(elsewhere["notcurses_install"])
        paths = self.paths()
        paths["notcurses_install"].parent.mkdir(parents=True)
        paths["notcurses_install"].symlink_to(elsewhere["notcurses_install"],
                                              target_is_directory=True)
        return paths, elsewhere["notcurses_install"]

    def assert_linked_prefix_refused(self, paths, target):
        before = {path.relative_to(target): path.read_bytes() for path in target.rglob("*")
                  if path.is_file()}
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_install"]), message)
        self.assertIn("link", message)
        self.assertEqual(self.ran("cmake"), [])
        after = {path.relative_to(target): path.read_bytes() for path in target.rglob("*")
                 if path.is_file()}
        self.assertEqual(after, before)

    def test_linked_prefix_with_a_matching_stamp_is_refused(self):
        self.assert_linked_prefix_refused(*self.linked_prefix(lambda install: None))

    def test_linked_prefix_with_another_configuration_is_refused(self):
        self.assert_linked_prefix_refused(*self.linked_prefix(lambda install: self.rewrite_stamp(
            install, lambda stamp: stamp.update(flags="-O3"))))

    def test_library_link_is_recorded_and_reused(self):
        """Installations name the library through a link to its versioned file beside it. The
        link records its target, the file is hashed under its own path, and a restored
        installation with them is reused."""
        self.cmake.alias = True
        paths = self.restored()
        files = self.stamp(paths)["files"]
        self.assertEqual(files[LIBRARY], "link:" + Path(ALIAS).name)
        self.assertEqual(files[ALIAS], "sha256:" + hashlib.sha256(
            (paths["notcurses_install"] / ALIAS).read_bytes()).hexdigest())
        output = self.build(paths)
        self.assertIn("already installed", output)
        self.assertEqual(self.commands, [])

    def stamped_with_link(self, name, link, target):
        """A built and stamped installation with a link at link to target, replacing any file
        there, and the stamp rewritten to record the link's target as ci.py records links."""
        paths = self.paths(name)
        self.build(paths)
        path = paths["notcurses_install"] / link
        if path.exists() or path.is_symlink():
            path.unlink()
        path.symlink_to(target)
        self.rewrite_stamp(paths["notcurses_install"],
                           lambda stamp: stamp["files"].update({link: "link:" + str(target)}))
        return paths

    def assert_link_refused(self, paths, link):
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_install"] / link), message)
        self.assertIn("outside", message)
        self.assertEqual(self.commands, [])
        self.assertTrue((paths["notcurses_install"] / link).is_symlink())

    def external_library(self):
        external = self.root / "external-lib" / Path(LIBRARY).name
        external.parent.mkdir()
        external.write_bytes(b"\x7fELF stand-in outside the prefix")
        return external

    def test_absolute_link_leaving_the_prefix_is_refused(self):
        """A recorded link's text matches whatever its target outside the prefix holds."""
        external = self.external_library()
        paths = self.stamped_with_link("build-root", LIBRARY, external)
        self.assert_link_refused(paths, LIBRARY)
        self.assertEqual(external.read_bytes(), b"\x7fELF stand-in outside the prefix")

    def test_relative_link_climbing_out_of_the_prefix_is_refused(self):
        external = self.external_library()
        install = self.paths()["notcurses_install"]
        target = os.path.relpath(external, (install / LIBRARY).parent)
        self.assertTrue(target.startswith(".." + os.sep))
        paths = self.stamped_with_link("build-root", LIBRARY, target)
        self.assert_link_refused(paths, LIBRARY)
        self.assertEqual(external.read_bytes(), b"\x7fELF stand-in outside the prefix")

    def test_dangling_link_leaving_the_prefix_is_refused(self):
        paths = self.stamped_with_link("build-root", "lib/libnotcurses-extra.so",
                                       "../../libnotcurses-extra.so")
        self.assert_link_refused(paths, "lib/libnotcurses-extra.so")

    def test_windows_style_link_targets_are_refused(self):
        """On Linux and macOS these name files inside the prefix, but Windows would read the
        backslashes as separators climbing out, and the drive as another root."""
        targets = {"backslashes": "..\\..\\..\\..\\..\\external-lib\\libnotcurses-core.so",
                   "drive": "C:/external-lib/libnotcurses-core.so"}
        for name, target in targets.items():
            with self.subTest(name):
                paths = self.stamped_with_link(name, "lib/libnotcurses-extra.so", target)
                self.assert_link_refused(paths, "lib/libnotcurses-extra.so")

    def test_links_resolving_inside_the_prefix_are_recorded_and_reused(self):
        """A link that climbs with ".." but stays inside, and a dangling link inside."""
        links = {"climbing": ("include/notcurses/library", "../../lib/libnotcurses-core.so"),
                 "dangling": ("lib/libnotcurses-old.so", "libnotcurses-gone.so")}
        for name, (link, target) in links.items():
            with self.subTest(name):
                paths = self.stamped_with_link(name, link, target)
                self.assertEqual(self.stamp(paths)["files"][link], "link:" + target)
                output = self.build(paths)
                self.assertIn("already installed", output)
                self.assertEqual(self.commands, [])

    def test_directory_link_leaving_a_stamped_installation_is_refused(self):
        """A stamp that recorded only a directory link's target covered none of the files
        behind it, so a changed library there would be accepted."""
        external = self.root / "external-lib"

        def link_library_directory(install):
            shutil.move(install / "lib", external)
            (install / "lib").symlink_to(external, target_is_directory=True)
            flip_byte(external / Path(LIBRARY).name, 7)
            self.rewrite_stamp(install, lambda stamp: stamp.update(files={
                HEADER: stamp["files"][HEADER], DOCUMENT: stamp["files"][DOCUMENT],
                "lib": "link:" + str(external)}))

        paths = self.restored(link_library_directory)
        library = (external / Path(LIBRARY).name).read_bytes()
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_install"] / "lib"), message)
        self.assertIn("outside", message)
        self.assertEqual(self.commands, [])
        self.assertTrue((paths["notcurses_install"] / "lib").is_symlink())
        self.assertEqual((external / Path(LIBRARY).name).read_bytes(), library)

    def test_directory_link_inside_a_stamped_installation_is_refused(self):
        """Links to directories are refused even inside the prefix: ci.py never follows one."""
        def link_library_directory(install):
            (install / "lib").rename(install / "lib-files")
            (install / "lib").symlink_to("lib-files", target_is_directory=True)
            self.rewrite_stamp(install, lambda stamp: stamp.update(files={
                HEADER: stamp["files"][HEADER], DOCUMENT: stamp["files"][DOCUMENT],
                "lib": "link:lib-files", "lib-files/libnotcurses-core.so": stamp["files"][LIBRARY]}))

        paths = self.restored(link_library_directory)
        message = self.refused(paths)
        self.assertIn(str(paths["notcurses_install"] / "lib"), message)
        self.assertIn("link to a directory", message)
        self.assertEqual(self.commands, [])

    def test_install_with_a_directory_link_is_refused(self):
        """An install that writes through a link to a directory, here one inside the prefix,
        is never stamped with digests, and is refused again after its prefix is emptied."""
        self.cmake.linked_documents = Path("../../notcurses-documents")
        paths = self.paths()
        link = paths["notcurses_install"] / "share/doc/notcurses"
        for attempt in range(2):
            message = self.refused(paths)
            self.assertIn(str(link), message)
            self.assertIn("link to a directory", message)
            self.assertNotIn("files", self.stamp(paths))
        self.assertTrue((paths["notcurses_install"] / "notcurses-documents/README.md").is_file())

    def test_install_with_a_link_leaving_the_prefix_is_refused(self):
        """An install that writes through a link out of the prefix is never stamped with
        digests, and emptying its prefix afterwards removes the link but not what it points
        to."""
        documents = self.root / "external-documents"
        documents.mkdir()
        (documents / "keep").write_bytes(b"keep")
        self.cmake.linked_documents = documents
        paths = self.paths()
        link = paths["notcurses_install"] / "share/doc/notcurses"
        for attempt in range(2):
            message = self.refused(paths)
            self.assertIn(str(link), message)
            self.assertIn("outside", message)
            self.assertNotIn("files", self.stamp(paths))
        self.assertTrue((documents / "keep").is_file())


class CacheKeyTest(CiTestCase):
    """CI keys its cached notcurses installation by a digest of the stamp a build writes,
    compiler identity included, so a key hit never restores an installation whose stamp
    then misses."""

    def config(self):
        return dict(self.ci.CONFIGURATIONS["linux-shared"], cc=sys.executable)

    def test_key_is_the_digest_of_the_stamp_a_build_writes(self):
        paths = self.paths()
        self.build(paths)
        stamp = self.stamp(paths)
        stamp.pop("files")
        self.assertEqual(stamp, self.ci.notcurses_stamp(self.config()))
        digest = hashlib.sha256(json.dumps(stamp, sort_keys=True).encode("utf-8")).hexdigest()
        self.assertEqual(self.ci.notcurses_cache_key(self.config()), digest)

    def test_new_compiler_changes_the_key_and_the_stamp(self):
        paths = self.paths()
        self.build(paths)
        key = self.ci.notcurses_cache_key(self.config())
        real_capture = self.ci.capture

        def capture(*args):
            output = real_capture(*args)
            if args[1:] == ("--version",):
                return "Compiler 99.0 from a new runner image\n" + output
            return output

        self.ci.capture = capture
        self.assertNotEqual(self.ci.notcurses_cache_key(self.config()), key)
        self.build(paths)
        self.assertEqual(len(self.ran("cmake", "--install")), 1)
        self.assertEqual(self.stamp(paths)["compiler"], "Compiler 99.0 from a new runner image")

    def main(self, *arguments, platform="linux"):
        """Run ci.py's command line; returns its exit status, output and error output."""
        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["ci.py", *arguments]), \
                mock.patch.object(sys, "platform", platform), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            try:
                status = self.ci.main()
            except SystemExit as stopped:
                status = stopped.code
        return status, output.getvalue(), errors.getvalue()

    def test_command_line_prints_only_the_key(self):
        self.ci.CONFIGURATIONS["linux-shared"]["cc"] = sys.executable
        status, output, _ = self.main("linux-shared", "--cache-key")
        self.assertEqual(status, 0)
        self.assertEqual(output, self.ci.notcurses_cache_key(self.config()) + "\n")
        self.assertEqual(self.commands, [])

    def test_configuration_without_notcurses_has_no_key(self):
        status, output, errors = self.main("macos-asan", "--cache-key", platform="darwin")
        self.assertEqual(status, 2)
        self.assertEqual(output, "")
        self.assertIn("builds no notcurses", errors)

    def test_workflow_keys_the_cache_by_the_printed_digest(self):
        workflow = (TOOLS.parent / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8")
        self.assertIn("python3 tools/ci.py linux-${{ matrix.configuration }} --cache-key",
                      workflow)
        keys = [line.strip() for line in workflow.splitlines()
                if line.strip().startswith("key: notcurses-")]
        self.assertEqual(len(keys), 1)
        self.assertTrue(keys[0].endswith("-${{ steps.notcurses.outputs.key }}"), keys[0])


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
