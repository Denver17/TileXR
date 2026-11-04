"""Write complete or partial JSON results and concise terminal summaries.

Replace reports through a temporary file; keep report output separate from the
Worker control protocol and per-rank diagnostic logs.
"""
import json
from pathlib import Path


def write_report(output, report):
    target = Path(output) / "result.json"
    temporary = target.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(target)


def summary(output, report):
    print("{} case={} ranks={} report={}".format(
        report["status"].upper(), report["case"]["id"], len(report["devices"]),
        Path(output) / "result.json"))
    if report.get("error"):
        print(report["error"])
