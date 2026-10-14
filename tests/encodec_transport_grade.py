"""Regressions for ready-snapshot subtraction and useful-window coverage."""
from __future__ import annotations

import csv
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

import encodec_transport as transport


class WindowGrade(unittest.TestCase):
    def fixture(self, root, omitted=()):
        start = 1_000_000_000_000
        records = []
        lines = []
        for index in range(135):
            if index in omitted:
                continue
            observed = start + (80_000_000 if index < 3 else index * 39_500_000)
            flags = 13 if index == 0 else 9 if index % 25 == 0 else 0
            record = (observed, 1920, len(records) + 1, index * 40, 1 + index // 25, flags)
            records.append(record)
            lines.append(f'KMX_AUDIO {record[1]} #{record[2]} at={record[3]} epoch={record[4]} flags={flags}\n')
        # The first record can follow terminal repaint escapes on the same line.
        lines[0] = '\x1b[0m\x1b[1;1H' + lines[0]
        (root / 'capture-origin.json').write_text(json.dumps({'monotonic_ns': start - 100_000_000}))
        with (root / 'capture-timing.csv').open('w') as handle:
            writer = csv.writer(handle)
            writer.writerow(('packet', 'capture_ns'))
            for packet in range(270):
                writer.writerow((packet, start - 100_000_000 + packet * 20_000_000))
        stats = ('kmx-attach: encodec_profile calls=0 rtf=0 input_drops=0 output_drops=0 discontinuities=0\n'
                 'kmx-attach: encodec_profile calls=135 rtf=.1 input_drops=0 output_drops=0 discontinuities=1\n'
                 'kmx-attach: encodec calls=135 rtf=.1 input_drops=0 output_drops=0 discontinuities=1\n')
        (root / 'serve.err').write_text(
            'event=start profile=C5-R4 marker=ready input_drops=0 output_drops=0 discontinuities=1\n'
            + stats.replace('kmx-attach:', 'kmx-serve:'))
        dumps, sinks, errors = [], [], []
        for name in ('a', 'b'):
            dump, sink, error = (root / f'healthy-{name}.{ext}' for ext in ('out', 'json', 'err'))
            dump.write_text(''.join(lines))
            sink.write_text(json.dumps({'clean_eof': True, 'pcm_bytes': len(lines) * 1920}))
            error.write_text(stats)
            dumps.append(dump); sinks.append(sink); errors.append(error)
        options = SimpleNamespace(evidence=root, seconds=5, only='installed-calibration6', frozen_bounds=None)
        state = {'records': [list(records), list(records)], 'window_start_ns': start + 80_000_000,
                 'window_end_ns': start + 5_080_000_000, 'process_peak_rss_kb': {'server': 400000},
                 'cgroup_peak_bytes': 1_200_000_000, 'poll_max_seconds': .01}
        return options, state, sinks, dumps, errors

    def test_ready_batch_and_excess_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = transport.finish_calibration(*self.fixture(Path(temporary)))
            self.assertEqual([client['window_frames'] for client in result['clients']], [126, 126])
            self.assertTrue(result['passed'])

    def test_real_loss_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(AssertionError):
                transport.finish_calibration(*self.fixture(Path(temporary), omitted=range(30, 35)))

    def test_lifetime_readiness_duplicate_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self.fixture(Path(temporary))
            path = arguments[3][0]
            path.write_text(path.read_text().replace('#2 at=40', '#1 at=40'))
            with self.assertRaises(AssertionError):
                transport.finish_calibration(*arguments)


if __name__ == '__main__':
    unittest.main()
