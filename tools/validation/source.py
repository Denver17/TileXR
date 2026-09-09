#!/usr/bin/env python3
"""导出固定 Git HEAD；在无 Git 的部署目录校验源码来源。"""

import argparse
import hashlib
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


def _git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


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


def verify_source(root, manifest):
    """Raise ValueError when an expected source file is missing or changed."""
    if (not isinstance(manifest, dict) or
            not isinstance(manifest.get("commit"), str) or
            not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", manifest.get("commit", "")) or
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


def snapshot(root):
    root = Path(root).resolve()
    manifest_path = root / MANIFEST
    if manifest_path.exists() and not (root / ".git").exists():
        if manifest_path.is_symlink():
            raise ValueError("source manifest must not be a symlink")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        verify_source(root, manifest)
        return manifest
    manifest = {
        "commit": _git(root, "rev-parse", "HEAD").decode().strip(),
        "files": {name: _digest(_path(root, name)) for _, _, name, _ in _tree(root)},
        "dirty": bool(_git(root, "status", "--porcelain", "--untracked-files=no").strip()),
    }
    return manifest


def pack(root, archive):
    """Export HEAD blobs, not modified working files or Windows CRLF checkouts."""
    root = Path(root).resolve()
    subprocess.run(["git", "-C", str(root), "submodule", "update", "--init", "--recursive"], check=True)
    commit = _git(root, "rev-parse", "HEAD").decode().strip()
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
    else:
        manifest = snapshot(args.root)
    print(json.dumps({"commit": manifest["commit"], "files": len(manifest["files"]),
                      "dirty": manifest.get("dirty", False)}))


if __name__ == "__main__":
    main()
