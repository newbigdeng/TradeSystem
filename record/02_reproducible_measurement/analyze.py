#!/usr/bin/env python3
"""Strict, dependency-free analysis of one monotonic-clock request cohort."""
import argparse
import csv
import json
import math
from collections import Counter
from pathlib import Path

STATUSES = {'ACCEPTED', 'CANCELED', 'REJECTED', 'TIMEOUT', 'UNSENT'}


def distribution(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return {'n': len(values), **{name: values[math.ceil(p * len(values)) - 1]
                               for name, p in [('p50_ns', .5), ('p95_ns', .95), ('p99_ns', .99)]},
            'max_ns': values[-1], 'mean_ns': sum(values) / len(values)}


def analyze(path, start_ns, end_ns):
    if not 0 <= start_ns < end_ns:
        raise ValueError('invalid measurement window')
    cohort = Counter({status: 0 for status in STATUSES})
    windows = Counter(offered=0, sent=0, completed=0, accepted=0, canceled=0, rejected=0)
    delays, rtt, late, seen = [], [], [], set()
    kinds = {}
    with Path(path).open(newline='', encoding='utf-8') as handle:
        for row in csv.DictReader(handle):
            rid = row['request_id']
            if not rid or rid in seen:
                raise ValueError('empty or duplicate request ID')
            seen.add(rid)
            scheduled = int(row['scheduled_ns'])
            sent = int(row['sent_ns']) if row['sent_ns'] else None
            completed = int(row['completed_ns']) if row['completed_ns'] else None
            status = row['status']
            if scheduled < 0 or status not in STATUSES:
                raise ValueError('invalid schedule or status')
            if sent is not None and sent < scheduled:
                raise ValueError('send before schedule')
            if status == 'UNSENT' and (sent is not None or completed is not None):
                raise ValueError('UNSENT has timestamps')
            if status == 'TIMEOUT' and (sent is None or completed is not None):
                raise ValueError('TIMEOUT must have send and no response')
            if status in {'ACCEPTED', 'CANCELED', 'REJECTED'} and (
                    sent is None or completed is None or completed < sent):
                raise ValueError('response lacks valid timestamps')
            if sent is not None and start_ns <= sent < end_ns:
                windows['sent'] += 1
            if completed is not None and start_ns <= completed < end_ns:
                windows['completed'] += 1
                windows[status.lower()] += 1
            if not start_ns <= scheduled < end_ns:
                continue
            windows['offered'] += 1
            cohort[status] += 1
            if sent is not None:
                late.append(sent - scheduled)
            if completed is not None:
                delays.append(completed - scheduled)
                rtt.append(completed - sent)
                kinds.setdefault(row['kind'], []).append(completed - scheduled)
    seconds = (end_ns - start_ns) / 1e9
    censored = cohort['TIMEOUT'] + cohort['UNSENT'] > 0
    return {'window_start_ns': start_ns, 'window_end_ns': end_ns, 'window_seconds': seconds,
            'total_rows': len(seen), 'cohort': dict(sorted(cohort.items())),
            'conservation_ok': sum(cohort.values()) == windows['offered'],
            'window_counts': dict(windows),
            'window_rates_per_s': {k: v / seconds for k, v in windows.items()},
            'send_lateness': distribution(late), 'actual_send_to_response': distribution(rtt),
            'scheduled_to_response': distribution(delays),
            'scheduled_to_response_by_kind': {k: distribution(v) for k, v in kinds.items()},
            'percentile_algorithm': 'nearest rank: sorted[ceil(p*n)-1]',
            'latency_scope': 'completed responses only',
            'all_offered_p99_ns': None if censored else distribution(delays).get('p99_ns'),
            'all_offered_p99_status': 'right-censored or unsent; not exactly estimable' if censored else 'observed'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--start-ns', type=int, required=True)
    parser.add_argument('--end-ns', type=int, required=True)
    args = parser.parse_args()
    print(json.dumps(analyze(args.csv, args.start_ns, args.end_ns), indent=2))
