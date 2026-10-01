#!/usr/bin/env python3
"""Repeatable Release measurements. Raw files belong in an independent output directory."""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path
from load_generator import run, sha

ROOT = Path(__file__).resolve().parents[2]


def command(args):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    return {'command': args, 'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}


def manifest(build):
    env = [command(args) for args in [['uname', '-srmo'], ['g++', '--version'], ['cmake', '--version'],
           ['lscpu'], ['ip', '-details', 'link', 'show', 'lo'], ['sysctl', 'net.core.rmem_max',
           'net.core.wmem_max', 'net.ipv4.tcp_rmem', 'net.ipv4.tcp_wmem']]]
    tracked = subprocess.check_output(['git', 'ls-files'], cwd=ROOT, text=True).splitlines()
    production = {p: sha(ROOT / p) for p in tracked if p.startswith(('common/', 'exchange/', 'trading/')) and p.endswith(('.cpp', '.h'))}
    measurement = {str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'record/02_reproducible_measurement').iterdir()
                   if p.suffix in {'.py', '.h', '.cpp'}}
    measurement.update({p: sha(ROOT / p) for p in tracked if p.startswith('benchmarks/') and p.endswith('.cpp')})
    cache = (build / 'CMakeCache.txt').read_text()
    if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache or 'TRADE_SANITIZER:STRING=\n' not in cache:
        raise ValueError('require ordinary Release build without sanitizer')
    return {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            'dirty_tree': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)),
            'build_type': 'Release', 'build_directory': str(build),
            'cmake_command': f'cmake -S . -B {build} -G Ninja -DCMAKE_BUILD_TYPE=Release',
            'build_command': f'cmake --build {build} -j 4',
            'compile_commands_sha256': sha(build / 'compile_commands.json'),
            'environment': env, 'memory': Path('/proc/meminfo').read_text(),
            'numa_maps_visible': Path('/proc/self/numa_maps').read_text(),
            'hugepage_policy': Path('/sys/kernel/mm/transparent_hugepage/enabled').read_text(),
            'frequency_policy': {str(p): p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')},
            'host_or_vm': 'VM; host background load is not visible', 'vcpus': os.cpu_count(),
            'python': platform.python_version(), 'clock': vars(time.get_clock_info('perf_counter')),
            'production_sources': production, 'measurement_sources': measurement,
            'production_sources_sha256': hashlib.sha256(json.dumps(production, sort_keys=True).encode()).hexdigest(),
            'measurement_patch_sha256': hashlib.sha256(json.dumps(measurement, sort_keys=True).encode()).hexdigest(),
            'logging_mode': 'ordinary diagnostic logging and durable fsync journal unchanged',
            'socket_buffers': 'server Common::TCPBufferSize=1048576 bytes; external client OS defaults + TCP_NODELAY',
            'queue_capacity': 262144, 'network': 'same VM, loopback lo; no cross-host one-way timing',
            'predeclared_primary_metric': 'scheduled NEW/CANCEL -> matching ACK observed in external generator, P99',
            'stable_capacity_rule': 'each measured run correct; P99 <=20ms; >=98% of scheduled requests complete inside offered window',
            'quantiles': 'nearest rank; report median and range of per-run quantiles, never mean or pooled P99',
            'raw_data_policy': 'all warmup, failures and valid runs retained; no slow-sample filtering'}


