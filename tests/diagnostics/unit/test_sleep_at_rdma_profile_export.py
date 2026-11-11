"""功能：验证导出器保留不同 device epoch 下的入口偏差，并拒绝与 records 不符的模型。
调用关系：使用 profiling records fixture 调用导出器；不依赖 CANN 或启动 NPU。
资源与生命周期：每个测试在 temporary directory 中生成输入，退出上下文时删除。
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools/profiling/python'))
sys.path.insert(0, str(ROOT/'tests/profiling/python'))
from _fixtures import CAPTURE_ID, record_block, records_bytes

spec = importlib.util.spec_from_file_location('sleep_at_export', ROOT/'tests/diagnostics/export_sleep_at_rdma_profile.py')
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class SleepAtExportTest(unittest.TestCase):
    def fixture(self, directory):
        target = 2**56
        for rank in (0, 1):
            epoch = rank * 1000000
            begin = target + epoch + 1000 + rank*500
            model = {'schema': 'tilexr_sleep_at_rdma_profile.v1', 'rank': rank,
                     'device': rank*2, 'state': 2, 'capture_id': CAPTURE_ID,
                     'wqes': 64, 'bytes_per_wqe': 4096, 'markers': 2,
                     'data_and_guards_verified': True, 'producer_before': 30,
                     'producer_after': 94, 'completed': 94,
                     'target': target, 'raw_entry': begin, 'raw_exit': begin+20000,
                     'local_deadline': target+epoch, 'anchor_cycle': begin,
                     'offset_cycles': -epoch, 'rate_correction': 0,
                     'corrected_entry': begin-epoch, 'corrected_exit': begin+20000-epoch,
                     'cycles_per_second': 1000000000, 'model_generation': 6,
                     'clock_model_bound_ns': 7200}
            (directory/f'sleep_at_rdma_rank{rank}.json').write_text(json.dumps(model))
            (directory/f'sleep_at_rdma_rank{rank}.records.bin').write_bytes(
                records_bytes([record_block(rank << 16, records=[(begin, 1), (begin+20000, 2)])]))

    def test_keeps_nonzero_skew_in_common_target_coordinates(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            trace, summary = exporter.export_trace(directory)
            events = [event for event in trace['traceEvents'] if event['ph'] == 'X']
            self.assertEqual([event['ts'] for event in events], [1.0, 1.5])
            self.assertEqual([event['dur'] for event in events], [20.0, 20.0])
            self.assertEqual(summary['entry_skew_ns'], 500)
            self.assertEqual(events[1]['args']['raw_start'] - events[0]['args']['raw_start'], 1000500)
            self.assertEqual(trace['metadata']['display_origin'], 'shared_sleep_at_reference_target')

    def test_rejects_sidecar_from_a_different_model(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            path = directory/'sleep_at_rdma_rank1.json'
            model = json.loads(path.read_text())
            model['offset_cycles'] = 0
            path.write_text(json.dumps(model))
            with self.assertRaisesRegex(ValueError, 'frozen model'):
                exporter.export_trace(directory)


if __name__ == '__main__':
    unittest.main()
