#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
"""Offline only: fake time, HCI, USB and subprocesses; no device enumeration."""
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import ble_overnight_soak as soak


class Clock:
    value = 0
    def monotonic(self): return self.value
    def sleep(self, seconds): self.value += seconds


class Fixtures(unittest.TestCase):
    def exercise(self, seconds, drop=False, update_error=False):
        clock, epoch, delivered = Clock(), [0], [False]
        class Console:
            def sample(self):
                return clock.value, dict(sample_ms=int(clock.value*1000), link_epoch=epoch[0],
                    link_state_idle_created_up_closing_stopped=2, enc_enabled=1,
                    midi_packets_in=int(clock.value), nimble_bonded=1, nimble_subscribed=1)
        console = Console()
        def connect(*args): epoch[0] += 1; return 0x50
        def select(*args):
            if drop and clock.value >= 1 and not delivered[0]: return ([radio], [], [])
            return ([], [], [])
        def event(*args): delivered[0] = True; return (5, bytes.fromhex('00 50 00 08'))
        intervals = []
        def update(radio, handle, interval):
            intervals.append(interval)
            if update_error and len(intervals) == 1: raise RuntimeError('central rejected update')
        radio = object()
        with patch.object(soak.time, 'monotonic', clock.monotonic), patch.object(soak.time, 'sleep', clock.sleep), \
             patch.object(soak.select, 'select', select), patch.object(soak, 'next_event', event), \
             patch.object(soak, 'bluez'), patch.object(soak, 'connect', connect), \
             patch.object(soak, 'stable', side_effect=lambda *a: console.sample()), \
             patch.object(soak, 'update', update), \
             patch.object(soak.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0)) as midi:
            run = soak.Run(io.StringIO(), seconds)
            soak.soak(run, radio, console, '00:11:22:33:44:55', '42:0', Path('unused.mid'))
            result = run.summary()
            self.assertTrue(result['completed'])
            self.assertGreater(midi.call_count, 0)
            self.assertEqual(midi.call_args.args[0][:3], ['aplaymidi', '-p', '42:0'])
            return result, intervals

    def test_schedule(self):
        result, intervals = self.exercise(2401)
        self.assertEqual(intervals, [12, 24, 9, 12])
        self.assertGreater(result['notes_sent'], 450)
        self.assertEqual(result['drops'], 0)
        self.assertEqual(result['reconnects'], 0)

    def test_drop_once_and_recover(self):
        result, _ = self.exercise(8, drop=True)
        self.assertEqual(result['drops'], 1)
        self.assertEqual(result['reconnects'], 1)
        self.assertEqual(result['observed_epochs'], 2)

    def test_rejected_update_is_not_a_drop(self):
        result, intervals = self.exercise(1201, update_error=True)
        self.assertEqual(result['drops'], 0)
        self.assertEqual(result['errors'], 1)
        self.assertEqual(intervals, [12, 12])

    def test_notes_and_usb_completion(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'notes.mid'; soak.write_notes(p)
            self.assertTrue(p.read_bytes().startswith(b'MThd\0\0\0\6'))
        # Import with a stub mido even on machines with no MIDI dependencies.
        with patch.dict(sys.modules, {'mido': types.ModuleType('mido')}):
            from fm1_remote import FM1, LG
            remote = FM1.__new__(FM1)
            remote.inp = types.SimpleNamespace(iter_pending=lambda: iter(()))
            sent = []; remote.sysex = sent.append
            remote.frames = lambda *a, **k: [b'enc_enabled 1', b'end']
            self.assertEqual(remote.read_log(True), 'enc_enabled 1\n')
            self.assertEqual(sent, [LG+[1]])
            with patch.object(remote, 'frames', side_effect=[
                    [b'incomplete'], [], [b'enc_enabled 1', b'end']]) as frames, \
                 patch('fm1_remote.time.sleep') as sleep:
                self.assertEqual(remote.read_log(True), 'enc_enabled 1\n')
                self.assertEqual(frames.call_count, 3)
                self.assertEqual(sleep.call_count, 2)
            with patch.object(remote, 'frames', return_value=[b'incomplete']) as frames, \
                 patch('fm1_remote.time.sleep'):
                with self.assertRaisesRegex(RuntimeError, 'missing end.*3 attempts'):
                    remote.read_log(True)
                self.assertEqual(frames.call_count, 3)


if __name__ == '__main__': unittest.main()
