#!/usr/bin/env python3
"""导出固定 Git HEAD，或为普通文件同步目录建立源码内容清单。"""

import argparse
import fnmatch
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile


MANIFEST = ".tilexr-source.json"
MARKER_TEXT = "TileXR validation deployment\n"
LOCAL_FILES = {MANIFEST, ".tilexr-validation-root", "validation.local.sh", "hostfile"}
# 普通文件同步没有 Git 的 tracked/ignored 信息，排除环境、参考仓库和生成物。
EXCLUDED_DIRS = {".git", ".worktrees", ".idea", ".vs", ".vscode", ".ci-build",
                 ".planning", ".agents", ".agnet", "__pycache__", ".pytest_cache",
                 ".venv", "venv", "env", "reference", "third_party", "node_modules",
                 "build", "install", "output", "run", "temp", "sign_tmp", "CMakeFiles", "Testing"}
EXCLUDED_FILES = ("*.pyc", "*.pyo", "*.o", "*.a", "*.so", "*.so.*", "*.log", "*.swp",
                  ".DS_Store", "CMakeCache.txt", "cmake_install.cmake", "install_manifest.txt")


class GitMetadataError(ValueError):
    pass


def _git(root, *args):
    # git -C 会向父目录寻找仓库；子模块缺少 .git 时不能查询父仓库的对象库。
    if not (Path(root) / ".git").exists():
        raise GitMetadataError("missing Git metadata: " + str(root))
    try:
        return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as error:
        raise GitMetadataError("unavailable Git metadata: " + str(root)) from error


def _path(root, name):
    """Reject traversal and symlinks before reading or writing manifest paths."""
    path = PurePosixPath(name)
    if (not name or "\\" in name or path.is_absolute() or
            any(p in ("", ".", "..", ".git") for p in name.split("/")) or
            any(":" in p for p in path.parts)):
        raise ValueError("unsafe source path: " + name)
    target = Path(root)
    for part in path.parts:
        target = target / part
        if target.is_symlink():
            raise ValueError("source symlink is unsupported: " + name)
    return target


def _tree(root, revision="HEAD", prefix=""):
    rows = _git(root, "ls-tree", "-rz", "--full-tree", revision).split(b"\0")
    for row in rows:
        if not row:
            continue
        metadata, raw_path = row.split(b"\t", 1)
        mode, kind, oid = metadata.decode("ascii").split()
        name = raw_path.decode("utf-8")
        relative = prefix + name
        _path(root, name)
        if relative in LOCAL_FILES:
            continue
        if kind == "commit":
            yield from _tree(Path(root) / name, oid, relative + "/")
        elif kind == "blob" and mode in ("100644", "100755"):
            yield root, oid, relative, int(mode[-3:], 8)
        else:
            raise ValueError("unsupported Git entry: " + relative)


def _digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def content_sha256(files):
    data = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _file_snapshot(root, exclude):
    excluded = LOCAL_FILES | set(exclude)
    for name in excluded:
        _path(root, name)
    files = {}

    def walk_error(error):
        raise error

    for directory, dirs, names in os.walk(root, onerror=walk_error):
        dirs[:] = sorted(name for name in dirs if name not in EXCLUDED_DIRS and
                         not name.startswith(("build-", "build_", "cmake-build-")))
        for name in dirs + sorted(names):
            relative = (Path(directory) / name).relative_to(root).as_posix()
            if (relative in excluded or name == ".git" or
                    any(fnmatch.fnmatchcase(name, pattern) for pattern in EXCLUDED_FILES)):
                continue
            path = _path(root, relative)
            if name not in dirs:
                if not path.is_file():
                    raise ValueError("source is not a regular file: " + relative)
                files[relative] = _digest(path)
    if not files:
        raise ValueError("源码目录为空，请同步完整源码及 3rdparty/spdlog 子模块内容")
    return {"commit": None, "provenance": "file-copy", "files": files,
            "content_sha256": content_sha256(files)}


