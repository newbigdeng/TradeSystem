#!/usr/bin/env python3
"""Paired live verification of bracketed CPU sampling, with the same fixed input."""
import argparse
import json
import os
import subprocess
from pathlib import Path
from load_generator import run
from run_measurements import manifest, summary, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tmpfs-output', type=Path, required=True)
    args = parser.parse_args()
    build = ROOT / 'build/reliability-release'
    data = manifest(build)
    if data['dirty_tree']:
        parser.error('require committed sources')
    for path in [args.output, args.tmpfs_output]:
        path = path.resolve()
        if path.exists() or path == ROOT or ROOT in path.parents:
            parser.error('require new output directories outside repository')
        path.mkdir(parents=True)
    if subprocess.check_output(['findmnt', '-n', '-T', str(args.tmpfs_output), '-o', 'FSTYPE'], text=True).strip() != 'tmpfs':
        parser.error('tmpfs control must reside on tmpfs')
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 5:
        parser.error('need five available CPUs')
    data['purpose'] = 'resource sampling boundary correction; same business binary, input and latency endpoints'
    (args.output / 'manifest.json').write_text(json.dumps(data, indent=2) + '\n')
    results = []
    for iteration in range(6):
        variants = ['ext4', 'tmpfs'] if iteration % 2 == 0 else ['tmpfs', 'ext4']
        for variant in variants:
            os.sched_setaffinity(0, cpus)
            directory = (args.output if variant == 'ext4' else args.tmpfs_output) / f'{iteration}_{variant}'
            item = run(build / 'exchange_main', directory,
                       {'mode': 'closed', 'clients': 1, 'tickers': 1, 'rate': 0,
                        'requests': 1000, 'seed': 20260928, 'timeout_s': 30}, cpus[:4], [cpus[4]])
            item.update(iteration=iteration, warmup=iteration == 0, variant=variant,
                        profile='closed_c1_t1', directory=str(directory))
            results.append(item)
            (args.output / 'runs.json').write_text(json.dumps(results, indent=2) + '\n')
            if not item['correctness_ok']:
                raise RuntimeError('business reconciliation failed; data preserved')
            if not item['cpu_sample_start_ns'] <= item['statistics']['window_start_ns'] <= item['counts']['observed_end_ns'] <= item['cpu_sample_end_ns']:
                raise RuntimeError('CPU interval does not bracket complete measurement')
            if abs(item['process_cpu_percent_sampled'] * item['cpu_sample_seconds'] / 100 -
                   item['process_cpu_seconds_sampled']) > 1e-9:
                raise RuntimeError('CPU calculation differs')
            print(iteration, variant, item['cpu_sample_seconds'], item['process_cpu_percent_sampled'],
                  item['statistics']['scheduled_to_response']['p99_ns'], flush=True)
    os.sched_setaffinity(0, cpus)
    for iteration in range(6):
        pair = [r for r in results if r['iteration'] == iteration]
        if pair[0]['input_sha256'] != pair[1]['input_sha256'] or pair[0]['binary_sha256'] != pair[1]['binary_sha256']:
            raise RuntimeError('paired source workload differs')
    (args.output / 'summary.json').write_text(json.dumps(summary(results), indent=2) + '\n')


if __name__ == '__main__':
    main()
