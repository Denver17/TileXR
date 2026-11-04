import importlib.util
from pathlib import Path
from types import SimpleNamespace
import json
import os
import tempfile
import subprocess
import sys
import socket
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[2]/'tools/rdma_compare/compare.py'
spec = importlib.util.spec_from_file_location('rdma_compare', path)
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class ComparePlanTests(unittest.TestCase):
    def args(self, **changes):
        values = dict(devices='0,2', nics='nic0,nic1', tilexr='bench', kernel='kernel.o',
                      nda='ib_write_bw', count=512, direction='forward', address='cyclic',
                      window_start=None, window_end=None)
        values.update(changes)
        return SimpleNamespace(**values)

    def test_both_backends_share_payload_count(self):
        for payload in (512, 8192, 32768, 1048576):
            args = self.args()
            tile = compare.commands(args, payload, 'tilexr', 12345)
            nda = compare.commands(args, payload, 'nda', 12345)
            self.assertEqual(tile[0]['command'][3:5], [str(payload), '512'])
            for row in nda:
                cmd = row['command']
                self.assertEqual(cmd[cmd.index('-s')+1], str(payload))
                self.assertEqual(cmd[cmd.index('-n')+1], '512')
                self.assertNotIn('-a', cmd)

    def test_sender_mapping(self):
        for direction, client in [('forward', 0), ('reverse', 1), ('duplex', 0)]:
            rows = compare.commands(self.args(direction=direction), 512, 'nda', 12345)
            self.assertEqual(rows[0]['role'], 'server')
            self.assertEqual(rows[1]['rank'], client)
            self.assertEqual(rows[1]['command'][-1], '127.0.0.1')
            for row in rows:
                self.assertEqual('-b' in row['command'], direction == 'duplex')

    def test_device_validation(self):
        for devices in ('0,0', '0,16', '0', '0,2,4', 'zero,2'):
            with self.assertRaises(ValueError):
                compare.commands(self.args(devices=devices), 512, 'tilexr', 12345)

    def test_explicit_aware_window_required(self):
        for changes in ({}, dict(window_start='2026-11-04T03:00', window_end='2026-11-04T07:00'),
                        dict(window_start='2020-01-01T03:00+08:00', window_end='2020-01-01T07:00+08:00')):
            with self.assertRaises((ValueError, RuntimeError)):
                compare.window(self.args(**changes))


    def test_cpu_guard_cannot_be_removed_by_runner(self):
        with patch.dict(os.environ, RDMA_BENCH_CPU_ONLY='1'):
            with self.assertRaisesRegex(RuntimeError, 'CPU-only'):
                compare.execute_pair(self.args(), 512, 'tilexr', Path('unused'), 12345)

    def test_duplex_without_coordinator_is_rejected(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, 'coordinator'):
                compare.execute_pair(self.args(direction='duplex'), 512, 'tilexr', Path('unused'), 12345)

    def test_success_exit_does_not_replace_samples(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            rows = compare.commands(self.args(), 512, 'nda', 12345)
            for row in rows:
                (directory/f"rank{row['rank']}.log").write_text('old binary exited successfully\n')
            with self.assertRaisesRegex(RuntimeError, 'acknowledge'):
                compare.validate_pair(self.args(), 512, 'nda', rows, directory)

    def test_tilexr_per_sender_samples(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            rows = compare.commands(self.args(), 512, 'tilexr', 12345)
            for row in rows:
                active = row['rank'] == 0
                record = dict(rank=row['rank'], device=row['device'], bytes=512, count=512,
                              direction=0, cycles=32, correct=True, start_ticks=100 if active else 0,
                              end_ticks=8000100 if active else 0, gbps=0.262144 if active else 0)
                (directory/f"rank{row['rank']}.log").write_text('MATCHED_SAMPLE '+json.dumps(record)+'\n')
            samples = compare.validate_pair(self.args(), 512, 'tilexr', rows, directory)
            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]['rank'], 0)

    def test_nda_effective_configuration_and_timing(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            rows = compare.commands(self.args(), 512, 'nda', 12345)
            config = ('RDMA_BENCH_CONFIG version=1313095937 qp=1 count=512 bytes=512 address=cyclic '
                      'batch=64 signal=64 pipeline=2 no_wrap=1 warmup=0 stage=effective sq_depth=1024 cq_depth=1024\n')
            for row in rows:
                text = config
                if row['rank'] == 0:
                    text += ('RDMA_BENCH_SAMPLE scope=sender bytes=512 count=512 npu=0 '
                             'duration_ticks=8000000 gbps=0.262144 payload_verified=0\n')
                text += (f"RDMA_BENCH_VERIFY payload_verified=1 bytes=512 count=512 "
                         f"npu={row['device']} role={row['role']} impl=NDA direction=uni address=cyclic "
                         f"checked_bytes=524288 source_verified=1 destination_verified=1 guards_verified=1 "
                         f"received={int(row['role']=='server')} peer_verified=1 status=PASS\n")
                (directory/f"rank{row['rank']}.log").write_text(text)
            samples = compare.validate_pair(self.args(), 512, 'nda', rows, directory)
            self.assertTrue(samples[0]['payload_verified'])
            sender = directory/'rank0.log'
            good = sender.read_text()
            for field, wrong in [('status=PASS', 'status=FAIL'), ('guards_verified=1', 'guards_verified=0'),
                                 ('peer_verified=1', 'peer_verified=0'), ('checked_bytes=524288', 'checked_bytes=0'),
                                 ('address=cyclic checked_bytes', 'address=streaming checked_bytes')]:
                sender.write_text(good.replace(field, wrong))
                with self.assertRaisesRegex(RuntimeError, 'verification missing'):
                    compare.validate_pair(self.args(), 512, 'nda', rows, directory)
            sender.write_text(good)
            receiver = directory/'rank1.log'
            receiver.write_text(config)
            with self.assertRaisesRegex(RuntimeError, 'verification missing'):
                compare.validate_pair(self.args(), 512, 'nda', rows, directory)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux process socket ownership')
    def test_delayed_listener_is_observed_without_connecting(self):
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        code = ('import socket,time; time.sleep(0.15); s=socket.socket(); '
                f's.bind(("127.0.0.1",{port})); s.listen(1); time.sleep(4)')
        process = subprocess.Popen([sys.executable, '-c', code])
        try:
            compare.wait_for_listener(process, port, timeout=2)
            # The observer must leave the server's first accept available.
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                pass
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