def verify_source(root, manifest):
    """Raise ValueError when an expected source file is missing or changed."""
    if not isinstance(manifest, dict):
        raise ValueError("invalid source manifest")
    file_copy = manifest.get("provenance") == "file-copy"
    commit = manifest.get("commit")
    if ((file_copy and commit is not None) or
            (not file_copy and (not isinstance(commit, str) or
                               not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit))) or
            not isinstance(manifest.get("files"), dict) or not manifest["files"]):
        raise ValueError("invalid source manifest")
    for name, expected in manifest["files"].items():
        if not isinstance(name, str):
            raise ValueError("invalid source filename")
        target = _path(root, name)
        if (name in LOCAL_FILES or not isinstance(expected, str) or
                not re.fullmatch(r"[0-9a-f]{64}", expected) or
                not target.is_file() or _digest(target) != expected):
            raise ValueError("source mismatch: " + name)
    if file_copy and manifest.get("content_sha256") != content_sha256(manifest["files"]):
        raise ValueError("invalid source content digest")


def snapshot(root, exclude=()):
    root = Path(root).resolve()
    manifest_path = root / MANIFEST
    if manifest_path.exists() and not (root / ".git").exists():
        if manifest_path.is_symlink():
            raise ValueError("source manifest must not be a symlink")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("provenance") != "file-copy":
            verify_source(root, manifest)
            return manifest
        # 普通同步目录允许刷新源码后直接重建；sync/run 仍校验上次构建清单。
    if (root / ".git").exists():
        try:
            return {
                "commit": _git(root, "rev-parse", "HEAD").decode().strip(),
                "files": {name: _digest(_path(root, name)) for _, _, name, _ in _tree(root)},
                "dirty": bool(_git(root, "status", "--porcelain", "--untracked-files=no").strip()),
            }
        except GitMetadataError:
            print("Git 元数据不完整，按实际文件构建，commit=unknown。", file=sys.stderr)
    return _file_snapshot(root, exclude)


def pack(root, archive):
    """Export HEAD blobs, not modified working files or Windows CRLF checkouts."""
    root = Path(root).resolve()
    commit = _git(root, "rev-parse", "HEAD").decode().strip()
    subprocess.run(["git", "-C", str(root), "submodule", "update", "--init", "--recursive"], check=True)
    entries = list(_tree(root, commit))
    manifest = {"commit": commit, "files": {}}
    # One cat-file process per repository avoids spawning Git for every blob.
    repositories = {}
    for repo, oid, name, mode in entries:
        repositories.setdefault(str(repo), []).append((oid, name, mode))
    archive = Path(archive)
    try:
        with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT) as output:
            for repo, files in repositories.items():
                with subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE) as blobs:
                    try:
                        for oid, name, mode in files:
                            blobs.stdin.write((oid + "\n").encode("ascii"))
                            blobs.stdin.flush()
                            header = blobs.stdout.readline().split()
                            if len(header) != 3 or header[1] != b"blob":
                                raise ValueError("cannot read Git blob: " + name)
                            data = blobs.stdout.read(int(header[2]))
                            if len(data) != int(header[2]) or blobs.stdout.read(1) != b"\n":
                                raise ValueError("truncated Git blob: " + name)
                            manifest["files"][name] = hashlib.sha256(data).hexdigest()
                            item = tarfile.TarInfo(name)
                            item.size, item.mode = len(data), mode
                            output.addfile(item, io.BytesIO(data))
                    finally:
                        blobs.stdin.close()
                    if blobs.wait() != 0:
                        raise ValueError("Git blob export failed: " + repo)
            data = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
            item = tarfile.TarInfo(MANIFEST)
            item.size, item.mode = len(data), 0o644
            output.addfile(item, io.BytesIO(data))
    except BaseException:
        archive.unlink(missing_ok=True)
        raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("pack", "verify", "snapshot"))
    parser.add_argument("root", nargs="?", default=os.getcwd())
    parser.add_argument("archive", nargs="?")
    args = parser.parse_args()
    if args.action == "pack":
        if not args.archive:
            parser.error("pack requires ROOT ARCHIVE")
        manifest = pack(args.root, args.archive)
    elif args.action == "verify":
        manifest_path = _path(args.root, MANIFEST)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        verify_source(args.root, manifest)
    else:
        manifest = snapshot(args.root)
    print(json.dumps({"commit": manifest["commit"], "files": len(manifest["files"]),
                      "content_sha256": content_sha256(manifest["files"]),
                      "dirty": manifest.get("dirty")}))


if __name__ == "__main__":
    main()
