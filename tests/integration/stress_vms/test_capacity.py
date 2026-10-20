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


def evaluate_row(source=None, record_close=None, logs0=None, logs1=None, alive=True):
    rn = object.__new__(cc.Runner)
    rn.th = cc.THRESHOLDS
    rn.args = SimpleNamespace(work="", node="node-1")
    rn.s = Mock()
    rn.s.nic_speed_mbps.return_value = 0
    rn.s.alive.return_value = alive
    rn.baseline = None
    rn.always = set()
    rn.source_window = source or {"cpu_pct_max": 0, "tick_late_p99_ms": 0}
    snap = dict(cpu=0, tasks={}, rss_kb=0, fds=0, tx=0)
    smp = SimpleNamespace(loads={}, health=[1], monitor=[])
    if record_close is not None:
        smp.record_close = record_close
    with patch.object(cc.cs, "check_schedules", return_value={"mismatch": 0}), \
            patch.object(cc, "mem_total_kb", return_value=1024 * 1024), \
            patch.object(cc, "record_gaps", return_value={"gaps": 0, "stale": 0}):
        return rn.evaluate(0, "0", 0, None, {}, dict(snap, t=100), dict(snap, t=160), smp,
                           {"live_streams": 2, "readers": 0, "streams": {}},
                           logs0 or {"rtp_timeout": 0}, logs1 or {"rtp_timeout": 0})


def close_samples(*queued, oldest_ms=1000):
    return [{"at": i * 5, "shards": [{"queued": q, "oldest_queued_ms": oldest_ms}]}
            for i, q in enumerate(queued)]


class CapacityEvidenceTest(unittest.TestCase):
    def test_ramp_timeout_fails_even_when_measurement_window_is_clean(self):
        row = evaluate_row(logs0={"rtp_timeout": 1}, logs1={"rtp_timeout": 1})
        self.assertFalse(row["ok"])
        self.assertEqual(row["limit"], "recording")
        self.assertEqual(row["ingest_timeouts"], 0)
        self.assertEqual(row["ingest_timeouts_outside_window"], 1)

    # L9: G-a lần 3 bậc 600 được chấm "đạt" khi hàng đợi rec close tăng 1 448 → 5 355.
    def test_growing_record_close_queue_fails(self):
        row = evaluate_row(record_close=close_samples(1448, 2300, 3100, 4200, 5355))
        self.assertEqual(row["verdict"], "trượt")
        self.assertEqual(row["limit"], "recording")
        self.assertTrue(row["record_close"]["growing"])

    def test_old_segment_in_record_close_queue_fails(self):
        row = evaluate_row(record_close=close_samples(5, 3, 4, oldest_ms=75000))
        self.assertEqual(row["verdict"], "trượt")
        self.assertIn("rec close", row["failures"][0])

    def test_small_or_draining_record_close_queue_passes(self):
        self.assertTrue(evaluate_row(record_close=close_samples(0, 2, 1, 3, 0, 2))["ok"])
        self.assertTrue(evaluate_row(record_close=close_samples(4000, 3000, 2000, 900))["ok"])

    # L7: nguồn còn CPU, trễ nhịp sau khi server đã timeout luồng kéo -> trượt, không phải không hợp lệ.
    def test_source_late_after_server_timeout_is_a_failure_not_invalid(self):
        late = {"cpu_pct_max": 20, "tick_late_p99_ms": 400, "tick_late_max_ms": 6000}
        row = evaluate_row(source=late, logs0={"rtp_timeout": 0}, logs1={"rtp_timeout": 352})
        self.assertEqual(row["verdict"], "trượt")
        self.assertEqual(row["invalid"], [])
        self.assertEqual(row["limit"], "recording")
        row = evaluate_row(source=late, alive=False)
        self.assertEqual(row["verdict"], "trượt")
        self.assertEqual(row["limit"], "crash")

    def test_source_late_while_server_is_healthy_stays_invalid(self):
        late = {"cpu_pct_max": 20, "tick_late_p99_ms": 400, "tick_late_max_ms": 6000}
        self.assertEqual(evaluate_row(source=late)["verdict"], "không hợp lệ (bộ sinh tải quá tải)")
        busy = dict(late, cpu_pct_max=95)
        row = evaluate_row(source=busy, logs1={"rtp_timeout": 10})
        self.assertEqual(row["verdict"], "không hợp lệ (bộ sinh tải quá tải)")

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

    def test_short_stall_is_recorded_from_threshold(self):
        p = Player(0)
        p.wall_offset = 1000
        p.on_media(10, 0, 1)
        p.on_media(11.4, 1, 2)    # ~0,4 s khựng
        p.on_media(11.5, 2, 3)    # không khựng
        self.assertEqual(len(p.stall_events), 1)
        self.assertEqual(p.stall_events[0]["at"], 1011)

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
