#!/usr/bin/env python3
"""Peer info changes seen from node-a (run_cluster.sh <dir> peerinfo).

Usage: check_peerinfo.py <cluster_dir> <step> [timeout_sec]
  steps: pi1_synced     node-b reachable and in node-a's sync peers; one periodic health check done
         pi2_same_endpoint  node-b renamed, failover flag flipped: applied without a new health check,
                        node-b never unreachable, still a sync peer
         pi3_endpoint   node-b listed with a wrong HTTP port: checked at once, out of the sync peers
         pi4_endpoint_back  correct port again: checked at once, back in the sync peers
         pi5_persisted  node-a restarted while the service API lists no server: the saved peer list
                        (with its server group) still makes node-b a sync peer
Polls GET /media/monitor/summary on node-a (api.secret header) and reads node-a/log/*.log
(stdout.log is colored, its lines do not start with the timestamp).
"""
import datetime
import glob
import json
import os
import re
import sys
import time
import urllib.request

CLUSTER, STEP = sys.argv[1], sys.argv[2]
TIMEOUT = int(sys.argv[3]) if len(sys.argv) > 3 else 120
URL = "http://127.0.0.1:18081/media/monitor/summary"
LOG_GLOB = os.path.join(CLUSTER, "node-a", "log", "*.log")
STATE = os.path.join(CLUSTER, "peerinfo_state.json")
PEER_URL = "http://127.0.0.1:18082"
BAD_URL = "http://127.0.0.1:18099"
RENAMED = "node-b-renamed"
GROUP = int(os.environ.get("SERVER_GROUP_ID", "5"))
HEALTH_PERIOD_SEC = 60  # ClusterManager timer
TS = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ")


def secret():
    # MediaServer dumps config.ini with CRLF line endings.
    for line in open(os.path.join(CLUSTER, "node-a", "config.ini"), encoding="utf-8"):
        if line.startswith("secret="):
            return line.split("=", 1)[1].strip()
    return ""


def summary():
    req = urllib.request.Request(URL, headers={"secret": secret()})
    with urllib.request.urlopen(req, timeout=5) as r:
        body = json.loads(r.read().decode())
    if body.get("code") != 0:
        raise RuntimeError("code=%s msg=%s" % (body.get("code"), body.get("msg")))
    return body["data"]


def peer_view():
    """(cluster peer entry of node-b or None, sync peer entry of node-b or None)."""
    cl = summary().get("cluster", {})
    peer = next((p for p in cl.get("peers", []) if p.get("id") == "node-b"), None)
    sync = next((p for p in cl.get("sync", {}).get("peers", []) if p.get("id") == "node-b"), None)
    return peer, sync


def log_lines(pattern):
    """(timestamp, line) of node-a log lines matching pattern."""
    out = []
    rx = re.compile(pattern)
    for path in sorted(glob.glob(LOG_GLOB)):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if rx.search(line):
                    m = TS.match(line)
                    if m:
                        ts = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
                        out.append((ts, line.rstrip()))
    return sorted(out)


def load_state():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {}


def save_state(**kv):
    st = load_state()
    st.update(kv)
    json.dump(st, open(STATE, "w"))


def wait(desc, pred):
    deadline = time.time() + TIMEOUT
    last = None
    while time.time() < deadline:
        try:
            ok, last = pred()
            if ok:
                print("PASS %s: %s" % (desc, last))
                return True
        except Exception as ex:  # node restarting, summary not computed yet
            last = "error: %s" % ex
        time.sleep(2)
    print("FAIL %s: %s" % (desc, last))
    return False


def applied_after(name, since):
    """Time node-a applied a server list where node-b has this name, after `since`."""
    lines = [t for t, _ in log_lines(r"Added media server: node-b, name: %s$" % re.escape(name)) if t >= since]
    return lines[0] if lines else None


def health_lines(url, kind):
    return [t for t, _ in log_lines(r"Health check %s for %s" % (kind, re.escape(url)))]


def pi1():
    ok = wait("node-b reachable and a sync peer of node-a",
              lambda: (lambda p, s: (bool(p and p.get("reachable") and s), (p, s)))(*peer_view()))
    # pi2 tells periodic checks from immediate ones: wait for one periodic round after start.
    ok &= wait("at least one periodic health check of node-b",
               lambda: (len(health_lines(PEER_URL, "succeeded")) >= 2, health_lines(PEER_URL, "succeeded")[-3:]))
    save_state(pi2_since=time.time())
    return ok


