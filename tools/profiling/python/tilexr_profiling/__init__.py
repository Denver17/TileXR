"""Producer-neutral profiling analysis and report generation."""

from .version import __version__
from .model import MAPPING_SCHEMA, RECORDS_SCHEMA, ProfilingInputError
from .cli import GeneratedReports, generate_reports


__all__ = [
    "__version__",
    "GeneratedReports",
    "MAPPING_SCHEMA",
    "ProfilingInputError",
    "RECORDS_SCHEMA",
    "generate_reports",
]
