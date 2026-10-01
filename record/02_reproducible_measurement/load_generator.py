#!/usr/bin/env python3
"""External canonical-wire load. Measures gateway acknowledgements, not fills."""
import csv
import hashlib
import importlib.util
import json
import os
import random
import re
import selectors
import signal
import socket
import struct
import subprocess
import time
from collections import deque
from pathlib import Path
from analyze import analyze

REQUEST = struct.Struct('!2sBBQQBIIQbqI')
RESPONSE = struct.Struct('!2sBBQQBIIQQbqIIBQq')
EPOCH = 0x880001
FIELDS = ['request_id', 'kind', 'scheduled_ns', 'first_write_ns', 'sent_ns', 'completed_ns', 'status']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def requests(count, clients, tickers, seed, initial_pair=0):
    if count % (2 * clients):
        raise ValueError('count must be divisible by 2*clients')
    rng = random.Random(seed)
    pairs = {}
    rows = []
    for index in range(count):
        client = 80 + index % clients
        step = index // clients
        order = initial_pair + step // 2 + 1
        kind = 1 if step % 2 == 0 else 2
        if kind == 1:
            pairs[client] = (rng.randrange(tickers), 100 + 256 * rng.randrange(3), 1)
        ticker, price, qty = pairs[client]
        rows.append({'client': client, 'order': order, 'ticker': ticker, 'price': price,
                     'qty': qty, 'kind': kind})
    return rows


class Peer:
    def __init__(self, sock, client):
        self.socket = sock
        self.client = client
        self.sequence = 0
        self.response_sequence = 0
        self.outgoing = deque()
        self.pending = deque()
        self.buffer = bytearray()


