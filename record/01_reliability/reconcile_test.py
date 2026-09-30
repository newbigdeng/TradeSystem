#!/usr/bin/env python3
"""Incomplete processing, missing fills and position drift must not pass audit."""
import tempfile,unittest
from pathlib import Path
from reconcile import reconcile

VALID='''TS_AUDIT_V1
RECEIVED 1 10 1 1 0 1 1 100 3
APPLY 1 10 1 1 0 1 1 100 3
RESPONSE 1 1 0 1 1 1 1 100 0 3 0 0
RECEIVED 2 20 1 1 0 2 -1 100 3
APPLY 2 20 1 1 0 2 -1 100 3
RESPONSE 2 2 0 2 2 1 -1 100 0 3 0 0
RESPONSE 3 2 0 2 2 3 -1 100 3 0 0 -3
RESPONSE 4 1 0 1 1 3 1 100 3 0 0 3
'''

class AuditTest(unittest.TestCase):
    def result(self,text):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'exchange.journal';path.write_text(text)
            return reconcile(path)
    def test_valid(self):
        r=self.result(VALID);self.assertTrue(r['consistent']);self.assertFalse(r['live_orders'])
    def test_received_without_processing(self):
        self.assertFalse(self.result(VALID+'RECEIVED 1 10 2 3 0 1 1 100 0\n')['consistent'])
    def test_processing_without_confirmation(self):
        text=VALID+'RECEIVED 1 10 2 3 0 1 1 100 0\nAPPLY 1 10 2 3 0 1 1 100 0\n'
        self.assertFalse(self.result(text)['consistent'])
    def test_missing_execution_side(self):
        self.assertFalse(self.result(VALID.rsplit('RESPONSE 4',1)[0])['consistent'])
    def test_duplicate_execution(self):
        self.assertFalse(self.result(VALID+'RESPONSE 4 1 0 1 1 3 1 100 3 0 0 3\n')['consistent'])
    def test_position_drift(self):
        self.assertFalse(self.result(VALID.replace('0 -3','0 -2'))['consistent'])
    def test_truncated_record(self):
        with self.assertRaises(ValueError): self.result(VALID.rstrip())

if __name__=='__main__':unittest.main()
