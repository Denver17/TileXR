"""Shared current-run records for validation tests; no stage execution."""
import os
from unittest import mock
import workflow
from validation_fixture import TemporaryRoot

class CurrentRunFixture(TemporaryRoot):
    def config(self):
        return workflow.Config("run")

    def setUp(self):
        super().setUp()
        self.hostfile.write_text("192.0.2.1\n192.0.2.2\n192.0.2.3\n")
        os.environ.update(TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_VALIDATION_OPERATOR_MODE="both",
                          TILEXR_VALIDATION_SERVERS="2", TILEXR_COMM_ID="192.0.2.1:10177")
        os.environ.pop("TILEXR_DEMO_BARRIER_ADDR", None)
        self.manifest = dict(checkpoint=None, build_id="build",
                             source=dict(commit="a" * 40, provenance="git"))

        os.environ.update(TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2")
        self.manifest.update(mode="production", artifacts={f"lib/tilexr/kernels/tilexr_ep_{operator}_memory_server_dedup_kernel.o": char * 64 for operator, char in (("dispatch", "d"), ("combine", "c"))})
        self.manifest["source"]["content_sha256"] = "a" * 64


    def full_records(self, operator="both", mode="functional", loops=5, warmup=2):
        with mock.patch.dict(os.environ, {
                "TILEXR_VALIDATION_OPERATOR_MODE": operator, "TILEXR_VALIDATION_RUN_MODE": mode,
                "TILEXR_VALIDATION_LOOPS": str(loops), "TILEXR_VALIDATION_WARMUP": str(warmup)}):
            config = self.config()
        config.chain["run_mode"] = mode
        if mode == "performance":
            config.chain["msprof"] = "msprof"
        mappings = workflow.rank_mapping(config.hosts())[:8]
        total, both = loops + warmup, operator == "both"
        records = []
        for mapping in mappings:
            identity = dict(workflow.rank_record(mapping, "run", self.manifest),
                            pid=100 + mapping["rank"], server_dedup_enabled=True, **config.chain)
            progress = dict(submitted=loops, completed=loops, warmup_submitted=warmup,
                            warmup_completed=warmup, cleanup="complete", stream_synchronizations=1)
            chain = dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3)
            if operator != "combine":
                stride = workflow.source_send_stride(config.chain)
                dispatch = dict(identity, **progress, kernel_scope="dispatch", native_status="dispatch",
                    dispatch_first_magic=1, dispatch_window_mask=2 if both or total == 1 else 3,
                    last_magic=2 * total - 1 if both else total,
                    window_offset=workflow.MEMORY_BOUNDARY_BYTES if both else total % 2 * workflow.MEMORY_BOUNDARY_BYTES,
                    first_hop_packets=2, first_hop_received=2, first_hop_bytes=2 * stride, token_stride=stride,
                    direct_packets=3, direct_received=3, direct_bytes=3 * stride,
                    local_data_bytes=stride, same_server_data_bytes=3 * stride, cross_server_data_bytes=3 * stride,
                    relay_packets=2, relay_received=2, relay_bytes=2 * stride, data_bytes=7 * stride,
                    relay_validated=True, dispatch_outputs_validated=True, five_tables_validated=True,
                    member_addresses_validated=True, flags_cleared=True, first_hop_validated=True,
                    direct_validated=True, route_coverage_validated=True, source_outputs_validated=True,
                    weights_unchanged=True)
                if operator == "dispatch":
                    records.append(dict(dispatch, chain=chain))
                    continue
            record = dict(identity, **progress, chain=chain,
                combine_delay_us=0, combine_empty=False, native_status="combine-m5",
                five_tables_linked=both, send_counts_linked=both,
                combine_input_check="dispatch-final-reference" if both else "full-snapshot",
                combine_inputs_unchanged=not both, independent_expert_out_unchanged=not both,
                expert_out_source="dispatch_expand_x" if both else "standalone_expert_out",
                combine_stage_validated=True, combine_control_validated=True, combine_control_reserved_unchanged=True,
                combine_result_validated=True, combine_result_flags_cleared=True, combine_y_out_validated=True,
                combine_unwritten_results_unchanged=True, combine_partial_validated=True,
                combine_numeric_policy="fp32-sequential-rne-bit-exact", combine_numeric_atol=0, combine_numeric_rtol=0,
                combine_observed_rounds=1)
            record.update(combine_traffic(config.chain, mapping["rank"]))
            for name in ("dispatch", "combine"):
                active = operator in (name, "both")
                first = (2 if name == "combine" and both else 1) if active else 0
                last = (2 * total - int(name == "dispatch") if both else total) if active else 0
                record.update({name + "_first_magic": first, name + "_last_magic": last,
                               name + "_window_mask": (1 << (first % 2) if both or total == 1 else 3) if active else 0,
                               name + "_window_offset": last % 2 * workflow.MEMORY_BOUNDARY_BYTES})
                for stage, count in (("", loops), ("warmup_", warmup)):
                    for state in ("submitted", "completed"):
                        record[name + "_" + stage + state] = count if active else 0
            record["combine_checked_window_mask"] = record["combine_window_mask"]
            if both:
                record["dispatch"] = dispatch
            records.append(record)
        return config, mappings, records


def combine_traffic(config, rank=0):
    """Small independent traffic fixture for the complete Combine protocol."""
    h, bs, k = (config[key] for key in ('h', 'bs', 'topk'))
    stride = ((2 * h + 479) // 480) * 512
    return dict(rank=rank, combine_type3_rows=2, combine_type1_rows=3, combine_type2_rows=1,
        combine_token_num=6, combine_direct_received_rows=bs - 1, combine_partial_received_rows=1,
        combine_partial_payload_bytes=2*h, combine_partial_wire_bytes=stride,
        combine_partial_member_rows=2, combine_partial_local_members=1,
        combine_partial_stage_read_bytes=4*h, combine_partial_self_read_bytes=2*h,
        combine_algorithm_bytes=(12+2*bs)*h, combine_final_read_bytes=bs*2*h,
        combine_y_out_bytes=bs*2*h, combine_flag_clear_bytes=bs*k*(stride//512)*32,
        combine_stage_copy_bytes=8*h, combine_direct_payload_bytes=6*h,
        combine_direct_wire_bytes=3*stride,
        combine_control_publish_bytes=(64*8+8*(64-config.get('combine_send_core_num',32)))*32)


def combine_events(send=32):
    events = {}
    for core in range(64):
        reader = core >= send
        pairs = [0x3003, 0x3017, 0x3019 if reader else 0x301b]
        if reader:
            pairs.extend((0x301d, 0x301f))
        pairs.append(0x3013)
        rows = [(0, 0x3001)]
        for index, event in enumerate(pairs, 1):
            rows.extend(((index*1000, event), (index*1000+500, event+1)))
        events[core] = rows + [(7000, 0x3002)]
    return events