def drive(peers, inputs, mode, rate, timeout_s, pause_ms=0):
    """Retains short writes; open-loop schedule is independent of acknowledgements."""
    selector = selectors.DefaultSelector()
    for peer in peers.values():
        selector.register(peer.socket, selectors.EVENT_READ, peer)
    rows = [{'request_id': f"{r['client']}:{r['order']}:{r['kind']}",
             'kind': 'NEW' if r['kind'] == 1 else 'CANCEL', 'scheduled_ns': 0,
             'first_write_ns': '', 'sent_ns': '', 'completed_ns': '', 'status': 'UNSENT'} for r in inputs]
    start = time.perf_counter_ns() + 20_000_000
    for row in rows:
        row['scheduled_ns'] = start
    interval = 1e9 / rate if mode == 'open' else 0
    if mode == 'open':
        for i, row in enumerate(rows):
            row['scheduled_ns'] = start + int(i * interval)
    index = completed = 0
    next_pause = start + 100_000_000
    paused = False
    response_ids = set()
    sent_bytes = received_bytes = would_block = partial_writes = 0
    last_send = start
    deadline = start + int((len(inputs) / rate if mode == 'open' else timeout_s) * 1e9) + int(timeout_s * 1e9)
    try:
        while completed < len(inputs) and time.perf_counter_ns() < deadline:
            now = time.perf_counter_ns()
            if pause_ms and not paused and now >= next_pause:
                time.sleep(pause_ms / 1000)
                paused = True
                continue
            while index < len(inputs):
                row, source = rows[index], inputs[index]
                peer = peers[source['client']]
                if mode == 'open' and now < row['scheduled_ns']:
                    break
                if mode == 'closed' and (now < start or any(p.outgoing or p.pending for p in peers.values())):
                    break
                if mode == 'closed':
                    row['scheduled_ns'] = time.perf_counter_ns()
                peer.sequence += 1
                payload = REQUEST.pack(b'TS', 1, 1, peer.sequence, EPOCH, source['kind'],
                                       source['client'], source['ticker'], source['order'], 1,
                                       source['price'], source['qty'])
                peer.outgoing.append([payload, 0, row, source, peer.sequence])
                selector.modify(peer.socket, selectors.EVENT_READ | selectors.EVENT_WRITE, peer)
                index += 1
                if mode == 'closed':
                    break
            wait = .001
            if mode == 'open' and index < len(rows):
                wait = min(wait, max(0, (rows[index]['scheduled_ns'] - time.perf_counter_ns()) / 1e9))
            for key, events in selector.select(wait):
                peer = key.data
                if events & selectors.EVENT_WRITE:
                    # A bounded budget prevents one socket from starving others.
                    for _ in range(32):
                        if not peer.outgoing:
                            break
                        entry = peer.outgoing[0]
                        payload, offset, row, source, seq = entry
                        if not row['first_write_ns']:
                            row['first_write_ns'] = time.perf_counter_ns()
                        try:
                            written = peer.socket.send(payload[offset:])
                        except BlockingIOError:
                            would_block += 1
                            break
                        if not written:
                            raise RuntimeError('zero byte send')
                        sent_bytes += written
                        entry[1] += written
                        if entry[1] < len(payload):
                            partial_writes += 1
                            continue
                        last_send = row['sent_ns'] = time.perf_counter_ns()
                        row['status'] = 'TIMEOUT'  # Unknown until an observed terminal response.
                        peer.pending.append((row, source, seq))
                        peer.outgoing.popleft()
                    if not peer.outgoing:
                        selector.modify(peer.socket, selectors.EVENT_READ, peer)
                if events & selectors.EVENT_READ:
                    try:
                        data = peer.socket.recv(65536)
                    except BlockingIOError:
                        continue
                    if not data:
                        raise RuntimeError('unexpected peer EOF')
                    received_bytes += len(data)
                    peer.buffer.extend(data)
                    while len(peer.buffer) >= RESPONSE.size:
                        reply = RESPONSE.unpack(peer.buffer[:RESPONSE.size])
                        del peer.buffer[:RESPONSE.size]
                        if not peer.pending:
                            raise RuntimeError('duplicate or unsolicited response')
                        row, source, seq = peer.pending.popleft()
                        timestamp = time.perf_counter_ns()
                        peer.response_sequence += 1
                        if (reply[:3] != (b'TS', 1, 2) or reply[3] != peer.response_sequence or
                                reply[3] != seq or reply[4] != EPOCH or reply[6] != source['client'] or
                                reply[7] != source['ticker'] or reply[8] != source['order']):
                            raise RuntimeError(f'correlation mismatch: {reply}')
                        if reply[15] == 0 or reply[15] in response_ids or reply[16] != 0:
                            raise RuntimeError('response ID duplicate/absent or unexpected position')
                        response_ids.add(reply[15])
                        wanted = source['kind']
                        expected_exec = 0 if wanted == 1 else 0xFFFFFFFF  # CANCEL execution quantity is not applicable.
                        if reply[5] != wanted or reply[14] != 0 or reply[12] != expected_exec or reply[13] != source['qty']:
                            raise RuntimeError(f'unexpected business response: {reply}')
                        row['completed_ns'] = timestamp
                        row['status'] = 'ACCEPTED' if wanted == 1 else 'CANCELED'
                        completed += 1
        observed_end = time.perf_counter_ns()
        end = start + int(len(rows) * interval) if mode == 'open' else max(start + 1, observed_end)
        return rows, start, end, {'observed_end_ns': observed_end, 'last_full_frame_sent_ns': last_send,
                'generated': len(rows), 'sent': sum(bool(r['sent_ns']) for r in rows), 'completed': completed,
                'unsent': sum(r['status'] == 'UNSENT' for r in rows),
                'timeout': sum(r['status'] == 'TIMEOUT' for r in rows),
                'partial_writes': partial_writes, 'send_would_block': would_block,
                'unfinished_partial_frames': sum(bool(e[1]) for p in peers.values() for e in p.outgoing),
                'sent_bytes': sent_bytes, 'received_bytes': received_bytes,
                'observation_timeout_s': timeout_s, 'injected_generator_pause_ms': pause_ms,
                'unique_response_ids': len(response_ids)}
    except Exception as error:
        # Preserve every request collected before a failed correlation check.
        error.captured_rows = rows
        error.captured_start = start
        error.captured_end = (start + int(len(rows) * interval)) if mode == 'open' else max(start + 1, time.perf_counter_ns())
        raise
    finally:
        selector.close()


