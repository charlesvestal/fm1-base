#!/usr/bin/env python3
# Copyright (c) 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT OR MPL-2.0
"""Offline bench protocol/counter fixtures. Opens no radio, console or device."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
(ROOT/'build').mkdir(exist_ok=True)   # scratch space (git-ignored)
spec = importlib.util.spec_from_file_location('bench', ROOT/'tools/ble_conn_update_bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class Radio:
    def sendall(self, payload):
        assert payload == bytes.fromhex('01 13 20 0e 00 08 06 00 06 00 00 00 f4 01 00 00 00 00')


class Fixtures(unittest.TestCase):
    def test_hci(self):
        events = [(15, bytes.fromhex('00 01 13 20')), (62, bytes.fromhex('03 00 00 08 06 00 00 00 f4 01'))]
        with patch.object(bench.select, 'select', return_value=([], [], [])), \
                patch.object(bench, 'next_event', side_effect=events):
            bench.update(Radio(), 0x800, 6)
        with patch.object(bench.select, 'select', return_value=([], [], [])), \
                patch.object(bench, 'next_event', return_value=(15, bytes.fromhex('0c 01 13 20'))):
            with self.assertRaisesRegex(RuntimeError, 'Command Status 0c'):
                bench.update(Radio(), 0x800, 6)

    def test_counter_measurement(self):
        a = dict(link_epoch=2, conn_interval_1250us=12, conn_updates=1, pending_kind=0,
                 conn_event=65500, conn_events=65500, supervision_fail=0, late_instants=0,
                 link_state_idle_created_up_closing_stopped=2, enc_enabled=1)
        b = dict(a, conn_event=(65500+1000) & 65535, conn_events=66500)
        class Console:
            def sample(self):
                return 25, b
        with patch.object(bench, 'stable', return_value=(10, a)), patch.object(bench.time, 'sleep'):
            self.assertEqual(bench.measure(Console(), 12, 15, 'fixture wrap')['measured_ms'], 15)
            for change in (dict(conn_event=65500), dict(conn_event=5000), dict(link_epoch=4),
                           dict(pending_kind=1), dict(enc_enabled=0)):
                saved = dict(b)
                b.update(change)
                with self.assertRaises(RuntimeError):
                    bench.measure(Console(), 12, 15, 'fixture must fail')
                b.clear()
                b.update(saved)

    def test_partial_results_after_usb_error(self):
        import json
        import sys
        from unittest.mock import MagicMock
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as directory:
            output=Path(directory)/'bench'
            argv=['bench','--address','00:11:22:33:44:55','--usb-port','fake',
                  '--output',str(output),'--soak-seconds','0']
            sample=(0,dict(conn_interval_1250us=12,conn_updates=0,link_epoch=1))
            with patch.object(sys,'argv',argv), patch.object(bench.socket,'socket'), \
                 patch.object(bench.subprocess,'Popen',return_value=MagicMock(poll=lambda: None)), \
                 patch.object(bench,'Console'), patch.object(bench,'bluez'), \
                 patch.object(bench,'connect',return_value=1), patch.object(bench,'update'), \
                 patch.object(bench,'stable',return_value=sample), patch.object(bench.time,'sleep'), \
                 patch.object(bench,'measure',side_effect=[dict(label='initial',measured_ms=15),
                     RuntimeError('USB log incomplete (missing end)')]):
                with self.assertRaisesRegex(RuntimeError,'USB log incomplete'):
                    bench.main()
            self.assertEqual(json.loads((output/'measurements.json').read_text()),
                             [dict(label='initial',measured_ms=15)])
            status=json.loads((output/'status.json').read_text())
            self.assertFalse(status['completed'])
            self.assertIn('missing end',status['error']['detail'])

    def test_console_parser(self):
        raw = ('log: conn_event 65535\nlog: link_epoch 2\nlog: conn_interval_1250us 12\n'
               'log: conn_events 90\nlog: enc_enabled 1\n'
               'log: worker_timer_last_ticks 0xF403A1CD\nlog: pending_proc kind 0 instant 0\n')
        values = bench.parse_counters(raw)
        self.assertEqual(values['conn_event'], 65535)
        self.assertEqual(values['worker_timer_last_ticks'], 0xF403A1CD)
        self.assertEqual(values['pending_kind'], 0)
        with self.assertRaisesRegex(RuntimeError, 'incomplete'):
            bench.parse_counters('conn_event 0\n')
        class Remote:
            def read_log(self, compact):
                assert compact
                return raw
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as directory:
            console = bench.Console.__new__(bench.Console)
            console.remote, console.directory, console.sequence, console.full = Remote(), Path(directory), 0, False
            with patch.object(bench.time, 'monotonic', side_effect=(1, 1.1)):
                stamp, values = console.sample()
            self.assertAlmostEqual(stamp, 1.05)
            self.assertTrue((Path(directory)/'usb-0001.txt').is_file())


if __name__ == '__main__':
    unittest.main()
