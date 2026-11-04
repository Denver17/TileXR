#!/usr/bin/env python3
"""Plan matching QP1 WRITE cases; execution requires a time window and fresh idle checks."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def commands(args, payload, backend, port):
    devices, nics = args.devices.split(','), args.nics.split(',')
    if len(devices) != 2 or len(nics) != 2 or len(set(devices)) != 2:
        raise ValueError('exactly two distinct devices and two NIC entries are required')
    if any(not x.isdecimal() or not 0 <= int(x) < 16 for x in devices):
        raise ValueError('devices must be physical IDs in [0,15]')
    direction = {'forward': 0, 'reverse': 1, 'duplex': 2}[args.direction]
    if backend == 'tilexr':
        return [dict(rank=rank, device=int(device), nic=nic,
                     command=[args.tilexr, str(rank), device, str(payload), str(args.count),
                              str(direction), str(int(args.address == 'streaming')), args.kernel, '--run'])
                for rank, (device, nic) in enumerate(zip(devices, nics))]
    client = 1 if args.direction == 'reverse' else 0
    result = []
    for rank in (1-client, client):  # listener first
        command = [args.nda, '-d', nics[rank], f'--use_cann={devices[rank]}', '-q', '1',
                   '-s', str(payload), '-n', str(args.count), '-m', '4096', '-c', 'RC',
                   '-x', '1', '-I', '0', '-F', '-N', '--report_gbits',
                   '--use_qubuf_dma_mode=INDEP_UB', '--use_direct_async', '-p', str(port)]
        if args.direction == 'duplex':
            command.append('-b')
        if rank == client:
            command.append('127.0.0.1')
        result.append(dict(rank=rank, device=int(devices[rank]), nic=nics[rank],
                           role='client' if rank == client else 'server', command=command))
    return result


def case(args, payload):
    return dict(bytes=payload, count=args.count, qp=1, sge=1, address=args.address,
                direction=args.direction, post_batch=64, signal_every=64, max_in_flight=128,
                no_wrap=True, fresh_qp_per_sample=True, mtu=4096, gid_index=1,
                dma='INDEP_UB', doorbell='hard', timer='kernel_cycles_1GHz_calibrated',
                statistics='per_direction_first; duplex_sum_separate')


def validate_pair(args, payload, backend, rows, directory):
    samples = []
    active = {0, 1} if args.direction == 'duplex' else {int(args.direction == 'reverse')}
    cycles = (32 if payload <= 8192 else 16 if payload <= 16384 else 8 if payload <= 32768
              else 4 if payload <= 65536 else 2 if payload <= 131072 else 1) if args.address == 'cyclic' else 0
    for row in rows:
        lines = (directory/f"rank{row['rank']}.log").read_text().splitlines()
        if backend == 'tilexr':
            records = [json.loads(line.split(' ', 1)[1]) for line in lines if line.startswith('MATCHED_SAMPLE ')]
            if len(records) != 1:
                raise RuntimeError('missing or duplicate TileXR kernel sample')
            record = records[0]
            if (record.get('rank'), record.get('device'), record.get('bytes'), record.get('count'),
                record.get('direction'), record.get('cycles'), record.get('correct')) != (
                    row['rank'], row['device'], payload, args.count,
                    {'forward': 0, 'reverse': 1, 'duplex': 2}[args.direction], cycles, True):
                raise RuntimeError('TileXR sample differs from requested case')
            if row['rank'] not in active:
                if record.get('start_ticks') or record.get('end_ticks') or record.get('gbps'):
                    raise RuntimeError('unidirectional receiver unexpectedly sent data')
                continue
            duration = record['end_ticks']-record['start_ticks']
        else:
            configs = [dict(item.split('=', 1) for item in line.split()[1:]) for line in lines
                       if line.startswith('RDMA_BENCH_CONFIG ')]
            expected = dict(version=str(0x4E444101), qp='1', count=str(args.count), bytes=str(payload), address=args.address,
                            batch='64', signal='64', pipeline='2', no_wrap='1', warmup='0')
            if not any(all(config.get(k) == value for k, value in expected.items()) for config in configs):
                raise RuntimeError('NDA binary did not acknowledge matching configuration')
            verified = [dict(item.split('=', 1) for item in line.split()[1:]) for line in lines
                        if line.startswith('RDMA_BENCH_VERIFY ')]
            span = (args.count-1)*(payload+64)+payload if args.address == 'streaming' else (cycles or 1)*payload
            half = (span+262143)//262144*262144
            if half-span < 64:
                half += 262144
            identity = dict(payload_verified='1', bytes=str(payload), count=str(args.count),
                            npu=str(row['device']), role=row['role'], impl='NDA',
                            direction='duplex' if args.direction == 'duplex' else 'uni',
                            address=args.address, checked_bytes=str(2*half), source_verified='1',
                            destination_verified='1', guards_verified='1', peer_verified='1', status='PASS',
                            received=str(int(row['role'] == 'server' or args.direction == 'duplex')))
            if len(verified) != 1 or not all(verified[0].get(k) == value for k, value in identity.items()):
                raise RuntimeError('NDA remote payload and guard verification missing or mismatched')
            if row['rank'] not in active:
                continue
            effective = [config for config in configs if config.get('stage') == 'effective']
            if not effective or int(effective[-1].get('sq_depth', 0)) <= args.count:
                raise RuntimeError('NDA actual queue admission missing')
            records = [dict(item.split('=', 1) for item in line.split()[1:]) for line in lines
                       if line.startswith('RDMA_BENCH_SAMPLE ')]
            if len(records) != 1:
                raise RuntimeError('missing or duplicate per-sender NDA kernel sample')
            record = records[0]
            if (record.get('scope'), record.get('bytes'), record.get('count'), record.get('npu')) != (
                    'sender', str(payload), str(args.count), str(row['device'])):
                raise RuntimeError('NDA sender sample differs from requested case')
            duration = int(record['duration_ticks'])
        gbps = float(record['gbps'])
        expected_gbps = payload*args.count*8/duration if duration > 0 else 0
        if duration <= 0 or not math.isfinite(gbps) or gbps <= 0 or abs(gbps-expected_gbps) > expected_gbps*1e-6:
            raise RuntimeError('invalid bandwidth or inconsistent kernel timing')
        samples.append(dict(rank=row['rank'], device=row['device'], duration_ns=duration,
                            gbps=gbps, payload_verified=True))
    if {sample['rank'] for sample in samples} != active:
        raise RuntimeError('missing active sender sample')
    return samples


def window(args):
    if not args.window_start or not args.window_end:
        raise ValueError('execution requires --window-start and --window-end with timezone offsets')
    start, end = (datetime.fromisoformat(x) for x in (args.window_start, args.window_end))
    if start.tzinfo is None or end.tzinfo is None or start >= end:
        raise ValueError('invalid aware time window')
    now = datetime.now(timezone.utc)
    if now < start or (end-now).total_seconds() < 150:
        raise RuntimeError('outside execution window or insufficient time for bounded cleanup')
    return end


def check_idle(args, directory):
    if not args.idle_check_script or not args.inventory or not args.guard_devices:
        raise ValueError('execution requires an occupancy checker, local inventory and guard device set')
    inventory = json.loads(Path(args.inventory).read_text())
    environments = inventory.get('environments', [])
    required = {int(x) for x in args.guard_devices.split(',')}
    if len(environments) != 1 or environments[0].get('ssh') != 'local':
        raise ValueError('the on-server runner requires one local occupancy target')
    configured = set(environments[0].get('devices', inventory.get('defaults', {}).get('devices', [])))
    if configured != required or not {int(x) for x in args.devices.split(',')} <= required:
        raise ValueError('occupancy inventory must cover exactly the authorized guard devices')
    check = subprocess.run([sys.executable, args.idle_check_script, args.inventory, '--pretty'],
                           capture_output=True, text=True, timeout=40)
    (directory/'occupancy.json').write_text(check.stdout)
    (directory/'occupancy.stderr').write_text(check.stderr)
    if check.returncode:
        raise RuntimeError('devices busy or occupancy indeterminate; no benchmark launched')
    report = json.loads(check.stdout)
    observed = {d['id']: d['state'] for e in report['environments'] for d in e['devices']}
    if not report.get('safe_to_use') or set(observed) != required or any(x != 'idle' for x in observed.values()):
        raise RuntimeError('occupancy checker did not establish all requested devices idle')


def wait_for_listener(process, port, timeout=30):
    """Observe our server's listening socket without consuming its one accept."""
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('NDA listener exited before opening its control socket')
        sockets = set()
        for name in ('tcp', 'tcp6'):
            for line in Path('/proc/net', name).read_text().splitlines()[1:]:
                fields = line.split()
                if fields[3] == '0A' and int(fields[1].rsplit(':', 1)[1], 16) == port:
                    sockets.add('socket:['+fields[9]+']')
        for fd in Path('/proc', str(process.pid), 'fd').iterdir():
            try:
                if os.readlink(fd) in sockets:
                    return
            except FileNotFoundError:
                pass
        time.sleep(0.02)
    raise RuntimeError('NDA listener did not open its control socket before deadline')


