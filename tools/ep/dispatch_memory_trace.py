#!/usr/bin/env python

"""Export EP Dispatch Memory profile logs as three Chrome Trace files."""

import combine_memory_trace as trace


trace.PROFILE_PREFIX = "EP_DISPATCH_MEMORY_PROFILE"
trace.PROFILE_VERSION = 2
trace.PROFILE_CYCLES_PER_US = 1000
trace.PROFILE_CORE_ROLES = ("sender", "count_cumsum")
trace.SEND_GROUP_COUNT_DIAGNOSTIC = "send_group_count"
trace.HIDDEN_ZERO_DURATION_ROLE_PHASES = (("sender", 3),)
trace.DEFAULT_OUTPUT_PREFIX = "ep_dispatch_memory"
trace.TRACE_KERNEL_NAME = "ep_dispatch_memory"
trace.TRACE_SCHEMA = "tilexr_ep_dispatch_memory_trace.v2"
trace.TRACE_OUTPUT_PREFIX = "EP_DISPATCH_MEMORY_TRACE"
trace.TRACE_DESCRIPTION = __doc__
trace.TIME_POINT_NAMES = (
    "init_begin",
    "init_end",
    "transfer_group0_end",
    "transfer_end",
    "cumsum_end",
    "first_payload_ready",
    "last_payload_ready",
    "final_end",
)
trace.METRIC_NAMES = ()
trace.DIAGNOSTIC_NAMES = (
    "assigned_routes",
    "issued_routes",
    "count_records",
    "alltoall_core_count",
    "send_buffer_num",
    "status_poll_attempts",
    "status_poll_misses",
    "cumsum_poll_attempts",
    "cumsum_gate_attempts",
    "valid_experts",
    "receive_batches",
    "receive_tokens",
    "payload_poll_attempts",
    "payload_poll_misses",
    "max_copy_token_count",
    "blocks_per_token",
    "send_group_count",
)
trace.PROVENANCE_INT_NAMES = (
    "world_size",
    "bs",
    "h",
    "k",
    "experts",
    "quant_mode",
    "comm_quant_mode",
    "active_mask_type",
    "expert_token_nums_type",
)


if __name__ == "__main__":
    trace.main()
