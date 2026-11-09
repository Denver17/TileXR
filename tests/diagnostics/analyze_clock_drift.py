#!/usr/bin/env python3
"""审计两 rank 的冻结仿射补偿：重算初始拟合，核对校正值，并输出相对逻辑 rank 0 的残差。

读取 test_clock_sync_e2e 的 CSV/JSON，仅将训练结束后的轮次用于残差统计。
Audit two-rank frozen affine correction from test_clock_sync_e2e CSV/JSON:
reproduce the fit, check corrected values and report post-training residuals against logical rank 0.
"""
import argparse
import csv
import json
import math
from decimal import Decimal, ROUND_HALF_UP, localcontext
from pathlib import Path
from statistics import median, mean


def read_csv(path):
    with Path(path).open(newline='') as stream:
        return list(csv.DictReader(stream))


def regression(points):
    xs, ys = zip(*points)
    mx, my = mean(xs), mean(ys)
    xx = math.fsum((x-mx)**2 for x in xs)
    if xx == 0:
        raise ValueError('zero regression time span')
    rate = math.fsum((x-mx)*(y-my) for x, y in points)/xx
    intercept = my-rate*mx
    return rate, intercept


def values(row):
    t0, t1, t2, t3 = (int(row[k]) for k in ('t0', 't1', 't2', 't3'))
    if (int(row['status']) != 0 or min(t0,t1,t2,t3) == 0 or
            t3 < t0 or t2 < t1 or t3-t0 < t2-t1 or
            t3-t0-(t2-t1) != int(row['rtt_cycles'])):
        raise ValueError('invalid timestamp exchange; no silent trimming')
    return t0,t1,t2,t3


