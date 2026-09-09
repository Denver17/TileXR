#!/usr/bin/env python3
"""导出固定 Git HEAD，或列出普通文件同步目录的源码路径；不校验源码内容。"""

import argparse
import fnmatch
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile


MANIFEST = ".tilexr-source.json"
MARKER_TEXT = "TileXR validation deployment\n"
LOCAL_FILES = {MANIFEST, ".tilexr-validation-root", "validation.local.sh", "hostfile"}
# 普通文件同步没有 Git 的 tracked/ignored 信息，排除环境、参考仓库和生成物。
EXCLUDED_DIRS = {".git", ".worktrees", ".idea", ".vs", ".vscode", ".ci-build",
                 ".planning", ".agents", ".agnet", "__pycache__", ".pytest_cache",
                 ".venv", "venv", "env", "node_modules",
                 "build", "install", "output", "run", "temp", "sign_tmp", "CMakeFiles", "Testing"}
# 仓库根目录的比较代码不分发；嵌套 reference/third_party 可能是实际编译输入。
ROOT_EXCLUDED_DIRS = {"reference", "third_party"}
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
    if not isinstance(name, str):
        raise ValueError("invalid source filename")
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


def _files(root, exclude):
    excluded = LOCAL_FILES | set(exclude)
    for name in excluded:
        _path(root, name)
    files = []

    def walk_error(error):
        raise error

    for directory, dirs, names in os.walk(root, onerror=walk_error):
        dirs[:] = sorted(name for name in dirs if name not in EXCLUDED_DIRS and
                         (Path(directory) != root or name not in ROOT_EXCLUDED_DIRS) and
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
                files.append(relative)
    return sorted(files)


def _commit(root):
    """Git HEAD and copied metadata are labels, never proof of current file contents."""
    if (root / ".git").exists():
        try:
            return _git(root, "rev-parse", "HEAD").decode().strip(), "git-head-unverified"
        except GitMetadataError:
            pass
    manifest_path = _path(root, MANIFEST)
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            commit = (manifest.get("commit") if isinstance(manifest, dict) and
                      isinstance(manifest.get("files"), list) and
                      manifest.get("provenance") in ("git-head-unverified", "manifest-unverified")
                      else None)
            if isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
                return commit, "manifest-unverified"
        except (OSError, UnicodeError, ValueError):
            pass
    return None, "file-copy"


def inventory(root, exclude=()):
    """Refresh paths for copying without reading or comparing source file contents."""
    root = Path(root).resolve()
    commit, provenance = _commit(root)
    return {"commit": commit, "files": _files(root, exclude), "provenance": provenance}


def pack(root, archive):
    """Export HEAD blobs, not modified working files or Windows CRLF checkouts."""
    root = Path(root).resolve()
    commit = _git(root, "rev-parse", "HEAD").decode().strip()
    subprocess.run(["git", "-C", str(root), "submodule", "update", "--init", "--recursive"], check=True)
    entries = list(_tree(root, commit))
    manifest = {"commit": commit, "files": [], "provenance": "git-head-unverified"}
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
                            manifest["files"].append(name)
                            item = tarfile.TarInfo(name)
                            item.size, item.mode = len(data), mode
                            output.addfile(item, io.BytesIO(data))
                    finally:
                        blobs.stdin.close()
                    if blobs.wait() != 0:
                        raise ValueError("Git blob export failed: " + repo)
            manifest["files"].sort()
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
    parser.add_argument("action", choices=("pack", "inventory"))
    parser.add_argument("root", nargs="?", default=os.getcwd())
    parser.add_argument("archive", nargs="?")
    args = parser.parse_args()
    if args.action == "pack":
        if not args.archive:
            parser.error("pack requires ROOT ARCHIVE")
        manifest = pack(args.root, args.archive)
    else:
        manifest = inventory(args.root)
    print(json.dumps({"commit": manifest["commit"], "files": len(manifest["files"]),
                      "provenance": manifest["provenance"]}))


if __name__ == "__main__":
    main()
