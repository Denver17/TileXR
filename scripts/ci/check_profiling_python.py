#!/usr/bin/env python3
"""Build and consume profiling distributions without network access or source imports.

Provision build, setuptools, wheel and venv/ensurepip before invoking this runner.
Ordinary unit tests do not build distributions; this is the separate release check.
"""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import venv
from uuid import UUID
import zipfile


ARTIFACTS = ("perfetto_trace.json", "analysis.md", "report.html")
CAPTURE_ID = "018f6f5e-41a0-7d31-9ad1-82d61fc0a001"


def run(command, cwd, expected=0):
    environment = None
    if "-I" in command:
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        environment.pop("PYTHONHOME", None)
    result = subprocess.run([str(part) for part in command], cwd=str(cwd),
                            env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode != expected:
        raise RuntimeError("command failed (expected {}, got {}): {}\n{}".format(
            expected, result.returncode, " ".join(map(str, command)), result.stdout))
    if result.stdout.strip():
        print(result.stdout.strip(), flush=True)
    return result


def write_fixture(directory):
    mapping = {
        "schema": "tilexr_profiling_mapping.v1", "operator": "installed_add4",
        "cycles_per_us": 1000.0,
        "stages": [{"name": "total", "category": "total", "critical": True,
                    "begin": 1, "end": 2}],
    }
    mapping_path = directory / "operator.mapping.json"
    mapping_path.write_text(json.dumps(mapping), encoding="utf-8")
    for name, records in (("complete", [(1000, 1), (3000, 2)]),
                          ("incomplete", [(1000, 1)])):
        payload = b"".join(struct.pack("<QII", timestamp, event, 0)
                           for timestamp, event in records)
        stream = struct.pack("<IIIIQ", 0, len(records), 0, 1, 24 + len(payload)) + payload
        header = struct.pack("<8sHHHHHBBI16sQ", b"TXRPRF1\0", 1, 0, 48, 16, 24,
                             1, 0, 1, UUID(CAPTURE_ID).bytes, 48 + len(stream))
        (directory / (name + ".records.bin")).write_bytes(header + stream)
    (directory / "corrupt.records.bin").write_bytes(b"corrupt")
    return mapping_path


def check_artifacts(directory, expected_status):
    if {path.name for path in directory.iterdir()} != set(ARTIFACTS):
        raise RuntimeError("unexpected report artifacts: " + str(directory))
    trace = json.loads((directory / ARTIFACTS[0]).read_text(encoding="utf-8"))
    if trace["metadata"]["status"] != expected_status:
        raise RuntimeError("wrong installed report status")
    if trace["metadata"]["provenance"]["capture_id"] != CAPTURE_ID:
        raise RuntimeError("installed report lost capture provenance")
    if not trace["traceEvents"]:
        raise RuntimeError("installed report did not render fixture events")
    for filename in ARTIFACTS[1:]:
        text = (directory / filename).read_text(encoding="utf-8")
        if "installed" not in text or "Profiling" not in text:
            raise RuntimeError("installed text report is empty or unrelated")


def check_wheel(wheel):
    with zipfile.ZipFile(wheel) as archive:
        names = archive.namelist()
        roots = {name.split("/")[0] for name in names}
        if "tilexr_profiling" not in roots or any(
            root != "tilexr_profiling" and not root.endswith(".dist-info") for root in roots
        ):
            raise RuntimeError("wheel contains unexpected packages: " + repr(sorted(roots)))
        if "tilexr_profiling/__main__.py" not in names:
            raise RuntimeError("wheel is missing the canonical CLI")


def unpack_sdist(sdist, destination):
    # Python 3.9 has no tarfile data filter: validate every member before extraction.
    with tarfile.open(sdist) as archive:
        members = archive.getmembers()
        for member in members:
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve()) or member.issym() or member.islnk():
                raise RuntimeError("unsafe sdist member: " + member.name)
            if not (member.isdir() or member.isfile()):
                raise RuntimeError("unexpected sdist member type: " + member.name)
        archive.extractall(destination, members=members)
    projects = list(destination.glob("*/pyproject.toml"))
    if len(projects) != 1:
        raise RuntimeError("sdist must contain exactly one project")
    return projects[0].parent


