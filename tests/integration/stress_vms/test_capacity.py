"""Regression tests for capacity evidence; run with unittest discovery."""
import csv
import json
import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock
from unittest.mock import patch

import check_ccu as cc
from viewer_load import Player


class CapacityEvidenceTest(unittest.TestCase):
    def test_ramp_timeout_fails_even_when_measurement_window_is_clean(self):
        rn = object.__new__(cc.Runner)
        rn.th = cc.THRESHOLDS
        rn.args = SimpleNamespace(work="", node="node-1")
        rn.s = Mock()
        rn.s.nic_speed_mbps.return_value = 0
        rn.baseline = None
        rn.always = set()
        rn.source_window = {"cpu_pct_max": 0, "tick_late_p99_ms": 0}
        snap = dict(cpu=0, tasks={}, rss_kb=0, fds=0, tx=0)
        with patch.object(cc.cs, "check_schedules", return_value={"mismatch": 0}), \
                patch.object(cc, "record_gaps", return_value={"gaps": 0, "stale": 0}):
            row = rn.evaluate(0, "0", 0, None, {}, dict(snap, t=100), dict(snap, t=160),
                              SimpleNamespace(loads={}, health=[1]),
                              {"live_streams": 2, "readers": 0, "streams": {}},
                              {"rtp_timeout": 1}, {"rtp_timeout": 1})
        self.assertFalse(row["ok"])
        self.assertEqual(row["limit"], "recording")
        self.assertEqual(row["ingest_timeouts"], 0)
        self.assertEqual(row["ingest_timeouts_outside_window"], 1)

    def test_scan_includes_ramp_timeout_and_execution_threads(self):
        with tempfile.TemporaryDirectory() as work:
            os.mkdir(os.path.join(work, "node-1"))
            with open(os.path.join(work, "node-1", "stdout.log"), "w") as fh:
                fh.write("[10-event poller 3] receive rtp timeout\n")
                fh.write("[10-rec close 1] Record mp4 file cam/hi/100/60//rec/cam/hi/file.mp4\n")
                fh.write("[10-api worker 2] Server configuration for camera loaded completed, took 200 ms\n")
            with patch.object(cc.cs, "scan_logs", return_value={}):
                result = cc.scan_logs(work, "node-1")
            self.assertEqual(result["rtp_timeout"], 1)
            self.assertEqual(result["execution_threads"], {"record": {"rec close": 1}, "config": {"api worker": 1}})
            self.assertEqual(result["segment_ends"]["/rec/cam/hi/file.mp4"], 160)

    def test_late_close_does_not_hide_media_gap(self):
        files = {"stream": [(100, 185, "first.mp4"), (180, 240, "next.mp4")]}
        with patch.object(cc, "record_streams", return_value=files):
            result = cc.record_gaps("", "", set(), 0, 240, cc.THRESHOLDS,
                                    {"stream/first.mp4": 160})
        self.assertEqual(result["gaps"], 1)
        self.assertEqual(result["stale"], 0)

    def test_stall_event_has_wall_clock_and_duration(self):
        p = Player(0)
        p.wall_offset = 1000
        p.on_media(10, 0, 1)
        p.on_media(14, 1, 5)
        self.assertEqual(p.stall_events, [{"at": 1011, "duration_sec": 3}])

    def test_session_csv_preserves_json_events_and_quotes(self):
        with tempfile.TemporaryDirectory() as work:
            path = os.path.join(work, "sessions.csv")
            events = [{"at": 1000.25, "duration_sec": 2.5}]
            cc.write_sessions_csv(path, [{"slot": 1, "why": 'a,"quoted"', "stall_events": events}])
            with open(path, newline="") as fh:
                row = next(csv.DictReader(fh))
            self.assertEqual(json.loads(row["stall_events"]), events)
            self.assertEqual(row["why"], 'a,"quoted"')


if __name__ == "__main__":
    unittest.main()
