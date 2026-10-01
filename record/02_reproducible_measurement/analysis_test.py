#!/usr/bin/env python3
"""Counterexamples for quantiles, censoring, cohorts and input validation."""
import csv
import tempfile
import unittest
from pathlib import Path
from analyze import analyze, distribution


class AnalysisTests(unittest.TestCase):
    def sample(self, rows, start=100, end=200):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'events.csv'
            with path.open('w', newline='') as handle:
                writer = csv.writer(handle)
                writer.writerow(['request_id', 'kind', 'scheduled_ns', 'sent_ns', 'completed_ns', 'status'])
                writer.writerows(rows)
            return analyze(path, start, end)

    def test_nearest_rank_and_empty(self):
        self.assertEqual(distribution(range(1, 101))['p99_ns'], 99)
        self.assertEqual(distribution([1, 2, 100])['p99_ns'], 100)
        self.assertEqual(distribution([]), {'n': 0})

    def test_censor_and_conservation(self):
        result = self.sample([['a', 'NEW', 110, 111, 120, 'ACCEPTED'],
                              ['b', 'NEW', 130, 131, '', 'TIMEOUT'],
                              ['c', 'NEW', 150, '', '', 'UNSENT']])
        self.assertEqual(result['window_counts']['offered'], 3)
        self.assertTrue(result['conservation_ok'])
        self.assertIsNone(result['all_offered_p99_ns'])
        self.assertEqual(result['scheduled_to_response']['n'], 1)

    def test_cohort_and_window_are_different(self):
        result = self.sample([['a', 'NEW', 90, 99, 105, 'ACCEPTED'],
                              ['b', 'CANCEL', 110, 111, 210, 'CANCELED'],
                              ['c', 'NEW', 199, 200, 211, 'REJECTED']])
        self.assertEqual(result['window_counts']['completed'], 1)
        self.assertEqual(result['window_counts']['sent'], 1)
        self.assertEqual(result['window_counts']['offered'], 2)
        self.assertEqual(result['scheduled_to_response']['n'], 2)

    def test_duplicates_and_invalid_timestamps(self):
        with self.assertRaises(ValueError):
            self.sample([['a', 'NEW', 110, 111, 120, 'ACCEPTED']] * 2)
        for row in [['a', 'NEW', 110, 109, 120, 'ACCEPTED'],
                    ['a', 'NEW', 110, 111, '', 'ACCEPTED'],
                    ['a', 'NEW', 110, '', '', 'TIMEOUT'],
                    ['a', 'NEW', 110, 111, 112, 'TIMEOUT'],
                    ['a', 'NEW', 110, 111, '', 'UNSENT']]:
            with self.assertRaises(ValueError):
                self.sample([row])

    def test_slow_sample_and_rejection_are_retained(self):
        result = self.sample([['a', 'NEW', 110, 111, 1000000, 'ACCEPTED'],
                              ['b', 'NEW', 120, 121, 122, 'REJECTED']])
        self.assertEqual(result['scheduled_to_response']['max_ns'], 999890)
        self.assertEqual(result['cohort']['REJECTED'], 1)


if __name__ == '__main__':
    unittest.main()
