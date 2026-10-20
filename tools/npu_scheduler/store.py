from __future__ import annotations

import json
import sqlite3
import threading
from collections.abc import Iterable
from pathlib import Path

from .models import Submission, new_job_id, utc_now


class ConflictError(RuntimeError):
    pass


class NotFoundError(KeyError):
    pass


class JobStore:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._connection = sqlite3.connect(
            str(path), check_same_thread=False, isolation_level=None
        )
        self._connection.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._connection:
            self._connection.execute("PRAGMA journal_mode=WAL")
            self._connection.executescript("""
                CREATE TABLE IF NOT EXISTS jobs (
                  id TEXT PRIMARY KEY, seq INTEGER UNIQUE NOT NULL,
                  request_id TEXT UNIQUE NOT NULL, payload TEXT NOT NULL,
                  payload_hash TEXT NOT NULL, status TEXT NOT NULL,
                  reason_code TEXT, message TEXT, exit_code INTEGER,
                  launch_intent INTEGER NOT NULL DEFAULT 0,
                  created_at TEXT NOT NULL, updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS reservations (
                  host TEXT PRIMARY KEY, job_id TEXT NOT NULL REFERENCES jobs(id)
                );
                CREATE TABLE IF NOT EXISTS events (
                  seq INTEGER PRIMARY KEY AUTOINCREMENT, job_id TEXT NOT NULL,
                  created_at TEXT NOT NULL, kind TEXT NOT NULL, detail TEXT
                );
            """)

    def close(self) -> None:
        self._connection.close()

    def submit(self, submission: Submission) -> tuple[dict[str, object], bool]:
        payload = submission.normalized_json()
        digest = submission.digest()
        with self._lock, self._connection:
            existing = self._connection.execute(
                "SELECT * FROM jobs WHERE request_id=?", (submission.request_id,)
            ).fetchone()
            if existing:
                if existing["payload_hash"] != digest:
                    raise ConflictError(
                        "request_id already exists with different content"
                    )
                return self._row(existing), False
            next_seq = self._connection.execute(
                "SELECT COALESCE(MAX(seq), 0) + 1 FROM jobs"
            ).fetchone()[0]
            job_id, now = new_job_id(), utc_now()
            self._connection.execute(
                "INSERT INTO jobs(id,seq,request_id,payload,payload_hash,status,created_at,updated_at) VALUES(?,?,?,?,?,'QUEUED',?,?)",
                (job_id, next_seq, submission.request_id, payload, digest, now, now),
            )
            self._event(job_id, "submitted", None)
            return self.get(job_id), True

    def get(self, job_id: str) -> dict[str, object]:
        with self._lock:
            row = self._connection.execute(
                "SELECT * FROM jobs WHERE id=?", (job_id,)
            ).fetchone()
            if not row:
                raise NotFoundError(job_id)
            return self._row(row)

    def list(
        self, statuses: Iterable[str] | None = None, limit: int = 100, offset: int = 0
    ) -> list[dict[str, object]]:
        with self._lock:
            values = tuple(statuses or ())
            if values:
                placeholders = ",".join("?" for _ in values)
                rows = self._connection.execute(
                    f"SELECT * FROM jobs WHERE status IN ({placeholders}) ORDER BY seq DESC LIMIT ? OFFSET ?",
                    values + (limit, offset),
                ).fetchall()
            else:
                rows = self._connection.execute(
                    "SELECT * FROM jobs ORDER BY seq DESC LIMIT ? OFFSET ?",
                    (limit, offset),
                ).fetchall()
            return [self._row(row) for row in rows]

    def queued(self) -> list[dict[str, object]]:
        with self._lock:
            rows = self._connection.execute(
                "SELECT * FROM jobs WHERE status='QUEUED' ORDER BY seq"
            ).fetchall()
            return [self._row(row) for row in rows]

    def reservations(self) -> dict[str, str]:
        with self._lock:
            return {
                row["host"]: row["job_id"]
                for row in self._connection.execute(
                    "SELECT host,job_id FROM reservations"
                )
            }

    def reserve(self, job_id: str, hosts: Iterable[str]) -> bool:
        with self._lock:
            try:
                self._connection.execute("BEGIN IMMEDIATE")
                row = self._connection.execute(
                    "SELECT status FROM jobs WHERE id=?", (job_id,)
                ).fetchone()
                if not row or row["status"] != "QUEUED":
                    self._connection.rollback()
                    return False
                for host in hosts:
                    self._connection.execute(
                        "INSERT INTO reservations(host,job_id) VALUES(?,?)",
                        (host, job_id),
                    )
                self._set_status(job_id, "RESERVED", None, None)
                self._connection.commit()
                return True
            except sqlite3.IntegrityError:
                self._connection.rollback()
                return False

    def transition(
        self,
        job_id: str,
        expected: Iterable[str],
        status: str,
        reason_code: str | None = None,
        message: str | None = None,
        exit_code: int | None = None,
        launch_intent: bool | None = None,
        release: bool = False,
    ) -> bool:
        with self._lock, self._connection:
            current = self._connection.execute(
                "SELECT status FROM jobs WHERE id=?", (job_id,)
            ).fetchone()
            if not current:
                raise NotFoundError(job_id)
            if current["status"] not in tuple(expected):
                return False
            assignments = ["status=?", "reason_code=?", "message=?", "updated_at=?"]
            values: list[object] = [status, reason_code, message, utc_now()]
            if exit_code is not None:
                assignments.append("exit_code=?")
                values.append(exit_code)
            if launch_intent is not None:
                assignments.append("launch_intent=?")
                values.append(int(launch_intent))
            values.append(job_id)
            self._connection.execute(
                f"UPDATE jobs SET {','.join(assignments)} WHERE id=?", values
            )
            if release:
                self._connection.execute(
                    "DELETE FROM reservations WHERE job_id=?", (job_id,)
                )
            self._event(
                job_id,
                "state",
                json.dumps({"status": status, "reason_code": reason_code}),
            )
            return True

    def recover(self) -> None:
        with self._lock, self._connection:
            rows = self._connection.execute(
                "SELECT id,status,launch_intent FROM jobs"
            ).fetchall()
            for row in rows:
                if row["status"] == "RESERVED" and not row["launch_intent"]:
                    self.transition(
                        row["id"],
                        ("RESERVED",),
                        "QUEUED",
                        "service_restarted",
                        release=True,
                    )
                elif row["status"] in ("STARTING", "RUNNING"):
                    self.transition(
                        row["id"],
                        (row["status"],),
                        "UNKNOWN",
                        "service_restarted",
                        "Service restarted while remote execution was unresolved",
                    )

    def _set_status(
        self, job_id: str, status: str, reason: str | None, message: str | None
    ) -> None:
        self._connection.execute(
            "UPDATE jobs SET status=?,reason_code=?,message=?,updated_at=? WHERE id=?",
            (status, reason, message, utc_now(), job_id),
        )
        self._event(job_id, "state", json.dumps({"status": status}))

    def _event(self, job_id: str, kind: str, detail: str | None) -> None:
        self._connection.execute(
            "INSERT INTO events(job_id,created_at,kind,detail) VALUES(?,?,?,?)",
            (job_id, utc_now(), kind, detail),
        )

    @staticmethod
    def _row(row: sqlite3.Row) -> dict[str, object]:
        value = dict(row)
        value.update(json.loads(str(value.pop("payload"))))
        value.pop("payload_hash", None)
        value["launch_intent"] = bool(value["launch_intent"])
        return value
