#!/usr/bin/env python3
"""Stale snapshot of the health monitor (run_cluster.sh <dir> stale).

Usage: check_stale.py <cluster_dir> <stall_sec>
node-a runs with S3MK_TEST_MONITOR_STALL_SEC=<stall_sec> (Debug build only): one cycle sleeps on the
monitor poller. Checks, in order:
  s1 before the stall: stale=false, snapshotAgeSec small; process.monitor lag_ms/queue_delay_ms small
  s2 during the stall: stale=true, overall=unknown, reason process/monitor.snapshot_age_sec critical,
     /health?rev=<rev> is not notModified, /summary stale too, healthcheck still answers fast
  s3 after the stall: stale=false again, generatedAt moves; lag_max_ms about stall - interval
Prints PASS/FAIL per step; the exit code is the number of failed steps.
"""
import json
import os
import sys
import time
import urllib.request

CLUSTER, STALL = sys.argv[1], int(sys.argv[2])
BASE = "http://127.0.0.1:18081"
LOG = os.path.join(CLUSTER, "node-a", "stdout.log")


def secret():
    # MediaServer dumps config.ini with CRLF line endings.
    for line in open(os.path.join(CLUSTER, "node-a", "config.ini"), encoding="utf-8"):
        if line.startswith("secret="):
            return line.split("=", 1)[1].strip()
    return ""


def get(path):
    req = urllib.request.Request(BASE + path, headers={"secret": secret()})
    with urllib.request.urlopen(req, timeout=5) as r:
        body = json.loads(r.read().decode())
    if body.get("code") != 0:
        raise RuntimeError("code=%s msg=%s" % (body.get("code"), body.get("msg")))
    return body


def health():
    return get("/media/monitor/health")["data"]


def monitor():
    return ((get("/media/monitor/summary")["data"].get("process") or {}).get("monitor")) or {}


def wait_monitor(cond, timeout):
    start, last = time.time(), {}
    while time.time() - start < timeout:
        last = monitor()
        if cond(last):
            break
        time.sleep(1)
    return last


def wait(cond, timeout):
    start, last = time.time(), None
    while time.time() - start < timeout:
        try:
            last = health()
            if cond(last):
                return last
        except Exception as ex:  # node starting
            last = {"error": str(ex)}
        time.sleep(2)
    return last


def brief(d):
    if not d or "error" in d:
        return d
    return "overall=%s stale=%s age=%s rev=%s generatedAt=%s reasons=%s" % (
        d.get("overall"), d.get("stale"), d.get("snapshotAgeSec"), d.get("rev"), d.get("generatedAt"),
        [r["component"] + "/" + r["metric"] + ":" + r["level"] for r in d.get("reasons", [])][:4])


def step(name, ok, detail):
    print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
    return 0 if ok else 1


def main():
    fail = 0
    # Cycle 1 at start, cycle 2 ten seconds later; the stall starts with cycle 3.
    d = wait(lambda x: x.get("enabled") and x.get("stale") is False and x.get("uptimeSec", 0) >= 5, 60)
    fail += step("s1_fresh", bool(d) and d.get("stale") is False and d.get("snapshotAgeSec", 99) < 15, brief(d))
    # Cycle 2 (10 s) has a lag sample, the probe of cycle 1 ran at 5 s; both before the stall at 20 s.
    m = wait_monitor(lambda x: x.get("lag_ms") is not None and x.get("queue_delay_ms") is not None, 30)
    fail += step("s1_poller_idle", m.get("lag_ms") is not None and m["lag_ms"] < 1000
                 and m.get("queue_delay_ms") is not None and m["queue_delay_ms"] < 1000, m)

    d = wait(lambda x: x.get("stale") is True, STALL + 60)
    first = (d or {}).get("reasons") or [{}]
    ok = (bool(d) and d.get("stale") is True and d.get("overall") == "unknown"
          and first[0].get("metric") == "monitor.snapshot_age_sec" and first[0].get("level") == "critical")
    fail += step("s2_stale_health", ok, brief(d))
    if d and d.get("stale"):
        rev = get("/media/monitor/health?rev=%s" % d["rev"])
        fail += step("s2_rev_not_cached", not rev.get("notModified") and rev["data"].get("stale") is True,
                     "notModified=%s" % rev.get("notModified"))
        s = get("/media/monitor/summary")["data"]
        fail += step("s2_summary_stale", s.get("stale") is True and s.get("overall") == "unknown",
                     "stale=%s overall=%s" % (s.get("stale"), s.get("overall")))
        t = time.time()
        urllib.request.urlopen(BASE + "/media/mserver/healthcheck", timeout=5).read()
        ms = (time.time() - t) * 1000
        fail += step("s2_api_responsive", ms < 1000, "healthcheck %.0f ms" % ms)
        logged = "TEST HOOK: monitor poller stalls %d s" % STALL in open(LOG, errors="replace").read()
        fail += step("s2_hook_logged", logged, LOG)
        stale_at = d["generatedAt"]
    else:
        stale_at = None

    d = wait(lambda x: x.get("stale") is False and (stale_at is None or x.get("generatedAt", 0) > stale_at), STALL + 60)
    fail += step("s3_recovered", bool(d) and d.get("stale") is False and d.get("snapshotAgeSec", 99) < 15, brief(d))
    # The Timer fires as soon as the stalled cycle returns: late by about the stall minus one interval.
    m = monitor()
    want = (STALL - 10) * 1000 - 2000
    fail += step("s3_lag_seen", m.get("lag_max_ms", 0) >= want and m.get("queue_delay_max_ms", 0) >= 0,
                 "%s (lag_max_ms >= %d)" % (m, want))
    return fail


if __name__ == "__main__":
    sys.exit(main())
