from __future__ import annotations

import hashlib
import json
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import PurePosixPath

ACTIVE_STATES = ("RESERVED", "STARTING", "RUNNING", "CHECKING", "UNKNOWN")
TERMINAL_STATES = ("SUCCEEDED", "FAILED", "CANCELLED")
ALL_STATES = ("QUEUED",) + ACTIVE_STATES + TERMINAL_STATES


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


@dataclass(frozen=True)
class Submission:
    request_id: str
    name: str
    execution_host: str
    reserved_hosts: tuple[str, ...]
    script_path: str | None = None
    script_content: str | None = None
    working_directory: str | None = None

    def validate(self, known_hosts: dict[str, object]) -> Submission:
        if not self.request_id.strip() or len(self.request_id) > 128:
            raise ValueError("request_id must contain 1-128 characters")
        if not self.name.strip() or len(self.name) > 200:
            raise ValueError("name must contain 1-200 characters")
        if bool(self.script_path) == bool(self.script_content):
            raise ValueError(
                "exactly one of script_path and script_content is required"
            )
        if self.script_path and not PurePosixPath(self.script_path).is_absolute():
            raise ValueError("script_path must be absolute")
        if (
            self.working_directory
            and not PurePosixPath(self.working_directory).is_absolute()
        ):
            raise ValueError("working_directory must be absolute")
        if not self.reserved_hosts:
            raise ValueError("reserved_hosts must not be empty")
        keys = [name.casefold() for name in self.reserved_hosts]
        if len(keys) != len(set(keys)):
            raise ValueError("reserved_hosts must not contain duplicates")
        unknown = [name for name in keys if name not in known_hosts]
        if unknown:
            raise ValueError(f"unknown hosts: {', '.join(unknown)}")
        execution_key = self.execution_host.casefold()
        if execution_key not in keys:
            raise ValueError("execution_host must be included in reserved_hosts")
        canonical = {key: known_hosts[key].name for key in keys}
        return Submission(
            self.request_id.strip(),
            self.name.strip(),
            known_hosts[execution_key].name,
            tuple(canonical[key] for key in keys),
            self.script_path,
            self.script_content,
            self.working_directory,
        )

    def normalized_json(self) -> str:
        value = {
            "execution_host": self.execution_host,
            "name": self.name,
            "request_id": self.request_id,
            "reserved_hosts": list(self.reserved_hosts),
            "script_content": self.script_content,
            "script_path": self.script_path,
            "working_directory": self.working_directory,
        }
        return json.dumps(
            value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        )

    def digest(self) -> str:
        return hashlib.sha256(self.normalized_json().encode("utf-8")).hexdigest()


def new_job_id() -> str:
    return str(uuid.uuid4())
