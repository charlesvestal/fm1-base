#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
"""OWNER ONLY: BlueZ encrypted BLE-MIDI soak; never run by builds/tests.

Already pair/bond the FM-1 in BlueZ and enable its ALSA BLE-MIDI port. Needs
CAP_NET_RAW/root, bluetoothctl, btmon, aplaymidi, mido and python-rtmidi.
Select the USB and BLE ALSA ports explicitly; close other USB log clients.
No flashing, bond removal, host reset, or firmware button presses.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import time

from ble_conn_update_bench import Console, bluez, connect, next_event, stable, update

INTERVALS = (12, 24, 9, 12)  # 15 / 30 / 11.25 / 15 ms, HCI units


def write_notes(path):
    # SMF format 0, default tempo 500000 us/quarter, 96 ticks/quarter.
    track = bytes.fromhex('00 90 3c 50 30 80 3c 00 00 ff 2f 00')
    path.write_bytes(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 96) +
                     b'MTrk' + struct.pack('>I', len(track)) + track)


def disconnected(code, payload, handle):
    return (code == 5 and len(payload) >= 4 and not payload[0] and
            struct.unpack_from('<H', payload, 1)[0] == handle)


class Run:
    def __init__(self, log, seconds):
        self.log, self.start = log, time.monotonic()
        self.end = self.start + seconds
        self.drops = self.connections = self.attempts = self.notes = self.updates = self.errors = 0
        self.first = self.last = None
        self.epochs = set()
        self.boots = 0
        self.completed = False
        self.deltas = {}

    def record(self, event, **fields):
        self.log.write(json.dumps(dict(event=event, utc=datetime.now(timezone.utc).isoformat(),
                                       elapsed=time.monotonic()-self.start, **fields)) + '\n')
        self.log.flush()

    def sample(self, console):
        stamp, counters = console.sample()
        self.record('sample', monotonic=stamp, counters=counters)
        if self.last and counters['sample_ms'] < self.last['sample_ms']:
            self.boots += 1
        if self.last:
            reboot = counters['sample_ms'] < self.last['sample_ms']
            for key in ('midi_packets_in', 'midi_packets_out', 'mic_failures', 'supervision_fail',
                        'late_instants', 'audio_late', 'audio_xruns', 'worker_timer_over_7500us',
                        'nimble_resets', 'midi_drop'):
                if key in counters and key in self.last:
                    increment = counters[key] if reboot else (counters[key]-self.last[key]) & 0xffffffff
                    self.deltas[key] = self.deltas.get(key, 0) + increment
        self.epochs.add((self.boots, counters['link_epoch']))
        if self.first is None:
            self.first = counters
        self.last = counters
        return counters

    def summary(self):
        return dict(completed=self.completed, elapsed_seconds=time.monotonic()-self.start,
                    drops=self.drops, reconnects=max(0,self.connections-1), successful_connections=self.connections, reconnect_attempts=self.attempts,
                    notes_sent=self.notes, updates=self.updates, errors=self.errors,
                    observed_epochs=len(self.epochs), observed_reboots=self.boots,
                    counter_deltas=self.deltas, counters_span_reboot=bool(self.boots), last=self.last)


def soak(run, radio, console, address, midi_port, notes):
    handle = None
    next_note, next_sample, next_update = run.start, run.start, run.start+600
    interval_index = 0
    while time.monotonic() < run.end:
        now = time.monotonic()
        if handle is None:
            run.attempts += 1
            try:
                bluez(address, False)  # clear a stale BlueZ object before reconnecting
                while select.select([radio], [], [], 0)[0]:
                    radio.recv(1024)
                handle = connect(radio, address)
                _, counters = stable(console)
                if not counters.get('nimble_bonded') or not counters.get('nimble_subscribed'):
                    raise RuntimeError('link must be bonded, encrypted and MIDI-subscribed; pair in BlueZ first')
                run.connections += 1
                run.record('connected', handle=handle, epoch=counters['link_epoch'])
                run.sample(console)
                next_sample = time.monotonic()+60
            except (RuntimeError, TimeoutError, OSError, subprocess.SubprocessError) as error:
                handle = None
                run.errors += 1
                run.record('reconnect_failed', detail=str(error))
                time.sleep(min(5, max(0, run.end-time.monotonic())))
            continue
        try:
            # Consume disconnection/encryption events continuously, not just at
            # the 60-second samples. No polling of USB at MIDI-note frequency.
            while select.select([radio], [], [], 0)[0]:
                code, p = next_event(radio, time.monotonic()+0.2)
                if disconnected(code, p, handle):
                    raise ConnectionError(f'HCI disconnect reason {p[3]:02x}')
                if code == 8 and len(p) >= 4 and struct.unpack_from('<H', p, 1)[0] == handle:
                    if p[0] or not p[3]:
                        raise ConnectionError('encryption lost')
            if now >= next_sample:
                c = run.sample(console)
                next_sample = time.monotonic()+60
                if c['link_state_idle_created_up_closing_stopped'] != 2 or not c['enc_enabled']:
                    raise ConnectionError('USB reports link down or unencrypted')
            if now >= next_update:
                interval = INTERVALS[interval_index % len(INTERVALS)]
                update(radio, handle, interval)
                stable(console, interval)
                run.updates += 1
                run.record('update', interval_ms=interval*1.25)
                interval_index += 1
                next_update += 600
                # Recovery never produces a burst of overdue update requests.
                if next_update <= time.monotonic():
                    next_update = time.monotonic()+600
            if now >= next_note:
                result = subprocess.run(['aplaymidi', '-p', midi_port, str(notes)],
                                        capture_output=True, text=True, timeout=10)
                if result.returncode:
                    raise RuntimeError('aplaymidi: '+(result.stderr or result.stdout).strip())
                run.notes += 1
                next_note = time.monotonic()+5
        except (ConnectionError, RuntimeError, TimeoutError, OSError, subprocess.SubprocessError) as error:
            run.errors += 1
            # A failed command is not automatically a link drop. Inspect the
            # firmware, and count a drop only for down/encryption/epoch changes.
            drop = isinstance(error, ConnectionError) or 'disconnected during update' in str(error)
            try:
                previous = run.last
                c = run.sample(console)
                drop |= (c['link_state_idle_created_up_closing_stopped'] != 2 or not c['enc_enabled'] or
                         previous is not None and c['link_epoch'] != previous['link_epoch'])
            except (RuntimeError, OSError) as usb_error:
                run.record('usb_error', detail=str(usb_error))
            run.record('error', detail=str(error), link_drop=drop)
            if drop:
                run.drops += 1
                handle = None
            else:
                next_update = time.monotonic()+600
                next_note = time.monotonic()+5
        time.sleep(0.1)
    run.completed = True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--address', required=True)
    p.add_argument('--usb-port', required=True, help='exact USB-MIDI input/output name')
    p.add_argument('--midi-port', required=True, help='ALSA BLE-MIDI destination from aplaymidi -l')
    p.add_argument('--index', type=int, default=0)
    p.add_argument('--hours', type=float, default=12)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if not 0 < args.hours <= 168:
        p.error('--hours must be in (0, 168]')
    args.output.mkdir(parents=True, exist_ok=False)
    notes = args.output/'notes.mid'
    write_notes(notes)
    console = monitor = None
    with (args.output/'samples.jsonl').open('w') as log, (args.output/'btmon.txt').open('w') as btlog:
        run = Run(log, args.hours*3600)
        run.record('start', address=args.address, hours=args.hours, intervals_ms=[i*1.25 for i in INTERVALS])
        try:
            with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as radio:
                radio.bind((args.index,))
                radio.setsockopt(0, 2, struct.pack('<IIIH2x', 1 << 4, 0xffffffff, 0xffffffff, 0))
                monitor = subprocess.Popen(['btmon', '-i', f'hci{args.index}', '-w',
                                            str(args.output/'capture.btsnoop')], stdout=btlog, stderr=subprocess.STDOUT)
                time.sleep(0.3)
                if monitor.poll() is not None:
                    raise RuntimeError("btmon failed; inspect btmon.txt")
                console = Console(args.usb_port, args.output)
                soak(run, radio, console, args.address, args.midi_port, notes)
        except KeyboardInterrupt:
            run.record('interrupted')
        except (RuntimeError, OSError, subprocess.SubprocessError) as error:
            run.errors += 1
            run.record('fatal', detail=str(error))
        finally:
            if console:
                try:
                    run.sample(console)
                except (RuntimeError, OSError) as error:
                    run.record('final_sample_failed', detail=str(error))
                console.close()
            if monitor:
                monitor.send_signal(signal.SIGINT)
                monitor.wait(timeout=10)
            summary = run.summary()
            run.record('summary', **summary)
            (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
            print(json.dumps(summary, indent=2))
    return 0 if run.completed and not run.errors else 1


if __name__ == '__main__':
    raise SystemExit(main())
