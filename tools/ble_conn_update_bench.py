#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
"""OWNER ONLY: Linux desktop single-update bench; never invoked by builds/tests.
Requires root/CAP_NET_RAW, btmon, bluetoothctl, an already bonded FM-1, and its
USB-MIDI log port with other USB log clients closed (mido + python-rtmidi). Does not flash or remove bonds.
Example: sudo python3 tools/ble_conn_update_bench.py --address FD:60:52:C0:DF:6C \
    --usb-port "TEMPLATE FM-1:TEMPLATE FM-1 MIDI 1 24:0" --output build/ble-bench
"""
import argparse
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import time


def parse_counters(text):
    text = re.sub(r'^log: ?', '', text, flags=re.M)
    values = {k: int(v, 0) for k, v in re.findall(
        r'^([a-zA-Z0-9_]+) (-?(?:0x[0-9A-Fa-f]+|[0-9]+))\r?$', text, re.M)}
    pending = re.search(r'^pending_proc kind (\d+)', text, re.M)
    if pending:
        values['pending_kind'] = int(pending[1])
    required = ('pending_kind', 'link_epoch', 'conn_interval_1250us', 'enc_enabled', 'conn_events')
    if any(k not in values for k in required):
        raise RuntimeError('incomplete BLE counters; install round-2 open firmware and close other USB clients')
    return values


class Console:
    """USB-MIDI transport. The retained name keeps existing measurement callers simple."""
    def __init__(self, port, directory, full=False):
        from fm1_remote import FM1
        self.remote = FM1(port)
        self.directory, self.sequence, self.full = directory, 0, full

    def close(self):
        self.remote.close()

    def sample(self):
        start = time.monotonic()
        text = self.remote.read_log(compact=not self.full)
        stop = time.monotonic()
        self.sequence += 1
        (self.directory / f'usb-{self.sequence:04d}.txt').write_text(
            f'monotonic_start {start:.9f}\nmonotonic_end {stop:.9f}\n{text}')
        return (start + stop) / 2, parse_counters(text)


def next_event(radio, deadline):
    while time.monotonic() < deadline:
        if select.select([radio], [], [], min(0.2, max(0, deadline-time.monotonic())))[0]:
            data = radio.recv(1024)
            if len(data) >= 3 and data[0] == 4 and len(data) == data[2] + 3:
                return data[1], data[3:]
    raise TimeoutError('desktop HCI event timed out; inspect btmon')


def bluez(address, connect):
    command = ['bluetoothctl', 'connect' if connect else 'disconnect', address]
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    print(result.stdout.strip(), flush=True)
    if connect and result.returncode:
        raise RuntimeError(result.stderr or result.stdout)


def connect(radio, address):
    bluez(address, True)
    deadline = time.monotonic() + 15
    peer = bytes.fromhex(address.replace(':', ''))[::-1]
    while True:
        code, p = next_event(radio, deadline)
        if code == 0x3e and len(p) >= 12 and p[0] in (1, 10) and p[6:12] == peer:
            if p[1]:
                raise RuntimeError(f'connection status {p[1]:02x}')
            return struct.unpack_from('<H', p, 2)[0]


def update(radio, handle, interval):
    # Drain old autonomous update events before issuing this one command.
    while select.select([radio], [], [], 0)[0]:
        radio.recv(1024)
    body = struct.pack('<7H', handle, interval, interval, 0, 500, 0, 0)
    radio.sendall(bytes((1, 0x13, 0x20, len(body))) + body)
    deadline = time.monotonic() + 15
    while True:
        code, p = next_event(radio, deadline)
        if code == 0x0f and len(p) == 4 and p[2:] == b'\x13\x20' and p[0]:
            raise RuntimeError(f'LE Connection Update Command Status {p[0]:02x}')
        if code == 5 and len(p) >= 4 and struct.unpack_from('<H', p, 1)[0] == handle:
            raise RuntimeError(f'disconnected during update: {p[3]:02x}')
        if code == 0x3e and len(p) >= 10 and p[0] == 3 and struct.unpack_from('<H', p, 2)[0] == handle:
            if p[1] or struct.unpack_from('<H', p, 4)[0] != interval:
                raise RuntimeError(f'unexpected Update Complete: {p.hex(" ")}')
            print(f'desktop Update Complete 00: {interval*1.25:g} ms', flush=True)
            return


def stable(console, interval=None):
    deadline = time.monotonic() + 20
    previous = None
    while time.monotonic() < deadline:
        stamp, values = console.sample()
        signature = (values['link_epoch'], values['conn_interval_1250us'], values['conn_updates'])
        if (values['link_state_idle_created_up_closing_stopped'] == 2 and
                values['enc_enabled'] == 1 and values['pending_kind'] == 0 and
                values['conn_latency'] == 0 and not values.get('param_pending', 1) and (interval is None or signature[1] == interval) and
                signature == previous):
            return stamp, values
        previous = signature
        time.sleep(0.5)
    raise RuntimeError('encrypted stable link not reached; pair/bond before this bench and inspect dumps')


