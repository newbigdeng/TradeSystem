#!/usr/bin/env python3
"""Real socketpair tests: fixed inputs, fragmented responses and short writes."""
import socket
import threading
import unittest
from load_generator import Peer, requests, drive, REQUEST, RESPONSE


class ShortWriter:
    def __init__(self, sock):
        self.sock = sock

    def fileno(self):
        return self.sock.fileno()

    def send(self, data):
        return self.sock.send(data[:3])

    def recv(self, size):
        return self.sock.recv(size)


class GeneratorTests(unittest.TestCase):
    def test_deterministic_input_and_cancel_identity(self):
        first = requests(64, 4, 8, 12)
        self.assertEqual(first, requests(64, 4, 8, 12))
        self.assertNotEqual(first, requests(64, 4, 8, 13))
        for i in range(0, 64, 8):
            for j in range(4):
                a, b = first[i + j], first[i + 4 + j]
                self.assertEqual({k: v for k, v in a.items() if k != 'kind'},
                                 {k: v for k, v in b.items() if k != 'kind'})

    def execute(self, mode, count=48, rate=1000, pause_ms=0):
        client, server = socket.socketpair()
        client.setblocking(False)
        failures = []

        def respond():
            try:
                data = b''
                for _ in range(count):
                    while len(data) < REQUEST.size:
                        chunk = server.recv(1024)
                        if not chunk:
                            raise RuntimeError('early close')
                        data += chunk
                    r = REQUEST.unpack(data[:REQUEST.size])
                    data = data[REQUEST.size:]
                    reply = RESPONSE.pack(b'TS', 1, 2, r[3], r[4], r[5], r[6], r[7], r[8],
                                          r[8], 1, r[10], 0 if r[5] == 1 else 0xFFFFFFFF, r[11], 0, r[3], 0)
                    for offset in range(0, len(reply), 7):
                        server.sendall(reply[offset:offset + 7])
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=respond)
        worker.start()
        try:
            rows, start, end, stats = drive({80: Peer(ShortWriter(client), 80)},
                                          requests(count, 1, 1, 9), mode, rate, 3, pause_ms)
            self.assertEqual(stats['completed'], count)
            self.assertEqual(stats['sent_bytes'], count * REQUEST.size)
            self.assertEqual(stats['received_bytes'], count * RESPONSE.size)
            self.assertGreater(stats['partial_writes'], count)
            self.assertTrue(all(r['completed_ns'] >= r['sent_ns'] >= r['scheduled_ns'] for r in rows))
            self.assertLess(start, end)
            if pause_ms:
                self.assertGreater(max(r['sent_ns'] - r['scheduled_ns'] for r in rows), 5_000_000)
        finally:
            client.close()
            worker.join(timeout=5)
            server.close()
        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])

    def test_open_loop_partial_io(self):
        self.execute('open')

    def test_closed_loop_partial_io(self):
        self.execute('closed')

    def test_generator_pause_is_observed(self):
        self.execute('open', count=120, rate=500, pause_ms=20)

    def test_timeouts_are_not_synthetic_completions(self):
        client, server = socket.socketpair()
        client.setblocking(False)
        try:
            rows, _, _, stats = drive({80: Peer(client, 80)}, requests(2, 1, 1, 9), 'open', 1000, .02)
            self.assertEqual(stats['timeout'], 2)
            self.assertTrue(all(r['status'] == 'TIMEOUT' and not r['completed_ns'] for r in rows))
        finally:
            client.close()
            server.close()


if __name__ == '__main__':
    unittest.main()
