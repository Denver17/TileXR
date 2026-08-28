#!/usr/bin/env python

"""Export EP Dispatch Memory profile logs as three Chrome Trace files."""

import combine_memory_trace as trace


trace.PROFILE_PREFIX = "EP_DISPATCH_MEMORY_PROFILE"
trace.PROFILE_VERSION = 1
trace.PROFILE_CYCLES_PER_US = 1000
trace.PROFILE_CORE_ROLES = ("sender", "count_cumsum")
trace.DEFAULT_OUTPUT_PREFIX = "ep_dispatch_memory"
trace.TRACE_KERNEL_NAME = "ep_dispatch_memory"
trace.TRACE_SCHEMA = "tilexr_ep_dispatch_memory_trace.v1"
trace.TRACE_OUTPUT_PREFIX = "EP_DISPATCH_MEMORY_TRACE"
trace.TRACE_DESCRIPTION = __doc__
trace.TIME_POINT_NAMES = (
    "init_begin",
    "static_init_end",
    "route_prepare_end",
    "count_publish_end",
    "send_buffer_init_end",
    "send_issue_end",
    "count_status_wait_end",
    "cumsum_publish_end",
    "pre_local_barrier_end",
    "local_copy_init_end",
    "cumsum_gate_end",
    "receive_metadata_end",
    "first_payload_ready",
    "all_payloads_ready",
    "output_drain_end",
    "flag_clear_issue_end",
    "local_copy_end",
    "final_barrier_end",
    "final_end",
)
trace.METRIC_NAMES = (
    "route_prepare",
    "count_compute",
    "count_store_issue",
    "dispatch_input_load",
    "dispatch_quant",
    "dispatch_pack",
    "dispatch_store_issue",
    "status_flag_load",
    "status_flag_reduce",
    "cumsum_peer_wait",
    "cumsum_gate_wait",
    "payload_flag_load",
    "payload_flag_reduce",
    "payload_load",
    "output_store_issue",
    "payload_flag_clear_issue",
)
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