def execute_pair(args, payload, backend, directory, port):
    if 'RDMA_BENCH_CPU_ONLY' in os.environ:
        raise RuntimeError('CPU-only environment forbids all benchmark execution')
    if args.direction == 'duplex':
        raise RuntimeError('duplex execution requires a qualified device start coordinator; plan only')
    window(args)
    directory.mkdir(parents=True)
    check_idle(args, directory)
    window(args)  # occupancy probing can consume the remaining time window
    rows = commands(args, payload, backend, port)
    processes, logs = [], []
    env = os.environ.copy()
    env.pop('ASCEND_RT_VISIBLE_DEVICES', None)
    env.update(RDMA_BENCH_COUNT=str(args.count), RDMA_BENCH_ADDRESS_MODE=args.address,
               RDMA_BENCH_NO_WRAP='1')
    if backend == 'tilexr':
        env.update(TILEXR_ENABLE_RDMA='1', TILEXR_ENABLE_IPC='0', TILEXR_ENABLE_CREDIT_IPC='0',
                   TILEXR_ENABLE_UDMA='0', TILEXR_ENABLE_SDMA='0', TILEXR_RDMA_MODE='aiv',
                   TILEXR_RDMA_DMA_MODE='indep_ub', TILEXR_RDMA_DB_MODE='hard', TILEXR_RDMA_MTU='4096',
                   TILEXR_RDMA_PORT='1', TILEXR_RDMA_GID_INDEX='1', TILEXR_RDMA_HOP_LIMIT='64',
                   TILEXR_RDMA_TIMEOUT_MS='20000', TILEXR_COMM_ID=f'127.0.0.1:{port}')
    started = time.monotonic()
    accepted = False
    samples = []
    try:
        for row in rows:
            child_env = dict(env, TILEXR_RDMA_DEVICE=row['nic'])
            log = open(directory/f"rank{row['rank']}.log", 'w')
            logs.append(log)
            process = subprocess.Popen(row['command'], env=child_env, stdout=log,
                                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                       start_new_session=True)
            processes.append(process)
            if backend == 'nda' and row['role'] == 'server':
                wait_for_listener(process, port)
        for process, row in zip(processes, rows):
            row['exit'] = process.wait(timeout=max(1, 125-(time.monotonic()-started)))
        if any(row['exit'] for row in rows):
            raise RuntimeError('benchmark failed; inspect both rank logs')
        for log in logs:
            log.flush()
        samples = validate_pair(args, payload, backend, rows, directory)
        accepted = True
    finally:
        for process in processes:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        for log in logs:
            log.close()
        (directory/'result.json').write_text(json.dumps(dict(backend=backend, case=case(args, payload),
            accepted=accepted, samples=samples, ranks=rows, elapsed_seconds=time.monotonic()-started), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tilexr', required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--nda', required=True)
    parser.add_argument('--devices', required=True)
    parser.add_argument('--nics', required=True)
    parser.add_argument('--direction', choices=['forward', 'reverse', 'duplex'], default='forward')
    parser.add_argument('--address', choices=['cyclic', 'streaming'], default='cyclic')
    parser.add_argument('--count', type=int, default=512)
    parser.add_argument('--payload', choices=['all', '1048576'], default='1048576')
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--warmups', type=int, default=0)
    parser.add_argument('--port', type=int, default=48720)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--window-start')
    parser.add_argument('--window-end')
    parser.add_argument('--idle-check-script')
    parser.add_argument('--inventory')
    parser.add_argument('--guard-devices')
    args = parser.parse_args()
    if not 1 <= args.count <= 1000 or not 1 <= args.samples <= 20 or not 0 <= args.warmups <= 5:
        parser.error('count must be 1..1000; samples 1..20; warmups 0..5')
    payloads = [2**n for n in range(9, 21)] if args.payload == 'all' else [1048576]
    args.output.mkdir(parents=True, exist_ok=False)
    plan = [dict(case=case(args, payload), tilexr=commands(args, payload, 'tilexr', args.port),
                 nda=commands(args, payload, 'nda', args.port)) for payload in payloads]
    hashes = {path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
              for path in (args.tilexr, args.kernel, args.nda) if Path(path).is_file()}
    (args.output/'plan.json').write_text(json.dumps(dict(cases=plan, binary_sha256=hashes,
        execution_requested=args.execute, samples=args.samples, warmups=args.warmups,
        window_start=args.window_start, window_end=args.window_end), indent=2))
    if not args.execute:
        print(f'CPU plan PASS: {len(plan)} matching cases, no subprocess or NPU calls')
        return
    if os.name != 'posix':
        raise ValueError('execute this runner on the Linux test host')
    for payload in payloads:
        for sample in range(-args.warmups, args.samples):
            backends = ['tilexr', 'nda'] if sample % 2 == 0 else ['nda', 'tilexr']
            for backend in backends:
                execute_pair(args, payload, backend, args.output/f'{payload}-sample{sample}-{backend}', args.port)


if __name__ == '__main__':
    main()