def consume_wheel(wheel, directory):
    check_wheel(wheel)
    directory.mkdir()
    environment = directory / "venv"
    venv.EnvBuilder(with_pip=True).create(environment)
    python = environment / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    outside = directory / "outside"
    outside.mkdir()
    clean_env = dict(os.environ)
    clean_env.pop("PYTHONPATH", None)
    clean_env.pop("PYTHONHOME", None)
    # -I ignores all Python environment overrides; the explicit environment also
    # prevents a subprocess launched by a future test from inheriting source paths.
    installed = subprocess.run([str(python), "-I", "-m", "pip", "install", "--no-index",
                                "--no-deps", str(wheel)], cwd=str(outside), env=clean_env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if installed.returncode:
        raise RuntimeError(installed.stdout)
    print(installed.stdout.strip(), flush=True)
    probe = (
        "import pathlib, sys, tilexr_profiling; "
        "p=pathlib.Path(tilexr_profiling.__file__).resolve(); "
        "assert p.is_relative_to(pathlib.Path(sys.prefix).resolve()), str(p); "
        "print('installed package:', p)"
    )
    run([python, "-I", "-c", probe], outside)
    mapping = write_fixture(outside)
    for name, expected, status in (("complete", 0, "complete"), ("incomplete", 2, "incomplete"),
                                   ("corrupt", 1, None)):
        output = outside / (name + "-report")
        run([python, "-I", "-m", "tilexr_profiling", "--mapping", mapping, "--records",
             outside / (name + ".records.bin"), "--output-dir", output], outside, expected)
        if status:
            check_artifacts(output, status)
        elif output.exists():
            raise RuntimeError("fatal input published a report")
    api = (
        "from pathlib import Path\n"
        "from tilexr_profiling import generate_reports\n"
        "result=generate_reports(Path('operator.mapping.json'), [Path('complete.records.bin')], "
        "Path('api-report'), attributes={'kernel_version':'installed-v1'})\n"
        "assert result.status == 'complete'\n"
        "assert len(result.artifacts) == 3\n"
    )
    run([python, "-I", "-c", api], outside)
    check_artifacts(outside / "api-report", "complete")


def verify(repo, work):
    if sys.version_info < (3, 9):
        raise RuntimeError("profiling package checks require Python >=3.9")
    if work.resolve().is_relative_to(repo.resolve()):
        raise RuntimeError("package consumer checks must run outside the source repository; choose an external --work-dir")
    for module in ("build", "setuptools", "wheel"):
        if importlib.util.find_spec(module) is None:
            raise RuntimeError("preinstall the build tool before this offline runner: " + module)
    # Setuptools writes build/ and egg-info beside the project. Stage a clean copy
    # so simultaneous checks and editable installs cannot mutate each other's build.
    source = work / "source"
    shutil.copytree(repo / "tools/profiling", source,
                    ignore=shutil.ignore_patterns("build", "dist", "*.egg-info", "__pycache__"))
    dist = work / "dist"
    run([sys.executable, "-m", "build", "--no-isolation", "--wheel", "--sdist",
         "--outdir", dist, source], work)
    wheels = list(dist.glob("*.whl"))
    sdists = list(dist.glob("*.tar.gz"))
    if len(wheels) != 1 or len(sdists) != 1:
        raise RuntimeError("expected one wheel and one sdist")
    consume_wheel(wheels[0], work / "wheel-consumer")
    unpacked = work / "sdist-source"
    unpacked.mkdir()
    project = unpack_sdist(sdists[0], unpacked)
    rebuilt = work / "sdist-dist"
    run([sys.executable, "-m", "build", "--no-isolation", "--wheel", "--outdir", rebuilt,
         project], work)
    rebuilt_wheels = list(rebuilt.glob("*.whl"))
    if len(rebuilt_wheels) != 1:
        raise RuntimeError("sdist did not rebuild exactly one wheel")
    consume_wheel(rebuilt_wheels[0], work / "sdist-consumer")
    print("PASS: wheel and sdist install independently; isolated CLI 0/1/2 and API reports verified.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--work-dir", type=Path, help="retain evidence in a new subdirectory here")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="p-", dir=str(args.work_dir.resolve())))
        print("Evidence directory:", work, flush=True)
        verify(args.repo_root.resolve(), work)
    else:
        with tempfile.TemporaryDirectory(prefix="tilexr-profiling-package-") as temporary:
            verify(args.repo_root.resolve(), Path(temporary))


if __name__ == "__main__":
    main()
