#!/usr/bin/env python3
"""功能：校验 SDK records 和模型 sidecar，用冻结 drift 模型导出 Perfetto trace 与入口偏差。
调用关系：读取 clock E2E 的输出，调用 profiling SDK parser、analysis 和 renderer。
时间坐标：保留 raw cycle，以两边共用的 sleep_at target 为原点，避免分别归零掩盖入口偏差。
"""
import argparse
from dataclasses import replace
from decimal import Decimal, ROUND_HALF_UP, localcontext
import json
from pathlib import Path

from tilexr_profiling.analysis import analyze_run
from tilexr_profiling.manifest import mapping_fingerprint
from tilexr_profiling.pairing import load_profile_run
from tilexr_profiling.perfetto import render_perfetto


def _correct(model, cycle):
    with localcontext() as context:
        context.prec = 50
        value = (Decimal(cycle) + Decimal(str(model['offset_cycles'])) +
                 Decimal(str(model['rate_correction'])) *
                 Decimal(cycle - model['anchor_cycle']))
        return int(value.to_integral_value(rounding=ROUND_HALF_UP))


def export_trace(directory):
    directory = Path(directory)
    models = [json.loads((directory / f'sleep_at_rdma_rank{rank}.json').read_text())
              for rank in (0, 1)]
    for rank, model in enumerate(models):
        if (model['schema'] != 'tilexr_sleep_at_rdma_profile.v1' or model['rank'] != rank or
                model['state'] != 2 or model['wqes'] != 64 or model['bytes_per_wqe'] != 4096 or
                model['markers'] != 2 or not model['data_and_guards_verified'] or
                model['producer_after'] - model['producer_before'] != 64 or
                model['completed'] != model['producer_after']):
            raise ValueError('incomplete RDMA/profile evidence')
    first = models[0]
    for model in models:
        if any(model[key] != first[key] for key in ('capture_id', 'target', 'cycles_per_second')):
            raise ValueError('profile ranks do not share capture, target and frequency')
    frequency = first['cycles_per_second']
    if frequency <= 0:
        raise ValueError('invalid cycle frequency')
    mapping = {'schema': 'tilexr_profiling_mapping.v1', 'operator': 'sleep_at_rdma_write64',
               'cycles_per_us': frequency / 1e6,
               'stages': [{'name': 'rdma_write64_kernel', 'category': 'total',
                           'critical': True, 'begin': 1, 'end': 2}]}
    participants = [{'stream_id': rank << 16, 'rank': rank, 'logical_lane': 0, 'sampled': True,
                     'device': {'id': str(model['device']), 'namespace': 'acl_logical'}}
                    for rank, model in enumerate(models)]
    manifest = {'schema': 'tilexr_profiling_manifest.v1', 'attempt_id': first['capture_id'],
                'capture_id': first['capture_id'], 'mapping_sha256': mapping_fingerprint(mapping),
                'participants': participants,
                'stage_metrics': [{'stream_id': rank << 16, 'stage_id': 0,
                                   'stage': 'rdma_write64_kernel', 'source': 'verified_test_contract',
                                   'bytes': 64 * 4096, 'byte_semantics': 'outgoing RDMA WRITE payload',
                                   'scope': 'per_interval'} for rank in (0, 1)]}
    run = load_profile_run(mapping, [directory / f'sleep_at_rdma_rank{r}.records.bin' for r in (0, 1)],
                           manifest=manifest)
    if run.usable_stream_count != 2 or run.dropped_record_count or run.overflowed_stream_count:
        raise ValueError('profiling capture incomplete')
    origin = first['target']
    lanes = []
    for lane in run.launches[0].lanes:
        model = models[lane.rank]
        if len(lane.samples) != 1 or len(lane.records) != 2:
            raise ValueError('expected exactly two raw markers and one kernel interval')
        sample = lane.samples[0]
        if (sample.raw_start, sample.raw_end) != (model['raw_entry'], model['raw_exit']):
            raise ValueError('sidecar timestamps do not match SDK records')
        for raw_key, corrected_key in (('raw_entry', 'corrected_entry'), ('raw_exit', 'corrected_exit')):
            if abs(_correct(model, model[raw_key]) - model[corrected_key]) > 1:
                raise ValueError('sidecar correction does not match frozen model')
        begin, end = model['corrected_entry'], model['corrected_exit']
        if begin < origin or end <= begin or sample.raw_start < model['local_deadline']:
            raise ValueError('kernel violates sleep deadline or interval')
        attributes = dict(sample.attributes, target_reference_cycle=origin,
                          local_deadline=model['local_deadline'], corrected_entry_cycle=begin,
                          corrected_exit_cycle=end, model_generation=model['model_generation'],
                          clock_model_bound_ns=model['clock_model_bound_ns'],
                          clock_assumption='frozen_affine_drift_and_symmetric_path',
                          independent_physical_alignment_verified=False)
        sample = replace(sample, start_us=(begin-origin)*1e6/frequency,
                         end_us=(end-origin)*1e6/frequency,
                         duration_us=(end-begin)*1e6/frequency, attributes=attributes)
        lanes.append(replace(lane, samples=(sample,)))
    run = replace(run, launches=(replace(run.launches[0], lanes=tuple(lanes)),),
                  clock_domain='logical_rank0_frozen_drift_model',
                  display_origin='shared_sleep_at_reference_target',
                  attributes=dict(run.attributes, frozen_models=models,
                                  independent_physical_alignment_verified=False))
    trace = render_perfetto(run, analyze_run(run))
    entries = [model['corrected_entry'] for model in models]
    summary = {'capture_id': first['capture_id'], 'target': origin,
               'entry_skew_ns': (max(entries)-min(entries))*1e9/frequency,
               'entry_lateness_ns': [(value-origin)*1e9/frequency for value in entries],
               'kernel_duration_ns': [(m['corrected_exit']-m['corrected_entry'])*1e9/frequency for m in models],
               'raw_entries': [m['raw_entry'] for m in models], 'corrected_entries': entries,
               'clock_model_bound_ns': max(m['clock_model_bound_ns'] for m in models),
               'wqes_per_rank': 64, 'bytes_per_wqe': 4096,
               'independent_physical_alignment_verified': False}
    trace['metadata']['alignment'] = summary
    return trace, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    trace, summary = export_trace(args.directory)
    (args.directory/'sleep_at_rdma.perfetto.json').write_text(
        json.dumps(trace, ensure_ascii=False, allow_nan=False, separators=(',', ':'))+'\n', encoding='utf-8')
    (args.directory/'sleep_at_rdma.alignment.json').write_text(
        json.dumps(summary, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps(summary, allow_nan=False))


if __name__ == '__main__':
    main()
