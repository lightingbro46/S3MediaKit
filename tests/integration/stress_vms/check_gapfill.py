#!/usr/bin/env python3
"""Báo cáo kịch bản gapfill: rớt mạng đồng loạt rồi bù khoảng hở từ thẻ SD.

In theo từng phase (mốc trong phases.jsonl):
  - thời gian vòng tier ("Tiering cycle done. Elapsed"),
  - các pool BlockingExecutors (blocking.jsonl): hàng đợi lớn nhất, chờ trung bình/lớn nhất,
    % bận trung bình/lớn nhất, số task,
  - số lần ghi lại timefile lệch thứ tự ("Rewrote N out-of-order").
Cuối cùng: trạng thái task gap fill trong DB, số phiên replay nguồn đã phát hết, số timefile
còn dấu chưa sắp xếp trong .dirty.

Dùng: check_gapfill.py <work_dir> [--node node-a]
"""
import argparse
import glob
import json
import os
import re
import sqlite3
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")
STAMP = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")
DURATION = re.compile(r"(\d+)(h|m|s|ms)\b")


def duration_ms(text):
    unit = {"h": 3600000, "m": 60000, "s": 1000, "ms": 1}
    return sum(int(v) * unit[u] for v, u in DURATION.findall(text))


def log_events(node_dir):
    """(epoch, dòng) cho các dòng quan tâm trong log của node, theo thứ tự thời gian."""
    out = []
    files = sorted(glob.glob(os.path.join(node_dir, "log", "*.log")))
    for path in files:
        with open(path, errors="replace") as fh:
            for raw in fh:
                if ("Tiering cycle done" not in raw and "out-of-order time file" not in raw
                        and "Skip time file rebuild" not in raw and "stale timefile block" not in raw):
                    continue
                line = ANSI.sub("", raw)
                m = STAMP.match(line)
                if not m:
                    continue
                at = time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))
                out.append((at, line.strip()))
    return sorted(out)


def phases(work):
    rows = [json.loads(l) for l in open(os.path.join(work, "phases.jsonl")) if l.strip()]
    spans = []
    for i, row in enumerate(rows):
        end = rows[i + 1]["at"] if i + 1 < len(rows) else time.time()
        spans.append((row["phase"], row["at"], end))
    return spans


def pct(values, p):
    if not values:
        return 0
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--node", default="node-a")
    args = ap.parse_args()
    node_dir = os.path.join(args.work, args.node)

    events = log_events(node_dir)
    samples = []
    path = os.path.join(args.work, "blocking.jsonl")
    if os.path.exists(path):
        for line in open(path):
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("node") == args.node:
                samples.append(row)

    for name, start, end in phases(args.work):
        print("== phase %s (%d s)" % (name, end - start))
        cycles = [duration_ms(l.split("Elapsed:", 1)[1]) for at, l in events
                  if start <= at < end and "Tiering cycle done" in l]
        if cycles:
            print("   vòng tier: %d vòng, max %.1f s, trung bình %.1f s"
                  % (len(cycles), max(cycles) / 1000.0, sum(cycles) / 1000.0 / len(cycles)))
        rewrote = [int(re.search(r"Rewrote (\d+)", l).group(1)) for at, l in events
                   if start <= at < end and "Rewrote" in l]
        skipped = sum(1 for at, l in events if start <= at < end and "Skip time file rebuild" in l)
        if rewrote or skipped:
            print("   sắp xếp timefile: %d file trong %d lượt, bỏ lượt vì khoá: %d"
                  % (sum(rewrote), len(rewrote), skipped))
        pools = {}
        for row in samples:
            if not (start <= row["at"] < end):
                continue
            for st in row.get("resp", {}).get("data", []):
                p = pools.setdefault(st["name"], {"threads": st["threads"], "queued": [], "wait_max": [],
                                                  "wait_avg": [], "busy": [], "tasks": 0})
                p["queued"].append(st["queued"])
                p["wait_max"].append(st["window_wait_max_ms"])
                if st["window_tasks"]:
                    p["wait_avg"].append(st["window_wait_avg_ms"])
                p["busy"].append(st["window_busy_pct"])
                p["tasks"] += st["window_tasks"]
        if pools:
            print("   %-18s %3s %8s %10s %10s %8s %8s %8s"
                  % ("pool", "thr", "q max", "chờ p95", "chờ max", "bận tb", "bận max", "task"))
            for pname in sorted(pools):
                p = pools[pname]
                busy = p["busy"]
                print("   %-18s %3d %8d %8.0fms %8.0fms %7.1f%% %7.1f%% %8d"
                      % (pname, p["threads"], max(p["queued"]), pct(p["wait_avg"], 0.95),
                         max(p["wait_max"]), sum(busy) / len(busy), max(busy), p["tasks"]))

    db = os.path.join(node_dir, "db", "mserver.sqlite")
    if os.path.exists(db):
        con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
        print("== task gap fill")
        for state, n, secs in con.execute(
                "select state, count(*), sum(end_time - begin_time) from media_gap_fill_task group by state"):
            print("   %-10s %5d task, %7d s bản ghi" % (state, n, secs or 0))
        done = con.execute("select min(processing_start_time), max(processing_end_time) from media_gap_fill_task "
                           "where processing_end_time > 0").fetchone()
        if done and done[0]:
            print("   xử lý từ %s tới %s" % (time.strftime("%H:%M:%S", time.localtime(done[0])),
                                           time.strftime("%H:%M:%S", time.localtime(done[1]))))
        gaps = con.execute("select count(*) from local_resource_status_gap").fetchone()[0]
        print("   khoảng hở ghi nhận: %d" % gaps)

    state = os.path.join(args.work, "rtsp_state.json")
    if os.path.exists(state):
        st = json.load(open(state))
        print("== nguồn RTSP: replay đã phát hết %d phiên" % st.get("replays_done", 0))

    unsorted = 0
    for dirty in glob.glob(os.path.join(node_dir, "record", "**", ".dirty"), recursive=True):
        unsorted += sum(1 for l in open(dirty) if l.startswith("!"))
    print("== timefile còn dấu chưa sắp xếp: %d" % unsorted)


if __name__ == "__main__":
    main()
