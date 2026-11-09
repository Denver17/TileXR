# Purpose: Public entry points for offline profiling; imports the parser and report APIs.
# 用途与关系：离线 profiling 的公共入口，导出解析器与报表 API。
"""Producer-neutral profiling analysis and report generation."""

from .version import __version__
from .model import (MAPPING_SCHEMA, MAPPING_SCHEMA_V2, SUPPORTED_MAPPING_SCHEMAS,
                    RECORDS_SCHEMA, ProfilingInputError)
from .cli import GeneratedReports, generate_reports
from .manifest import (MANIFEST_SCHEMA, attach_host_events, attach_completion, load_manifest,
                       mapping_fingerprint, merge_manifest_shards)
from .budgets import InputLimits
from .capture_set import (CAPTURE_SET_SCHEMA, load_capture_set, analyze_capture_set,
                          generate_capture_set_reports)
from .sampling import SAMPLING_SCHEMA, SamplingPolicy, SamplingSelection
from .environment import ENVIRONMENT_SCHEMA, collect_environment, reconcile_environment
from .calibration import (CALIBRATION_SCHEMA, CalibrationSession, load_calibration, convert_timestamp,
                          compare_global_order, derive_global_coordinates)
from .serialization import build_analysis_document, render_analysis_json, render_analysis_csv
from .comparison import Threshold, compare_reports
from .collectives import build_collective_expected, collective_capture_id


__all__ = [
    "__version__",
    "GeneratedReports",
    "MAPPING_SCHEMA",
    "MAPPING_SCHEMA_V2", "SUPPORTED_MAPPING_SCHEMAS",
    "ProfilingInputError",
    "RECORDS_SCHEMA",
    "generate_reports",
    "MANIFEST_SCHEMA",
    "attach_host_events",
    "attach_completion",
    "InputLimits",
    "CAPTURE_SET_SCHEMA", "load_capture_set", "analyze_capture_set", "generate_capture_set_reports",
    "SAMPLING_SCHEMA", "SamplingPolicy", "SamplingSelection",
    "ENVIRONMENT_SCHEMA", "collect_environment", "reconcile_environment",
    "CALIBRATION_SCHEMA", "CalibrationSession", "load_calibration", "convert_timestamp", "compare_global_order",
    "derive_global_coordinates", "build_analysis_document", "render_analysis_json",
    "render_analysis_csv", "Threshold", "compare_reports",
    "load_manifest",
    "mapping_fingerprint",
    "merge_manifest_shards",
    "build_collective_expected", "collective_capture_id",
]