def counters():
    fields = Path('/proc/stat').read_text().splitlines()[0].split()[1:]
    text = Path('/proc/net/snmp').read_text().splitlines()
    udp = {}
    for i, line in enumerate(text[:-1]):
        if line.startswith('Udp:') and 'InDatagrams' in line:
            udp = dict(zip(line.split()[1:], map(int, text[i + 1].split()[1:])))
    return {'cpu_ticks': list(map(int, fields)), 'udp_system_counters': udp,
            'loadavg': Path('/proc/loadavg').read_text().strip()}


def resources(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().split()
    status = Path(f'/proc/{pid}/status').read_text()
    memory = {line.split(':')[0]: line.split(':')[1].strip() for line in status.splitlines()
              if line.startswith(('VmRSS:', 'VmHWM:', 'VmSize:'))}
    return {'cpu_ticks': int(fields[13]) + int(fields[14]), **memory}


def run(binary, directory, config, server_cpus, generator_cpus):
    directory.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, generator_cpus)
    probe = socket.socket()
    occupied = probe.connect_ex(('127.0.0.1', 12345)) == 0
    probe.close()
    if occupied:
        raise RuntimeError('port 12345 is occupied; existing process preserved')
    output = (directory / 'process_output.log').open('w')
    env = dict(os.environ, TRADE_SNAPSHOT_MS='60000')
    process = subprocess.Popen(['taskset', '-c', ','.join(map(str, server_cpus)), str(binary)],
                               cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT)
    peers = {}
    metadata = {'config': config, 'start_wall_ns': time.time_ns(), 'server_cpus': server_cpus,
                'generator_cpus': generator_cpus, 'binary_sha256': sha(binary)}
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('server exited during startup')
            candidate = socket.socket()
            candidate.settimeout(.2)
            try:
                candidate.connect(('127.0.0.1', 12345))
                peers[80] = Peer(candidate, 80)
                break
            except OSError:
                candidate.close()
                time.sleep(.05)
        if not peers:
            raise RuntimeError('server startup deadline exceeded')
        for cid in range(81, 80 + config['clients']):
            sock = socket.create_connection(('127.0.0.1', 12345), timeout=2)
            peers[cid] = Peer(sock, cid)
        for peer in peers.values():
            peer.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            peer.socket.setblocking(False)
        warm_count = 32 * config['clients']
        warmup = requests(warm_count, config['clients'], config['tickers'], config['seed'] + 1)
        _, _, _, warm_result = drive(peers, warmup, 'closed', 0, 5)
        if warm_result['completed'] != warm_count:
            raise RuntimeError('warmup did not complete')
        source = requests(config['requests'], config['clients'], config['tickers'], config['seed'], 16)
        (directory / 'input.json').write_text(json.dumps(source, sort_keys=True, separators=(',', ':')) + '\n')
        cpu_sample_start = time.perf_counter_ns()
        resource_before, before = resources(process.pid), counters()
        rows, start, end, counts = drive(peers, source, config['mode'], config['rate'],
                                        config.get('timeout_s', 5), config.get('pause_ms', 0))
        resource_after = resources(process.pid)
        cpu_sample_end = time.perf_counter_ns()
        after = counters()
        with (directory / 'events.csv').open('w', newline='') as handle:
            writer = csv.DictWriter(handle, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        metadata.update({'input_sha256': sha(directory / 'input.json'), 'events_sha256': sha(directory / 'events.csv'),
                         'warmup': warm_result, 'counts': counts, 'statistics': analyze(directory / 'events.csv', start, end),
                         'system_before': before, 'system_after': after,
                         'process_before': resource_before, 'process_after': resource_after,
                         'clock_ticks_per_s': os.sysconf('SC_CLK_TCK')})
        metadata['inflight_at_window_end'] = sum(bool(r['sent_ns']) and r['sent_ns'] < end and
                                                (not r['completed_ns'] or r['completed_ns'] >= end) for r in rows)
        metadata['generator_scheduled_not_sent_at_window_end'] = sum(r['scheduled_ns'] < end and
                                                                   (not r['sent_ns'] or r['sent_ns'] >= end) for r in rows)
        metadata['measurement_and_drain_seconds'] = (counts['observed_end_ns'] - start) / 1e9
        metadata['cpu_sample_start_ns'] = cpu_sample_start
        metadata['cpu_sample_end_ns'] = cpu_sample_end
        metadata['cpu_sample_seconds'] = (cpu_sample_end - cpu_sample_start) / 1e9
        metadata['process_cpu_seconds_sampled'] = (resource_after['cpu_ticks'] - resource_before['cpu_ticks']) / os.sysconf('SC_CLK_TCK')
        metadata['process_cpu_percent_sampled'] = 100 * metadata['process_cpu_seconds_sampled'] / metadata['cpu_sample_seconds']
        metadata['cpu_scope'] = 'all child threads; coarse /proc ticks; bracketed resource sampling includes presend wait and drain'
        metadata['filesystem'] = subprocess.check_output(['findmnt', '-T', str(directory), '-o', 'TARGET,SOURCE,FSTYPE,OPTIONS'], text=True)
        process.send_signal(signal.SIGTERM)
        metadata['exit_code'] = process.wait(timeout=15)
        spec = importlib.util.spec_from_file_location('reconcile', Path(__file__).parents[1] / '01_reliability/reconcile.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        ledger = module.reconcile(directory / 'exchange_orders.journal')
        metadata['reconciliation'] = ledger
        metadata['journal_sha256'] = sha(directory / 'exchange_orders.journal')
        logs = (directory / 'exchange_main.log').read_text()
        match = re.search(r'EXCHANGE STOPPED request_queue:(\d+) response_queue:(\d+) md_queue:(\d+) request_high:(\d+) response_high:(\d+) md_high:(\d+) audit_records:(\d+)', logs)
        metadata['queue_final_and_highwater'] = list(map(int, match.groups())) if match else None
        metadata['log_dropped_records'] = {p.name: int(m.group(1)) for p in directory.glob('*.log')
            if (m := re.search(r'\[LOGGER\] dropped_records=(\d+)', p.read_text()))}
        metadata['correctness_ok'] = (metadata['exit_code'] == 0 and ledger['consistent'] and
            not ledger['live_orders'] and not ledger['unapplied_requests'] and counts['sent'] == counts['completed'] == config['requests'] and
            ledger['received_count'] == ledger['applied_count'] == warm_count + config['requests'] and
            ledger['distinct_response_ids'] == warm_count + config['requests'] and
            metadata['queue_final_and_highwater'] is not None and metadata['queue_final_and_highwater'][:3] == [0, 0, 0])
    except Exception as error:
        metadata['failure'] = f'{type(error).__name__}: {error}'
        metadata['correctness_ok'] = False
        if hasattr(error, 'captured_rows'):
            with (directory / 'failed_events.csv').open('w', newline='') as handle:
                writer = csv.DictWriter(handle, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerows(error.captured_rows)
            metadata['failed_events_sha256'] = sha(directory / 'failed_events.csv')
            metadata['failed_statistics'] = analyze(directory / 'failed_events.csv', error.captured_start, error.captured_end)
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        metadata.setdefault('exit_code', process.returncode)
        for peer in peers.values():
            peer.socket.close()
        output.close()
        metadata['end_wall_ns'] = time.time_ns()
        (directory / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata
