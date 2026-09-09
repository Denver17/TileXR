"""Behavior tests for committed exports and source integrity; no SSH required."""

import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from source import pack, snapshot, verify_source


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
        self.assertEqual(snapshot(deployed), manifest)
        self.assertEqual(manifest["commit"], self.git(self.root, "rev-parse", "HEAD").decode().strip())
        self.assertEqual((self.root / "file.txt").read_bytes(), b"uncommitted\r\n")

    def test_live_snapshot_records_actual_bytes_and_dirty_state(self):
        manifest = snapshot(self.root)
        self.assertFalse(manifest["dirty"])
        (self.root / "file.txt").write_bytes(b"changed\n")
        changed = snapshot(self.root)
        self.assertTrue(changed["dirty"])
        self.assertNotEqual(changed["files"], manifest["files"])
        verify_source(self.root, changed)
        with self.assertRaisesRegex(ValueError, "source mismatch"):
            verify_source(self.root, manifest)

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
        verify_source(deployed, manifest)

    def test_tampered_or_missing_source_is_rejected(self):
        archive = self.base / "source.tar.gz"
        manifest = pack(self.root, archive)
        deployed = self.unpack(archive)
        (deployed / "file.txt").write_bytes(b"tamper\n")
        with self.assertRaisesRegex(ValueError, "source mismatch"):
            snapshot(deployed)
        (deployed / "file.txt").unlink()
        with self.assertRaisesRegex(ValueError, "source mismatch"):
            verify_source(deployed, manifest)

    def test_manifest_rejects_path_escape(self):
        for name in ("../outside", "/absolute", "dir/../outside", "dir\\outside", "C:/outside", ".git/config"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsafe source path"):
                verify_source(self.root, {"commit": "a" * 40, "files": {name: "0" * 64}})

    def test_manifest_rejects_symlink_components(self):
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
            verify_source(self.root, {"commit": "a" * 40, "files": {"linked/file.txt": "0" * 64}})

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