def summary(results):
    groups = {}
    for row in results:
        if row['warmup']:
            continue
        groups.setdefault(row['profile'] + ':' + row['variant'], []).append(row)
    result = {}
    for name, rows in groups.items():
        metrics = {}
        for metric in ['p50_ns', 'p95_ns', 'p99_ns', 'max_ns']:
            values = [r['statistics']['scheduled_to_response'][metric] for r in rows
                      if 'statistics' in r and metric in r['statistics']['scheduled_to_response']]
            if values:
                metrics[metric] = {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
        rates = [r['statistics']['window_rates_per_s']['completed'] for r in rows if 'statistics' in r]
        stable = all(r.get('correctness_ok') and r['statistics']['all_offered_p99_ns'] is not None and
                     r['statistics']['all_offered_p99_ns'] <= 20_000_000 and
                     r['statistics']['window_counts']['completed'] >= .98 * r['config']['requests'] for r in rows)
        result[name] = {'runs': len(rows), 'all_correct': all(r.get('correctness_ok') for r in rows),
                        'latency_per_run': metrics,
                        'window_completed_per_s': {'median': statistics.median(rates), 'min': min(rates), 'max': max(rates)} if rates else {},
                        'stable_under_predeclared_budget': stable}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/reliability-release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--requests', type=int, default=1000)
    parser.add_argument('--tmpfs-control-output', type=Path,
                        help='optional new tmpfs directory for paired closed-loop storage controls')
    parser.add_argument('--diagnostic', action='store_true')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--candidate-binary', type=Path)
    parser.add_argument('--candidate-commit')
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents or output.exists():
        parser.error('output must be a new independent directory outside repository')
    if args.repeats < 5 and not args.smoke:
        parser.error('formal measurements require at least five repeats')
    if args.requests < 1000 and not args.smoke:
        parser.error('formal measurements require at least 1000 requests per run')
    data = manifest(build)
    if data['dirty_tree'] and not args.diagnostic:
        parser.error('dirty tree cannot be a formal baseline; commit changes first')
    output.mkdir(parents=True)
    storage_control = args.tmpfs_control_output.resolve() if args.tmpfs_control_output else None
    if storage_control:
        if storage_control.exists() or storage_control == ROOT or ROOT in storage_control.parents:
            parser.error('storage control must be a new directory outside repository')
        storage_control.mkdir(parents=True)
        filesystem = subprocess.check_output(['findmnt', '-n', '-T', str(storage_control), '-o', 'FSTYPE'], text=True).strip()
        if filesystem != 'tmpfs':
            parser.error('storage control must reside on tmpfs')
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 5:
        parser.error('need at least five available vCPUs for declared placement')
    server_cpus, generator_cpus = cpus[:4], [cpus[4]]
    binaries = {'baseline': build / 'exchange_main'}
    if args.candidate_binary:
        if not args.candidate_commit:
            parser.error('candidate binary requires source commit identifier')
        binaries['candidate'] = args.candidate_binary.resolve()
    data.update({'start_wall_ns': time.time_ns(), 'repeats': args.repeats, 'process_warmups': 1,
                 'load_seed': 20260928, 'requests_per_run': args.requests,
                 'server_cpus': server_cpus, 'generator_cpus': generator_cpus,
                 'binaries': {k: {'path': str(p), 'sha256': sha(p)} for k, p in binaries.items()},
                 'candidate_commit': args.candidate_commit,
                 'result_filesystem': command(['findmnt', '-T', str(output), '-o', 'TARGET,SOURCE,FSTYPE,OPTIONS']),
                 'tmpfs_storage_control': str(storage_control) if storage_control else None,
                 'comparison_class': 'B1/B2 candidate experiment' if args.candidate_binary else 'B1 measurement reference; no engine optimization candidate'})
    (output / 'manifest.json').write_text(json.dumps(data, indent=2) + '\n')
    (output / 'compile_commands.json').write_bytes((build / 'compile_commands.json').read_bytes())
    (output / 'source.diff').write_text(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=ROOT, text=True))
    with (output / 'environment.txt').open('w') as handle:
        for entry in data['environment']:
            handle.write('$ ' + ' '.join(entry['command']) + '\n' + entry['stdout'] + entry['stderr'] + '\n')
        handle.write(data['memory'])
    probes = ['release_benchmark', 'logger_benchmark', 'hash_benchmark', 'measurement_component_benchmark']
    micro = []
    for iteration in range(1 + (1 if args.smoke else args.repeats)):
        for probe in probes:
            directory = output / 'components' / f'{iteration}_{probe}'
            directory.mkdir(parents=True)
            cmd = ['taskset', '-c', ','.join(map(str, cpus[:2])), str(build / probe)]
            if iteration % 2 and probe != 'measurement_component_benchmark':
                cmd.append('--reverse')
            env = dict(os.environ, TRADE_PRODUCER_CPU=str(cpus[0]), TRADE_CONSUMER_CPU=str(cpus[1]))
            result = subprocess.run(cmd, cwd=directory, env=env, capture_output=True, text=True, timeout=90)
            (directory / 'stdout.txt').write_text(result.stdout)
            (directory / 'stderr.txt').write_text(result.stderr)
            item = {'iteration': iteration, 'warmup': iteration == 0, 'command': cmd,
                    'exit_code': result.returncode, 'binary_sha256': sha(build / probe),
                    'statistics': [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')],
                    'directory': str(directory)}
            micro.append(item)
            (output / 'components.json').write_text(json.dumps(micro, indent=2) + '\n')
            print('component', iteration, probe, result.returncode, flush=True)
            if result.returncode:
                raise RuntimeError('component failed; failed artifacts preserved')
    profiles = [(f'open_c{c}_t{t}_r100', {'mode': 'open', 'clients': c, 'tickers': t, 'rate': 100})
                for c in [1, 2, 4] for t in [1, 8]]
    profiles += [('closed_c1_t1', {'mode': 'closed', 'clients': 1, 'tickers': 1, 'rate': 0})]
    profiles += [(f'capacity_c4_t8_r{rate}', {'mode': 'open', 'clients': 4, 'tickers': 8, 'rate': rate})
                 for rate in [50, 250, 1000]]
    if args.smoke:
        profiles = [profiles[0], profiles[5], profiles[6]]
    data['profiles'] = profiles
    (output / 'manifest.json').write_text(json.dumps(data, indent=2) + '\n')
    results = []
    # Interleave configurations and alternate baseline/candidate order each round.
    for iteration in range(1 + (1 if args.smoke else args.repeats)):
        for name, profile in (profiles if iteration % 2 == 0 else list(reversed(profiles))):
            variants = list(binaries) if iteration % 2 == 0 else list(reversed(binaries))
            if storage_control and name == 'closed_c1_t1':
                variants = (['baseline', 'tmpfs_storage_control'] if iteration % 2 == 0
                            else ['tmpfs_storage_control', 'baseline'])
            for variant in variants:
                config = dict(profile, requests=args.requests, seed=20260928, timeout_s=30)
                directory = ((storage_control if variant == 'tmpfs_storage_control' else output) /
                             'network' / f'{iteration}_{name}_{variant}')
                os.sched_setaffinity(0, cpus)
                item = run(binaries['baseline' if variant == 'tmpfs_storage_control' else variant], directory, config, server_cpus, generator_cpus)
                item.update({'profile': name, 'variant': variant, 'iteration': iteration,
                             'warmup': iteration == 0, 'directory': str(directory)})
                results.append(item)
                (output / 'network.json').write_text(json.dumps(results, indent=2) + '\n')
                print('network', iteration, name, variant, item['correctness_ok'],
                      item.get('statistics', {}).get('scheduled_to_response'), item.get('failure', ''), flush=True)
                if not item['correctness_ok']:
                    raise RuntimeError('network correctness failure; complete available artifacts preserved')
    os.sched_setaffinity(0, cpus)
    (output / 'summary.json').write_text(json.dumps(summary(results), indent=2) + '\n')
    hashes = {str(p.relative_to(output)): sha(p) for p in output.rglob('*') if p.is_file()}
    if storage_control:
        hashes.update({str(p): sha(p) for p in storage_control.rglob('*') if p.is_file()})
    (output / 'artifact_hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print('complete', output, flush=True)


if __name__ == '__main__':
    main()