def measure(console, interval, seconds, label):
    t0, a = stable(console, interval)
    time.sleep(seconds)
    t1, b = console.sample()
    if (a['link_epoch'] != b['link_epoch'] or b['pending_kind'] or
            b['conn_interval_1250us'] != interval or a['conn_updates'] != b['conn_updates'] or
            b['link_state_idle_created_up_closing_stopped'] != 2 or b['enc_enabled'] != 1 or
            b['supervision_fail'] != a['supervision_fail'] or b['late_instants'] != a['late_instants']):
        raise RuntimeError('link/procedure changed during measurement; inspect saved dumps')
    delta = (b['conn_event'] - a['conn_event']) & 0xffff
    serviced = (b['conn_events'] - a['conn_events']) & 0xffffffff
    actual = (t1-t0)*1000/delta if delta else float('inf')
    result = dict(label=label, seconds=t1-t0, event_delta=delta, serviced_delta=serviced,
                  measured_ms=actual, expected_ms=interval*1.25, epoch=a['link_epoch'])
    print(json.dumps(result), flush=True)
    if delta != serviced or abs(actual-interval*1.25) > interval*1.25*0.08:
        raise RuntimeError('wall-clock interval mismatch or suppressed event closes')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--address', required=True)
    parser.add_argument('--usb-port', required=True, help='exact USB-MIDI input/output name; do not select the BLE port')
    parser.add_argument('--full-log', action='store_true', help='use fm1_remote log instead of the fast ble readout')
    parser.add_argument('--index', type=int, default=0)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--measure-seconds', type=float, default=10)
    parser.add_argument('--soak-seconds', type=float, default=95,
                        help='initial hold crosses the original 89.478-second failure boundary')
    args = parser.parse_args()
    if args.measure_seconds < 5 or args.soak_seconds < 0:
        parser.error('measure for at least 5 seconds; soak must be nonnegative')
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    error = None
    completed = False
    try:
        with (args.output / 'btmon.txt').open('w') as log, socket.socket(
                socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as radio:
            radio.bind((args.index,))
            radio.setsockopt(0, 2, struct.pack('<IIIH2x', 1 << 4, 0xffffffff, 0xffffffff, 0))
            monitor = subprocess.Popen(['btmon', '-i', f'hci{args.index}', '-w', str(args.output/'capture.btsnoop')],
                                       stdout=log, stderr=subprocess.STDOUT)
            console = None
            try:
                time.sleep(0.5)
                if monitor.poll() is not None:
                    raise RuntimeError('btmon failed; inspect btmon.txt')
                console = Console(args.usb_port, args.output, args.full_log)
                bluez(args.address, False)
                time.sleep(1)
                handle = connect(radio, args.address)
                _, current = stable(console)
                if current['conn_interval_1250us'] != 12:
                    update(radio, handle, 12)  # separately captured normalization
                results.append(measure(console, 12, args.measure_seconds, 'initial 15 ms'))
                time.sleep(args.soak_seconds)
                results.append(measure(console, 12, args.measure_seconds, '15 ms after soak'))
                for old, new in ((12, 6), (6, 9), (9, 12), (12, 24), (24, 12)):
                    _, before = stable(console, old)
                    update(radio, handle, new)
                    _, after = stable(console, new)
                    if after['conn_updates'] != before['conn_updates'] + 1:
                        raise RuntimeError('FM-1 must apply exactly one update; inspect update records')
                    results.append(measure(console, new, args.measure_seconds, f'{old*1.25:g}->{new*1.25:g} ms'))
                _, before = stable(console, 12)
                bluez(args.address, False)
                time.sleep(1)
                handle = connect(radio, args.address)
                _, after = stable(console)
                if after['link_epoch'] == before['link_epoch']:
                    raise RuntimeError('connection epoch did not change')
                if after['conn_interval_1250us'] != 12:
                    update(radio, handle, 12)
                results.append(measure(console, 12, args.measure_seconds, '15 ms after disconnect/reconnect'))
                bluez(args.address, False)
            finally:
                if console:
                    console.close()
                monitor.send_signal(signal.SIGINT)
                monitor.wait(timeout=10)
        completed = True
    except BaseException as exc:
        error = dict(type=type(exc).__name__, detail=str(exc))
        raise
    finally:
        # Preserve every completed measurement even if setup, reconnect, USB,
        # a later measurement or monitor cleanup fails (also Ctrl-C).
        (args.output/'measurements.json').write_text(json.dumps(results, indent=2)+'\n')
        (args.output/'status.json').write_text(json.dumps(dict(
            completed=completed, measurements=len(results), error=error), indent=2)+'\n')



if __name__ == '__main__':
    main()
