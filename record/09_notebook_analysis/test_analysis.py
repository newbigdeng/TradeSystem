"""用反例验证 notebook 的单位、完整样本与数据验收边界。"""
import csv
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'notebooks'))
import perf_analysis as analysis


class NotebookAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def csv(self, name, fields, rows):
        path = self.root / name
        with path.open('w', encoding='utf-8', newline='') as handle:
            writer = csv.writer(handle)
            writer.writerow(fields)
            writer.writerows(rows)
        return path

    def traces(self):
        path = self.root / 'exchange.log'
        path.write_text('12:00:00.123456789 RDTSC work 10\n' * 2 +
                        '12:00:00.123456790 RDTSC work 1000000\n'
                        '12:00:00.123456791 TTT T1 1000000000000000001\n'
                        '12:00:00.123456792 TTT T2 1000000000000000101\n', encoding='utf-8')
        return path

    def requests(self, rows=None):
        return self.csv('events.csv', ['request_id', 'kind', 'scheduled_ns', 'sent_ns',
                                       'completed_ns', 'status'], rows or [
            ['a', 'NEW', 110, 111, 120, 'ACCEPTED'],
            ['b', 'NEW', 130, 131, '', 'TIMEOUT'],
            ['c', 'NEW', 150, '', '', 'UNSENT']])

    def test_no_data_and_trace_disabled_are_not_zero_latency(self):
        self.assertEqual(analysis.analyze_directory()['components'], [])
        (self.root / 'exchange.log').write_text('EXCHANGE STOPPED request_queue:0\n')
        data = analysis.analyze_directory(self.root)
        self.assertEqual(data['rdtsc'], [])
        self.assertTrue(any('关闭诊断' in item for item in data['issues']))

    def test_duplicate_and_slow_trace_records_are_retained(self):
        self.traces()
        row = analysis.analyze_directory(self.root)['rdtsc'][0]
        self.assertEqual((row['n'], row['p50'], row['p99'], row['max']), (3, 10, 1000000, 1000000))
        self.assertEqual(row['unit'], 'TSC ticks')

    def test_tsc_conversion_requires_explicit_frequency(self):
        self.traces()
        row = analysis.analyze_directory(self.root, tsc_hz=2e9)['rdtsc'][0]
        self.assertEqual(row['p50'], 5)
        self.assertEqual(row['unit'], 'ns (calibrated TSC)')

    def test_invalid_tsc_frequency_is_rejected(self):
        for value in [0, -1, float('nan'), float('inf'), True, '2e9']:
            with self.subTest(value=value), self.assertRaises(ValueError):
                analysis.analyze_directory(self.root, tsc_hz=value)

    def test_ttt_is_coverage_only_and_preserves_integer_precision(self):
        self.traces()
        data = analysis.analyze_directory(self.root)
        self.assertEqual(len(data['ttt']), 2)
        self.assertEqual(data['ttt'][0]['min_epoch_ns'], 1000000000000000001)
        self.assertNotIn('p50', data['ttt'][0])
        self.assertFalse(any('T1' in key or 'T2' in key for key in data['series']))

    def test_invalid_trace_line_is_reported(self):
        (self.root / 'exchange.log').write_text('12:00:00 RDTSC work -7\n')
        data = analysis.analyze_directory(self.root)
        self.assertEqual(data['rdtsc'], [])
        self.assertTrue(any('exchange.log:1' in issue for issue in data['issues']))

    def test_component_full_tail_and_zero_are_retained(self):
        self.csv('calls.csv', ['operation', 'elapsed_ns'], [[0, 0], [1, 1], [2, 1000000]])
        row = analysis.analyze_directory(self.root)['components'][0]
        self.assertEqual((row['n'], row['p50'], row['p99'], row['max']), (3, 1, 1000000, 1000000))
        self.assertEqual(row['unit'], 'ns/sample')

    def test_pool_samples_are_normalized_by_each_cycles_pairs(self):
        self.csv('pool.csv', ['cycle', 'pairs', 'elapsed_ns'], [[0, 8192, 16384], [1, 1, 100]])
        row = analysis.analyze_directory(self.root)['components'][0]
        self.assertEqual((row['mean'], row['p50'], row['p99']), (51, 2, 100))
        self.assertEqual(row['unit'], 'ns/pair (cycle mean)')

    def test_component_metadata_mismatch_is_reported(self):
        self.csv('calls.csv', ['operation', 'elapsed_ns'], [[0, 1], [1, 100]])
        (self.root / 'result.txt').write_text(json.dumps({'name': 'calls', 'operations': 2,
            'p50_ns': 1, 'p95_ns': 100, 'p99_ns': 2, 'max_ns': 100, 'errors': 0}))
        data = analysis.analyze_directory(self.root)
        self.assertEqual(data['components'], [])
        self.assertTrue(any('不一致' in issue for issue in data['issues']))

    def test_duplicate_operation_and_wrong_row_length_are_rejected(self):
        for rows in [[[0, 1], [0, 2]], [[0, 1, 'extra']], [[0, -1]]]:
            with self.subTest(rows=rows):
                self.csv('calls.csv', ['operation', 'elapsed_ns'], rows)
                data = analysis.analyze_directory(self.root)
                self.assertEqual(data['components'], [])
                self.assertTrue(data['issues'])

    def test_censored_requests_have_no_all_offered_p99(self):
        path = self.requests()
        saved = analysis._analysis.analyze(path, 100, 200)
        (self.root / 'result.json').write_text(json.dumps({'statistics': saved, 'correctness_ok': False}))
        data = analysis.analyze_directory(self.root)
        row = data['requests'][0]
        self.assertEqual((row['offered'], row['completed_rtt_n'], row['timeouts'], row['unsent']), (3, 1, 1, 1))
        self.assertIsNone(row['all_offered_scheduled_p99_ns'])
        self.assertEqual(row['correctness'], 'failed')

    def test_request_window_and_acceptance_are_not_inferred(self):
        self.requests()
        self.assertEqual(analysis.analyze_directory(self.root)['requests'], [])
        row = analysis.analyze_directory(self.root, request_window_ns=(100, 200))['requests'][0]
        self.assertEqual(row['correctness'], 'not provided')

    def test_warmup_is_not_merged_into_formal_window(self):
        self.requests()
        self.csv('warmup_events.csv', ['request_id', 'kind', 'scheduled_ns', 'sent_ns',
                                      'completed_ns', 'status'], [['w', 'NEW', 105, 106, 107, 'ACCEPTED']])
        data = analysis.analyze_directory(self.root, request_window_ns=(100, 200))
        self.assertEqual(len(data['requests']), 1)
        self.assertEqual(data['requests'][0]['offered'], 3)
        self.assertTrue(any('预热请求' in issue for issue in data['issues']))

    def test_duplicate_request_id_is_not_silently_deduplicated(self):
        self.requests([['a', 'NEW', 110, 111, 120, 'ACCEPTED']] * 2)
        data = analysis.analyze_directory(self.root, request_window_ns=(100, 200))
        self.assertEqual(data['requests'], [])
        self.assertTrue(any('duplicate request' in issue for issue in data['issues']))

    def test_request_metadata_mismatch_is_not_passed(self):
        path = self.requests()
        saved = analysis._analysis.analyze(path, 100, 200)
        saved['total_rows'] = 999
        (self.root / 'result.json').write_text(json.dumps({'statistics': saved, 'correctness_ok': True}))
        data = analysis.analyze_directory(self.root)
        self.assertEqual(data['requests'], [])
        self.assertTrue(any('不能沿用' in issue for issue in data['issues']))

    def test_unsupported_schema_is_not_assumed_to_be_nanoseconds(self):
        self.csv('unknown.csv', ['latency', 'unit'], [[1, 'us']])
        data = analysis.analyze_directory(self.root)
        self.assertEqual(data['components'], [])
        self.assertTrue(any('不支持的 CSV' in issue for issue in data['issues']))


if __name__ == '__main__':
    unittest.main()