def write_csv(path, rows):
    with Path(path).open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def analyze(directory, output):
    directory, output = Path(directory), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    if json.loads((directory/'exit.json').read_text()) != [0,0]:
        raise ValueError('both ranks must exit successfully')
    raw = read_csv(directory/'samples_rank1.csv')
    models = [r for r in read_csv(directory/'drift_models_rank1.csv') if int(r['peer']) == 1]
    remote_models = [r for r in read_csv(directory/'drift_models_rank0.csv') if int(r['peer']) == 1]
    model_fields = [k for k in models[0] if k != 'rank']
    assert [[r[k] for k in model_fields] for r in models] == [
        [r[k] for k in model_fields] for r in remote_models], 'gathered models disagree'
    by_round = {int(r['round']):r for r in models}
    model = models[-1]
    state = int(model['state'])
    assert state in (2,3,4), 'training did not close'
    trained_round = int(model['trained_at_generation'])-1
    frozen_keys = [k for k in model_fields if k != 'round']
    assert all([r[k] for k in frozen_keys] == [model[k] for k in frozen_keys]
               for r in models if int(r['round']) >= trained_round), 'coefficients changed after training'
    frequency = int(model['cycles_per_second'])
    anchor = int(model['anchor_cycle'])
    requested = int(model['requested_window_us'])
    rate, intercept = float(model['rate_correction']), float(model['offset_cycles'])
    point_rows = []
    first = None
    for row in raw:
        t0,t1,t2,t3 = values(row)
        midpoint = t0+(t3-t0)//2
        if first is None:
            first = midpoint
        point_rows.append((midpoint, ((t1-t0)+(t2-t3))/2))
        if (midpoint-first)*1000000 >= requested*frequency:
            break
    assert len(point_rows) == int(model['sample_count'])
    assert anchor == point_rows[-1][0] and anchor-first == int(model['span_cycles'])
    assert int(raw[len(point_rows)-1]['round']) == trained_round
    if state == 2:
        fitted_rate, fitted_offset = regression([(x-anchor,y) for x,y in point_rows])
        assert math.isclose(rate,fitted_rate,rel_tol=1e-7,abs_tol=1e-15)
        assert abs(intercept-fitted_offset) < 0.001
    corrections = {(int(r['round']),int(r['sequence'])):r
                   for r in read_csv(directory/'corrected_samples_rank1.csv')}
    fixed = json.loads((directory/'frozen_calibration_rank1.json').read_text())
    initial_offset = int(fixed['offset_cycles'][1])
    rows = []
    with localcontext() as context:
        context.prec = 50
        exact_offset, exact_rate = Decimal.from_float(intercept), Decimal.from_float(rate)
        for row in raw:
            t0,t1,t2,t3 = values(row)
            round_id, sequence = int(row['round']), int(row['sequence'])
            assert int(row['applied_offset_cycles']) == initial_offset
            if round_id <= trained_round:
                continue  # Entire closing round excluded, including any unused trailing samples.
            exported = corrections[(round_id,sequence)]
            assert int(exported['state']) == state
            assert int(exported['trained_at_generation']) == trained_round+1
            assert by_round[round_id]['rate_correction'] == model['rate_correction']
            midpoint = t0+(t3-t0)/2
            offset_now = ((t1-t0)+(t2-t3))/2
            corrected_ns = None
            if state == 2:
                c0,c3 = int(exported['corrected_t0']),int(exported['corrected_t3'])
                for local,aligned in ((t0,c0),(t3,c3)):
                    expected = Decimal(local)+exact_offset+exact_rate*Decimal(local-anchor)
                    rounded = int(expected.to_integral_value(rounding=ROUND_HALF_UP))
                    # Host long-double evaluation can differ by one cycle at a half-cycle boundary.
                    assert abs(rounded-aligned) <= 1, 'production correction disagrees with formula'
                corrected_ns = ((c0-t1)+(c3-t2))/2*1e9/frequency
            rows.append(dict(round=round_id,sequence=sequence,
                age_since_training_s=(midpoint-anchor)/frequency,
                first_offset_only_ns=(initial_offset-offset_now)*1e9/frequency,
                anchor_offset_only_ns=(intercept-offset_now)*1e9/frequency if state == 2 else None,
                affine_ns=corrected_ns))
    assert rows and max(r['age_since_training_s'] for r in rows) >= 60, 'need one minute after training'
    rounds = []
    for round_id in sorted({r['round'] for r in rows}):
        batch = [r for r in rows if r['round'] == round_id]
        assert len(batch) == 64
        out = dict(round=round_id,age_s=mean(r['age_since_training_s'] for r in batch),samples=len(batch))
        for key in ('first_offset_only_ns','anchor_offset_only_ns','affine_ns'):
            data = [r[key] for r in batch if r[key] is not None]
            out[key+'_median'] = median(data) if data else None
            out[key+'_max_abs'] = max(map(abs,data)) if data else None
        rounds.append(out)
    statistics = {}
    for key in ('first_offset_only_ns','anchor_offset_only_ns','affine_ns'):
        data = [r[key] for r in rows if r[key] is not None]
        statistics[key] = None if not data else dict(
            max_abs_ns=max(map(abs,data)),first_window_max_ns=rounds[0][key+'_max_abs'],
            last_window_max_ns=rounds[-1][key+'_max_abs'],
            signed_median_drift_ns_per_s=regression([(r['age_s'],r[key+'_median']) for r in rounds])[0],
            last10_signed_median_drift_ns_per_s=regression([(r['age_s'],r[key+'_median']) for r in rounds[-10:]])[0])
    summary = dict(schema='tilexr_frozen_clock_drift_validation.v1',model_state=state,
        requested_window_us=requested,actual_training_span_us=int(model['span_cycles'])*1e6/frequency,
        training_points=len(point_rows),trained_round=trained_round,
        fitted_rate_correction_ns_per_s=rate*1e9 if state == 2 else None,
        heldout_rounds=len(rounds),heldout_samples=len(rows),
        first_validation_age_s=min(r['age_since_training_s'] for r in rows),
        last_validation_age_s=max(r['age_since_training_s'] for r in rows),
        coefficients_refitted_during_validation=False,statistics=statistics,
        path_symmetry_verified=False,independently_measured_max_clock_skew_ns=None)
    write_csv(output/'residual_samples.csv',rows)
    write_csv(output/'rounds.csv',rounds)
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',required=True)
    parser.add_argument('--output',required=True)
    args = parser.parse_args()
    print(json.dumps(analyze(args.input,args.output),indent=2))
