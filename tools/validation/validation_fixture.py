"""Isolated filesystem and environment for validation workflow tests."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import workflow


class TemporaryRoot(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tilexr-validation-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "coordinator"
        self.root.mkdir()
        self.hostfile = self.root / "hostfile"
        self.hostfile.write_text("127.0.0.1\n", encoding="utf-8")
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("TILEXR_VALIDATION_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root),
                           TILEXR_VALIDATION_HOSTFILE=str(self.hostfile),
                           TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_TIMEOUT="30")
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)

    def config(self, action="sync"):
        return workflow.Config(action)
