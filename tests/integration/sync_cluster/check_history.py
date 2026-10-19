#!/usr/bin/env python3
"""Health monitor history checks on node-a (run_cluster.sh <dir> history).

Usage: check_history.py <cluster_dir> <step> [timeout_sec]
  steps: h0_config        speed up the monitor through /media/monitor/config/set
         h1_history       /media/monitor/history: points, fields filter, system series, bad args
         h2_open          play the live stream + an unknown one: open latency samples
         h3_baseline      fps baseline of the main stream learnt (kept in the state file)
         h4_coverage      24 h recording coverage of the camera computed, no gap
         h5_gap           after the source stopped for a while: a gap in the coverage
         h6_clean_restart after a clean restart: restarts_24h = 1, none unclean, baseline restored
         h7_unclean       after kill -9: unclean_restarts_24h = 1, process warning
         h8_rss           synthetic RSS history growing 300 MB/h: host warning on the growth
Polls node-a with the api.secret header until the step holds.
"""
import glob
import json
import os
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CLUSTER, STEP = sys.argv[1], sys.argv[2]
TIMEOUT = int(sys.argv[3]) if len(sys.argv) > 3 else 180
BASE = "http://127.0.0.1:18081"
CAM = "cam-fo-1"
MAIN = CAM + "-main"
STATE_PATH = os.path.join(CLUSTER, "history_state.json")


def secret():
    for line in open(os.path.join(CLUSTER, "node-a", "config.ini"), encoding="utf-8"):
        if line.startswith("secret="):
            return line.split("=", 1)[1].strip()
    return ""


def call(path, params=None, payload=None):
    url = BASE + path + ("?" + urllib.parse.urlencode(params) if params else "")
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"secret": secret()}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as ex:
        return json.loads(ex.read().decode() or "{}")


def load_state():
    try:
        with open(STATE_PATH) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def save_state(state):
    with open(STATE_PATH, "w") as fh:
        json.dump(state, fh)


def summary():
    return call("/media/monitor/summary")["data"]


def camera():
    for c in call("/media/monitor/cameras", {"health": "all"})["data"].get("cameras", []):
        if c["cameraId"] == CAM:
            return c
    return None


def ffprobe(url, timeout=15):
    cmd = ["ffprobe", "-v", "error", "-rw_timeout", str(timeout * 1000000), "-rtsp_transport", "tcp",
           "-show_entries", "stream=codec_type", "-of", "json", url]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=timeout + 5, text=True).returncode == 0
    except subprocess.TimeoutExpired:
        return False


# ------------------------------------------------------------------ steps
def h0_config():
    ret = call("/media/monitor/config/set", payload={
        "monitor.baselineWarmupSec": 0, "monitor.baselineWindowSec": 120, "monitor.coverageIntervalSec": 60,
        "monitor.historyIntervalSec": 10, "monitor.playMinRequests": 3, "monitor.slowIntervalSec": 10})
    return ret.get("code") == 0, "config/set code=%s changed=%s" % (ret.get("code"), ret.get("changed"))


def h1_history():
    now = int(time.time())
    full = call("/media/monitor/history", {"from": now - 3600, "to": now + 60})
    d = full.get("data", {})
    points = d.get("points", [])
    if full.get("code") != 0 or len(points) < 3:
        return False, "code=%s points=%d" % (full.get("code"), len(points))
    p = points[-1]
    need = {"timestamp", "samples", "overall", "score", "score_min", "cameras_total", "cameras_online",
            "rec_expected", "rec_active", "rec_stalled", "viewers_total", "poller_delay_max_ms",
            "alerts_active", "proc_rss"}
    if not need.issubset(p) or p["proc_rss"] <= 0 or not d.get("system"):
        return False, "point=%s system=%d" % (p, len(d.get("system", [])))
    only = call("/media/monitor/history", {"from": now - 3600, "to": now + 60, "fields": "score"})["data"]
    if set(only["points"][-1]) != {"timestamp", "samples", "score"} or "system" in only:
        return False, "fields filter: %s" % only["points"][-1]
    bucket = call("/media/monitor/history", {"from": now - 3600, "to": now + 60, "bucket_sec": 3600})["data"]
    if len(bucket["points"]) > 2 or bucket["points"][-1]["samples"] < 3:
        return False, "bucket: %s" % bucket["points"]
    bad = call("/media/monitor/history", {"from": now, "to": now - 10})
    try:
        nosecret = json.loads(urllib.request.urlopen(BASE + "/media/monitor/history").read().decode())
    except urllib.error.HTTPError as ex:
        nosecret = json.loads(ex.read().decode() or "{}")
    if bad.get("code") == 0 or nosecret.get("code") == 0:
        return False, "bad args code=%s, no secret code=%s" % (bad.get("code"), nosecret.get("code"))
    return True, "points=%d last=%s system=%d bucket_sec(1h range)=%s" % (
        len(points), {k: p[k] for k in ("overall", "score", "cameras_online", "rec_active", "proc_rss")},
        len(d["system"]), d.get("bucket_sec"))


