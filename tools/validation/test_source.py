"""Behavior tests for source inventories and safe committed exports; no SSH required."""

import os
import json
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from source import inventory, pack


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.repo("root")

    def git(self, root, *args):
        return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.PIPE)

    def repo(self, name):
        root = self.base / name
        root.mkdir()
        self.git(root, "init", "-q")
        self.git(root, "config", "user.name", "Validation Test")
        self.git(root, "config", "user.email", "validation@example.invalid")
        self.git(root, "config", "core.autocrlf", "false")
        (root / "file.txt").write_bytes(b"committed\n")
        self.commit(root)
        return root

    def commit(self, root):
        self.git(root, "add", ".")
        self.git(root, "commit", "-qm", "fixture")

    def unpack(self, archive):
        deployed = self.base / "deployed"
        deployed.mkdir()
        with tarfile.open(archive) as package:
            for member in package:
                target = deployed / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(package.extractfile(member).read())
        return deployed

    def test_export_uses_committed_lf_and_omits_local_files(self):
        (self.root / "file.txt").write_bytes(b"uncommitted\r\n")
        (self.root / "validation.local.sh").write_text("secret config\n")
        (self.root / "untracked.md").write_text("not part of HEAD\n")
        archive = self.base / "source.tar.gz"
        manifest = pack(self.root, archive)
        deployed = self.unpack(archive)
        self.assertEqual((deployed / "file.txt").read_bytes(), b"committed\n")
        self.assertFalse((deployed / "validation.local.sh").exists())
        self.assertFalse((deployed / "untracked.md").exists())
        self.assertFalse((deployed / ".git").exists())
        self.assertEqual(inventory(deployed)["files"], manifest["files"])
        self.assertEqual(inventory(deployed)["provenance"], "manifest-unverified")
        self.assertEqual(manifest["commit"], self.git(self.root, "rev-parse", "HEAD").decode().strip())
        self.assertEqual((self.root / "file.txt").read_bytes(), b"uncommitted\r\n")

    def test_live_inventory_labels_head_without_checking_worktree_content(self):
        manifest = inventory(self.root)
        self.assertEqual(manifest["provenance"], "git-head-unverified")
        (self.root / "file.txt").write_bytes(b"changed\n")
        self.assertEqual(inventory(self.root), manifest)
        (self.root / "untracked.cpp").write_text("new source\n")
        self.assertIn("untracked.cpp", inventory(self.root)["files"])

    def test_plain_copy_never_queries_git_even_below_another_repository(self):
        deployed = self.root / "plain-copy"
        deployed.mkdir()
        (deployed / "file.txt").write_text("copied source\n")
        with patch("source._git", side_effect=AssertionError("must not call Git")), \
                patch.object(Path, "open", side_effect=AssertionError("must not read source bytes")):
            manifest = inventory(deployed)
        self.assertIsNone(manifest["commit"])
        self.assertEqual(manifest["provenance"], "file-copy")
        self.assertEqual(set(manifest["files"]), {"file.txt"})

    def test_missing_submodule_git_metadata_uses_actual_copied_files(self):
        vendor = self.repo("vendor")
        (vendor / "file.txt").write_text("vendor-specific source\n")
        self.commit(vendor)
        with patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
            self.git(self.root, "submodule", "add", "-q", str(vendor), "vendor")
        self.commit(self.root)
        (self.root / "vendor/.git").unlink()
        manifest = inventory(self.root)
        self.assertEqual(manifest["commit"], self.git(self.root, "rev-parse", "HEAD").decode().strip())
        self.assertIn("vendor/file.txt", manifest["files"])

    def test_plain_rebuild_refreshes_paths_without_checking_source_changes(self):
        deployed = self.base / "plain"
        deployed.mkdir()
        source_file = deployed / "source.cpp"
        source_file.write_text("original\n")
        manifest = inventory(deployed)
        (deployed / ".tilexr-source.json").write_text(json.dumps(manifest))
        source_file.write_text("updated\n")
        self.assertEqual(inventory(deployed), manifest)
        source_file.unlink()
        (deployed / "replacement.cpp").write_text("replacement\n")
        current = inventory(deployed)
        self.assertEqual(current["files"], ["replacement.cpp"])
        result = subprocess.run([sys.executable,
                                 str(Path(__file__).with_name("source.py")), "inventory", str(deployed)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"commit": None, "files": 1, "provenance": "file-copy"})

    def test_plain_copy_omits_runtime_files_and_includes_submodule_sources(self):
        deployed = self.base / "plain"
        deployed.mkdir()
        sources = ("CMakeLists.txt", "src/comm/example.cpp", "tests/ep/check.py",
                   "3rdparty/spdlog/include/spdlog/spdlog.h", "build_all.sh")
        omitted = ("build/validation/cache", "install/validation/bin/test", "run/validation/lock",
                   "tests/ep/__pycache__/check.pyc", "src/comm/build/CMakeCache.txt",
                   "env/cann/header.h", "reference/upstream/source.cpp", "3rdparty/spdlog/.git",
                   "hostfile", "validation.local.sh", "cluster/hosts", ".tilexr-validation-root")
        for name in sources + omitted:
            target = deployed / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("fixture\n")
        manifest = inventory(deployed, exclude=("cluster/hosts",))
        self.assertEqual(set(manifest["files"]), set(sources))
        (deployed / "install/validation/bin/test").write_text("rebuilt\n")
        self.assertEqual(inventory(deployed, exclude=("cluster/hosts",)), manifest)

    def test_nested_reference_and_vendor_sources_are_included(self):
        deployed = self.base / "plain"
        name = "src/moonep/planner_v2/reference/ep_plan_reference.cpp"
        target = deployed / name
        target.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve().parents[2] / name, target)
        for relative in ("src/component/third_party/support.h", "tests/reference/check.py",
                         "reference/upstream/reference.cpp", "third_party/comparison.cpp"):
            path = deployed / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n")
        self.assertEqual(set(inventory(deployed)["files"]),
                         {name, "src/component/third_party/support.h", "tests/reference/check.py"})

    def test_editor_probe_does_not_change_source_digest_or_sync_paths(self):
        from types import SimpleNamespace
        from workflow import source_content_sha256, sync_paths

        deployed = self.base / "plain"
        tools = deployed / "tools/validation"
        tools.mkdir(parents=True)
        real_source = tools / "example.py"
        real_source.write_text("original source\n")
        # Do not broadly ignore *.tmp: a same-suffix input can be intentional.
        (tools / "input.tmp").write_text("source fixture\n")
        probes = [deployed / ".vscode_io_test.tmp", tools / ".vscode_io_test.tmp"]
        config = SimpleNamespace(root=deployed, hostfile=deployed / "hostfile",
                                 install=deployed / "install/validation")
        baseline = inventory(deployed)
        fingerprint = source_content_sha256(deployed, baseline)
        paths = sync_paths(config, baseline, None)
        self.assertIn("tools/validation/input.tmp", paths)
        for content in ("before OFF build\n", "editor rewrites between OFF and ON\n", None):
            for probe in probes:
                if content is None:
                    probe.unlink()
                else:
                    probe.write_text(content)
            current = inventory(deployed)
            with self.subTest(content=content):
                self.assertEqual(current, baseline)
                self.assertEqual(source_content_sha256(deployed, current), fingerprint)
                self.assertEqual(sync_paths(config, current, None), paths)
        real_source.write_text("changed source\n")
        self.assertNotEqual(source_content_sha256(deployed, inventory(deployed)), fingerprint)

    def test_old_or_malformed_optional_metadata_does_not_block_plain_copy(self):
        deployed = self.base / "plain"
        deployed.mkdir()
        (deployed / "file.txt").write_text("source\n")
        for text in ('{"commit": "' + "a" * 40 + '", "files": {"missing.cpp": "invalid digest"}}',
                     '{broken json', '["not a manifest"]', '{"commit": "invalid", "files": []}'):
            with self.subTest(text=text):
                (deployed / ".tilexr-source.json").write_text(text)
                manifest = inventory(deployed)
                self.assertIsNone(manifest["commit"])
                self.assertEqual(manifest["files"], ["file.txt"])

    def test_plain_copy_with_empty_optional_spdlog_needs_no_git_or_source_bytes(self):
        deployed = self.base / "plain"
        (deployed / "3rdparty/spdlog").mkdir(parents=True)
        (deployed / "CMakeLists.txt").write_text("project(fixture)\n")
        with patch("source._git", side_effect=AssertionError("must not call Git")), \
                patch.object(Path, "open", side_effect=AssertionError("must not read source bytes")):
            self.assertEqual(inventory(deployed), {"commit": None, "files": ["CMakeLists.txt"],
                                                  "provenance": "file-copy"})

    def test_unavailable_local_git_metadata_does_not_block_inventory(self):
        deployed = self.base / "plain"
        deployed.mkdir()
        (deployed / "file.txt").write_text("source\n")
        (deployed / ".git").write_text("gitdir: missing-repository\n")
        manifest = inventory(deployed)
        self.assertIsNone(manifest["commit"])
        self.assertEqual(manifest["files"], ["file.txt"])

    def test_export_initializes_and_pins_recursive_submodules(self):
        leaf = self.repo("leaf")
        middle = self.repo("middle")
        with patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
            self.git(middle, "submodule", "add", "-q", str(leaf), "nested")
            self.commit(middle)
            self.git(self.root, "submodule", "add", "-q", str(middle), "vendor")
            self.commit(self.root)
            # Nested checkout starts uninitialized; pack must populate it itself.
            self.assertFalse((self.root / "vendor/nested/file.txt").exists())
            archive = self.base / "source.tar.gz"
            manifest = pack(self.root, archive)
        deployed = self.unpack(archive)
        self.assertIn("vendor/nested/file.txt", manifest["files"])
        self.assertEqual((deployed / "vendor/nested/file.txt").read_bytes(), b"committed\n")
        self.assertFalse((deployed / "vendor/.git").exists())
        self.assertFalse((deployed / "vendor/nested/.git").exists())
        self.assertEqual(inventory(deployed)["files"], manifest["files"])

    def test_changed_export_refreshes_inventory_with_unverified_commit_label(self):
        archive = self.base / "source.tar.gz"
        manifest = pack(self.root, archive)
        deployed = self.unpack(archive)
        (deployed / "file.txt").write_bytes(b"tamper\n")
        current = inventory(deployed)
        self.assertEqual(current["commit"], manifest["commit"])
        self.assertEqual(current["provenance"], "manifest-unverified")
        (deployed / "file.txt").unlink()
        self.assertEqual(inventory(deployed)["files"], [])

    def test_inventory_rejects_path_escape(self):
        for name in ("../outside", "/absolute", "dir/../outside", "dir\\outside", "C:/outside", ".git/config"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsafe source path"):
                inventory(self.root, exclude=(name,))

    def test_inventory_rejects_symlink_components(self):
        link = self.root / "linked"
        external = self.base / "external"
        external.mkdir()
        try:
            link.symlink_to(external, target_is_directory=True)
        except OSError as error:
            self.skipTest("symlink unavailable: " + str(error))
        if not link.is_symlink():
            self.skipTest("runtime copied the directory instead of creating a symlink")
        with self.assertRaisesRegex(ValueError, "symlink"):
            inventory(self.root)

    @unittest.skipUnless(shutil.which("powershell.exe") and Path("/usr/bin/bash").exists(),
                         "Windows PowerShell + MSYS transport harness unavailable")
    def test_push_transport_capture_exercises_remote_refresh_and_rejection(self):
        scripts = self.root / "tools/validation"
        scripts.mkdir(parents=True)
        for name in ("source.py", "push.ps1"):
            shutil.copyfile(Path(__file__).with_name(name), scripts / name)
        self.commit(self.root)
        capture = self.base / "capture"
        capture.mkdir()
        deployed = self.base / "server's source path"
        native = lambda path: subprocess.check_output(["cygpath", "-w", str(path)]).decode().strip()
        quote = lambda value: "'" + value.replace("'", "''") + "'"
        harness = self.base / "capture.ps1"
        harness.write_text("""
$ErrorActionPreference = 'Stop'
$capture = CAPTURE
function scp {
    Copy-Item -LiteralPath $args[-2] -Destination (Join-Path $capture 'source.tar.gz')
    $global:LASTEXITCODE = 0
}
function ssh {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $capture 'script.sh'), (($input -join "`n") + "`n"), $utf8)
    [IO.File]::WriteAllText((Join-Path $capture 'command.txt'), $args[-1], $utf8)
    $global:LASTEXITCODE = 0
}
& PUSH -Server 'fixture.invalid' -RemotePath DESTINATION
""".replace("CAPTURE", quote(native(capture)))
           .replace("PUSH", quote(native(scripts / "push.ps1")))
           .replace("DESTINATION", quote(str(deployed))), encoding="utf-8")
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                                 "-ExecutionPolicy", "Bypass", "-File", native(harness)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        command = (capture / "command.txt").read_text()
        arguments = shlex.split(command)
        self.assertEqual(arguments[-1], str(deployed))
        remote_archive = Path(arguments[-2])
        self.addCleanup(lambda: remote_archive.unlink(missing_ok=True))
        script = (capture / "script.sh").read_bytes()

        def deploy():
            shutil.copyfile(capture / "source.tar.gz", remote_archive)
            return subprocess.run(["/usr/bin/bash", "-c", command], input=script,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=dict(os.environ, PATH="/usr/bin:" + os.environ["PATH"]))

        result = deploy()
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertTrue((deployed / ".tilexr-validation-root").is_file())
        (deployed / "validation.local.sh").write_text("retained config\n")
        (deployed / "hostfile").write_text("retained hostfile\n")
        # A previous deployment can carry the old digest dictionary. Only its
        # paths matter when removing stale files; its contents are not verified.
        old_manifest = json.loads((deployed / ".tilexr-source.json").read_text())
        old_manifest["files"] = {name: "ignored old digest" for name in old_manifest["files"]}
        (deployed / ".tilexr-source.json").write_text(json.dumps(old_manifest))
        (deployed / "file.txt").write_text("modified deployed source\n")
        (self.root / "file.txt").unlink()
        (self.root / "new.txt").write_text("new source\n")
        self.commit(self.root)
        pack(self.root, capture / "source.tar.gz")
        result = deploy()
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertFalse((deployed / "file.txt").exists())
        self.assertTrue((deployed / "new.txt").is_file())
        self.assertEqual((deployed / "validation.local.sh").read_text(), "retained config\n")
        self.assertEqual((deployed / "hostfile").read_text(), "retained hostfile\n")
        self.assertIsInstance(json.loads((deployed / ".tilexr-source.json").read_text())["files"], list)

        original_archive = (capture / "source.tar.gz").read_bytes()
        for name, reason in (("../outside", b"unsafe archive path"),
                             ("unexpected.txt", b"archive and source manifest disagree"),
                             ("new.txt", b"archive and source manifest disagree")):
            with self.subTest(archive_entry=name):
                malformed = self.base / "malformed.tar.gz"
                with tarfile.open(capture / "source.tar.gz") as source, tarfile.open(malformed, "w:gz") as output:
                    for member in source:
                        output.addfile(member, source.extractfile(member))
                    output.addfile(tarfile.TarInfo(name))
                shutil.copyfile(malformed, capture / "source.tar.gz")
                result = deploy()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stdout)
                self.assertFalse((self.base / "outside").exists())
                (capture / "source.tar.gz").write_bytes(original_archive)
        lock = deployed / "run/validation/node.lock"
        lock.mkdir(parents=True)
        result = deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"validation is active", result.stdout)
        lock.rmdir()
        (deployed / ".git").mkdir()
        result = deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"do not overwrite a Git checkout", result.stdout)
        (deployed / ".git").rmdir()
        (deployed / ".tilexr-validation-root").unlink()
        result = deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"destination must be empty or managed", result.stdout)
        self.assertEqual((deployed / "validation.local.sh").read_text(), "retained config\n")


if __name__ == "__main__":
    unittest.main()