def pi2():
    since = load_state().get("pi2_since", 0)
    ok = wait("renamed node-b applied (summary shows the new name and failover flag)",
              lambda: (lambda p, s: (bool(p and p.get("name") == RENAMED and p.get("hasFailover") is False),
                                     p))(*peer_view()))
    applied = applied_after(RENAMED, since)
    if applied is None:
        print("FAIL rename not found in node-a log")
        return False
    # Watch for 20 s: node-b stays reachable and a sync peer on every sample.
    end, bad = time.time() + 20, []
    while time.time() < end:
        try:
            p, s = peer_view()
            if not (p and p.get("reachable") and s):
                bad.append((p, s))
        except Exception as ex:
            bad.append(str(ex))
        time.sleep(2)
    print("%s node-b reachable and a sync peer during 20 s after the rename%s"
          % ("PASS" if not bad else "FAIL", "" if not bad else ": %s" % bad[:3]))
    ok &= not bad
    unreachable = [l for t, l in log_lines(r"Peer node-b is unreachable") if t >= since]
    print("%s no 'Peer node-b is unreachable' after the rename%s"
          % ("PASS" if not unreachable else "FAIL", "" if not unreachable else ": %s" % unreachable[:2]))
    ok &= not unreachable
    # No health check outside the periodic slots: the rename must not invalidate the last check.
    before = [t for t in health_lines(PEER_URL, "succeeded") if t < applied]
    after = [t for t in health_lines(PEER_URL, "succeeded") + health_lines(PEER_URL, "failed")
             if applied <= t <= applied + 5]
    slot = before[-1] if before else None
    extra = [t for t in after
             if slot is None or abs(((t - slot) % HEALTH_PERIOD_SEC + 2) % HEALTH_PERIOD_SEC - 2) > 2]
    print("%s no immediate health check of node-b after the rename (applied %.3f, last periodic %s, "
          "within 5 s: %s)" % ("PASS" if not extra else "FAIL", applied, slot, after))
    ok &= not extra
    save_state(pi3_since=time.time())
    return ok


def pi3():
    since = load_state().get("pi3_since", 0)
    ok = wait("node-a checks the new endpoint at once and drops node-b from the sync peers",
              lambda: (lambda p, s: (bool(p and not p.get("reachable") and s is None
                                          and [t for t in health_lines(BAD_URL, "failed") if t >= since]),
                                     (p, s)))(*peer_view()))
    save_state(pi4_since=time.time())
    return ok


def pi4():
    since = load_state().get("pi4_since", 0)
    return wait("node-a checks the restored endpoint at once and syncs with node-b again",
                lambda: (lambda p, s: (bool(p and p.get("reachable") and s
                                            and [t for t in health_lines(PEER_URL, "succeeded") if t >= since]),
                                       (p, s)))(*peer_view()))


def pi5():
    since = load_state().get("pi5_since", 0)
    ok = True
    peers = None
    for line in open(os.path.join(CLUSTER, "node-a", "config.ini"), encoding="utf-8"):
        if line.startswith("peer_list="):
            peers = json.loads(line.split("=", 1)[1].strip())
    groups = {p.get("id"): p.get("serverGroupId") for p in peers or []}
    print("%s saved peer list keeps the server group: %s" % ("PASS" if groups.get("node-b") == GROUP else "FAIL", groups))
    ok &= groups.get("node-b") == GROUP
    ok &= wait("node-a (no server list from the service API) pulls from node-b after the restart",
               lambda: (lambda p, s: (bool(s and s.get("last_ok_at") and s["last_ok_at"] >= since), (p, s)))(*peer_view()))
    applied = [t for t, _ in log_lines(r"Load media server cluster config") if t >= since]
    print("%s node-a got no server list from the service API after the restart%s"
          % ("PASS" if not applied else "FAIL", "" if not applied else ": %d" % len(applied)))
    ok &= not applied
    return ok


if __name__ == "__main__":
    if STEP == "pi5_mark":
        save_state(pi5_since=time.time())
        sys.exit(0)
    steps = {"pi1_synced": pi1, "pi2_same_endpoint": pi2, "pi3_endpoint": pi3,
             "pi4_endpoint_back": pi4, "pi5_persisted": pi5}
    sys.exit(0 if steps[STEP]() else 1)