_opened = []


def h2_open():
    if not _opened:
        ok = [ffprobe("rtsp://127.0.0.1:18551/%s/%s" % (CAM, MAIN)) for _ in range(4)]
        ffprobe("rtsp://127.0.0.1:18551/%s/no-such-stream" % CAM, timeout=20)
        _opened.append(ok)
    o = summary()["delivery"]["play"]["open"]
    # The unknown stream waits out maxStreamWaitMS: counted, not in the live percentiles.
    ok = (o["live_samples"] >= 4 and o["not_found"] >= 1 and o["live_p95_ms"] is not None
          and o["live_p95_ms"] < 2000)
    return ok, "plays=%s open=%s" % (_opened[0], o)


def h3_baseline():
    c = camera()
    main = (c or {}).get("streams", {}).get("main") or {}
    if main.get("baseline_at") is None:
        return False, "main=%s" % {k: main.get(k) for k in ("fps", "fps_baseline", "baseline_at")}
    st = load_state()
    st["baseline_at"] = main["baseline_at"]
    st["fps_baseline"] = main["fps_baseline"]
    save_state(st)
    return True, "fps=%s fps_baseline=%s baseline_at=%s" % (main["fps"], main["fps_baseline"], main["baseline_at"])


def h4_coverage():
    c = camera()
    r = (c or {}).get("recording", {})
    ok = r.get("coverage_pct_24h") is not None and r["coverage_pct_24h"] >= 95 and r.get("gaps_24h") == 0
    return ok, "recording=%s" % r


def h5_gap():
    c = camera()
    r = (c or {}).get("recording", {})
    ok = (r.get("gaps_24h") or 0) >= 1 and (r.get("longest_gap_sec") or 0) >= 60
    return ok, "recording=%s" % r


def h6_clean_restart():
    d = summary()
    proc = d["process"]
    st = load_state()
    main = ((camera() or {}).get("streams", {}).get("main")) or {}
    ok = (proc.get("restarts_24h") == 1 and proc.get("unclean_restarts_24h") == 0
          and main.get("baseline_at") == st.get("baseline_at"))
    return ok, "restarts=%s unclean=%s baseline_at=%s (before %s) uptime=%s" % (
        proc.get("restarts_24h"), proc.get("unclean_restarts_24h"), main.get("baseline_at"),
        st.get("baseline_at"), proc.get("uptimeSec"))


def h7_unclean():
    d = summary()
    proc = d["process"]
    reasons = [r["metric"] for r in d["reasons"] if r["component"] == "process"]
    ok = (proc.get("unclean_restarts_24h") == 1 and d["components"]["process"] == "warning"
          and "process.unclean_restarts_24h" in reasons)
    return ok, "restarts=%s unclean=%s process=%s reasons=%s" % (
        proc.get("restarts_24h"), proc.get("unclean_restarts_24h"), d["components"]["process"], reasons)


_inserted = []


def h8_rss():
    if not _inserted:
        started = summary()["process"]["startedAt"]
        db = None
        for path in glob.glob(os.path.join(CLUSTER, "node-a", "db", "*")):
            try:
                con = sqlite3.connect(path, timeout=10)
                con.execute("SELECT 1 FROM health_metrics LIMIT 1")
                db = con
                break
            except sqlite3.Error:
                continue
        if db is None:
            return False, "no database with health_metrics"
        mb = 1024 * 1024
        rows = [(started + 1800 + i * 60, "ok", 100, 500 * mb + int(i / 60.0 * 300 * mb)) for i in range(181)]
        db.executemany("INSERT INTO health_metrics (timestamp, overall, score, proc_rss) VALUES (?, ?, ?, ?)", rows)
        db.commit()
        db.close()
        _inserted.append(len(rows))
    d = summary()
    ram = d["host"]["ram"]
    reasons = [r["metric"] for r in d["reasons"] if r["component"] == "host"]
    g = ram.get("proc_rss_growth_mb_per_h")
    ok = g is not None and abs(g - 300) < 5 and "ram.proc_rss_growth_mb_per_h" in reasons
    return ok, "growth=%s span=%s host=%s reasons=%s" % (g, ram.get("proc_rss_growth_span_sec"),
                                                         d["components"]["host"], reasons)


STEPS = {"h0_config": h0_config, "h1_history": h1_history, "h2_open": h2_open, "h3_baseline": h3_baseline,
         "h4_coverage": h4_coverage, "h5_gap": h5_gap, "h6_clean_restart": h6_clean_restart,
         "h7_unclean": h7_unclean, "h8_rss": h8_rss}


def main():
    check = STEPS[STEP]
    start, detail = time.time(), ""
    while time.time() - start < TIMEOUT:
        try:
            ok, detail = check()
            if ok:
                print("PASS %s after %.0fs: %s" % (STEP, time.time() - start, detail), flush=True)
                return 0
        except Exception as ex:  # node restarting
            detail = "error: %s" % ex
        time.sleep(5)
    print("FAIL %s: %s" % (STEP, detail), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
