#!/usr/bin/env python3
"""Checks of the node removal scenario (run_cluster.sh <dir> removal), policy in docs/api-sync.md ("Node bị bỏ khỏi cluster").

Usage: check_removal.py <cluster_dir> <rm1|rm2|rm3>
Each check polls until it holds or RM_WAIT_SEC (default 120) runs out.
Exit code: number of failed checks.
"""
import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CLUSTER, PHASE = sys.argv[1], sys.argv[2]
WAIT_SEC = int(os.environ.get("RM_WAIT_SEC", "120"))
SETTLE_SEC = int(os.environ.get("RM_SETTLE_SEC", "20"))
ORPHAN, MOVED, SPEAKER = "cam-rm-orphan", "cam-rm-moved", "spk-rm-1"
STATE = os.path.join(CLUSTER, "removal_state.json")

failed = 0


def query(node, sql, args=()):
    path = os.path.join(CLUSTER, node, "db", "esc.sqlite")
    if not os.path.exists(path):
        return []
    con = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    try:
        con.execute("PRAGMA busy_timeout=3000")
        return con.execute(sql, args).fetchall()
    except sqlite3.Error:
        return []
    finally:
        con.close()


def has_resource(node, guid):
    return bool(query(node, "SELECT 1 FROM vms_resource WHERE guid=?", (guid,)))


def active_owners(node, guid):
    return [r[0] for r in query(node, "SELECT owner_peer_id FROM vms_resource_assignment "
                                      "WHERE resource_guid=? AND released_at=0", (guid,))]


def open_assignments_of(node, owner):
    return query(node, "SELECT resource_guid FROM vms_resource_assignment WHERE owner_peer_id=? AND released_at=0",
                 (owner,))


def own_max_seq(node):
    rows = query(node, "SELECT max(sequence) FROM transaction_log WHERE peer_guid=?", (node,))
    return rows[0][0] if rows and rows[0][0] is not None else 0


def decommissioned_since(node):
    """peer.decommissioned_since in the node's config.ini (MediaServer writes CRLF)."""
    section = None
    try:
        for line in open(os.path.join(CLUSTER, node, "config.ini"), encoding="utf-8", errors="replace"):
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                section = s[1:-1]
            elif section == "peer" and s.startswith("decommissioned_since="):
                return int(s.split("=", 1)[1] or 0)
    except (OSError, ValueError):
        pass
    return 0


def log_has(node, text):
    path = os.path.join(CLUSTER, node, "stdout.log")
    try:
        return text in open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return False


HTTP_PORT = {"node-a": 18081, "node-b": 18082}


def sync_status(target, requester):
    """HTTP status of /sync/changes and /sync/snapshot on `target` when called as `requester`."""
    base = "http://127.0.0.1:%d/media/esc/sync" % HTTP_PORT[target]
    body = json.dumps({"peer": requester, "db": "db-check", "limit": 1, "cursors": [], "ack_cursors": [],
                       "mediaServerId": requester}).encode()
    calls = (urllib.request.Request(base + "/changes", data=body, headers={"Content-Type": "application/json"}),
             urllib.request.Request(base + "/snapshot?" + urllib.parse.urlencode({"peer": target,
                                                                                 "mediaServerId": requester})))
    out = []
    for req in calls:
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                out.append(resp.status)
        except urllib.error.HTTPError as err:
            out.append(err.code)
        except (urllib.error.URLError, OSError):
            out.append(0)
    return out


def check(code, desc, pred, timeout=None):
    """Poll pred() -> (ok, detail) until ok or timeout."""
    global failed
    deadline = time.time() + (WAIT_SEC if timeout is None else timeout)
    while True:
        ok, detail = pred()
        if ok or time.time() >= deadline:
            break
        time.sleep(1)
    print("%s %s: %s%s" % ("PASS" if ok else "FAIL", code, desc, "" if ok else " -- " + detail))
    if not ok:
        failed += 1
    return ok


def hold(code, desc, pred):
    """pred() must stay true for SETTLE_SEC."""
    global failed
    deadline = time.time() + SETTLE_SEC
    while time.time() < deadline:
        ok, detail = pred()
        if not ok:
            print("FAIL %s: %s -- %s" % (code, desc, detail))
            failed += 1
            return False
        time.sleep(1)
    print("PASS %s: %s" % (code, desc))
    return True


def load_state():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {}


def save_state(**kv):
    state = load_state()
    state.update(kv)
    json.dump(state, open(STATE, "w"))


if PHASE == "rm1":
    check("RM-01", "node-a received node-b's cameras and speaker",
          lambda: (all(has_resource("node-a", g) for g in (ORPHAN, MOVED, SPEAKER)),
                   "resources=%s" % [g for g in (ORPHAN, MOVED, SPEAKER) if has_resource("node-a", g)]))
    check("RM-02", "node-a sees node-b as owner of both cameras",
          lambda: (active_owners("node-a", ORPHAN) == ["node-b"] and active_owners("node-a", MOVED) == ["node-b"],
                   "orphan=%s moved=%s" % (active_owners("node-a", ORPHAN), active_owners("node-a", MOVED))))
    check("RM-03", "node-a serves sync changes/snapshot to node-b, a member",
          lambda: (sync_status("node-a", "node-b") == [200, 200], "status=%s" % sync_status("node-a", "node-b")),
          timeout=10)

elif PHASE == "rm2":
    check("RM-10", "node-b is decommissioned after the 412 answers",
          lambda: (decommissioned_since("node-b") > 0, "decommissioned_since=%d" % decommissioned_since("node-b")))
    save_state(decommissioned_ms=decommissioned_since("node-b") * 1000)
    check("RM-11", "node-b stopped its 2 cameras and removed its speaker",
          lambda: (log_has("node-b", "Media server decommissioned: 2 cameras stopped, 1 speakers removed"),
                   "decommission summary line not found in node-b stdout"), timeout=10)
    seq = own_max_seq("node-b")
    hold("RM-12", "node-b appends nothing to its own transaction_log while decommissioned",
         lambda: (own_max_seq("node-b") == seq, "max seq %d -> %d" % (seq, own_max_seq("node-b"))))
    check("RM-18", "node-a refuses sync changes/snapshot to node-b once it is removed",
          lambda: (sync_status("node-a", "node-b") == [403, 403], "status=%s" % sync_status("node-a", "node-b")),
          timeout=10)
    check("RM-13", "node-a owns the moved camera",
          lambda: (active_owners("node-a", MOVED) == ["node-a"], "owners=%s" % active_owners("node-a", MOVED)))
    check("RM-14", "node-a cleaned up node-b's ESC data after removedPeerCleanupSec",
          lambda: (log_has("node-a", "Cleaned up ESC data of removed media server node-b"),
                   "cleanup line not found in node-a stdout"))
    check("RM-15", "deleted camera and speaker are gone from node-a's ESC",
          lambda: (not has_resource("node-a", ORPHAN) and not has_resource("node-a", SPEAKER),
                   "orphan=%s speaker=%s" % (has_resource("node-a", ORPHAN), has_resource("node-a", SPEAKER))),
          timeout=5)
    check("RM-16", "moved camera kept on node-a's ESC, no open assignment of node-b left",
          lambda: (has_resource("node-a", MOVED) and not open_assignments_of("node-a", "node-b"),
                   "moved=%s open=%s" % (has_resource("node-a", MOVED), open_assignments_of("node-a", "node-b"))),
          timeout=5)
    check("RM-17", "removal record of node-b is gone",
          lambda: (not query("node-a", "SELECT key FROM misc_data WHERE key LIKE 'REMOVED_PEER:%'"),
                   "rows=%s" % query("node-a", "SELECT key FROM misc_data WHERE key LIKE 'REMOVED_PEER:%'")),
          timeout=5)
    save_state(rejoin_ms=int(time.time() * 1000))

elif PHASE == "rm3":
    state = load_state()
    check("RM-20", "node-b leaves the decommissioned state once listed again",
          lambda: (decommissioned_since("node-b") == 0 and log_has("node-b", "Media server is back in the cluster"),
                   "decommissioned_since=%d" % decommissioned_since("node-b")))
    check("RM-21", "node-b catches up with the cleanup (deleted camera and speaker gone, moved camera on node-a)",
          lambda: (not has_resource("node-b", ORPHAN) and not has_resource("node-b", SPEAKER)
                   and active_owners("node-b", MOVED) == ["node-a"] and not open_assignments_of("node-b", "node-b"),
                   "orphan=%s speaker=%s moved=%s open_b=%s" % (
                       has_resource("node-b", ORPHAN), has_resource("node-b", SPEAKER),
                       active_owners("node-b", MOVED), open_assignments_of("node-b", "node-b"))))
    def cursors_converged():
        a = own_max_seq("node-a")
        b_cursor = query("node-b", "SELECT sequence FROM transaction_sequence WHERE peer_guid='node-a'")
        b_cursor = b_cursor[0][0] if b_cursor else 0
        return a > 0 and b_cursor == a, "node-a seq=%d, node-b cursor for node-a=%d" % (a, b_cursor)
    check("RM-23", "node-b pulls node-a's cleanup entries after the rejoin", cursors_converged, timeout=60)

    def moved_camera_intact():
        kv = {n: query(n, "SELECT count(*) FROM vms_kvpair WHERE resource_guid=?", (MOVED,))[0][0] for n in ("node-a", "node-b")}
        ok = all(has_resource(n, MOVED) and kv[n] > 0 for n in kv) and active_owners("node-a", MOVED) == ["node-a"]
        return ok, "resource a/b=%s/%s kv=%s owner=%s" % (has_resource("node-a", MOVED), has_resource("node-b", MOVED),
                                                          kv, active_owners("node-a", MOVED))
    # Regression: node-b used to apply its config on stale ESC data right after the rejoin and delete the
    # ESC record of the camera node-a took over meanwhile.
    check("RM-24", "node-b caught up before applying its config", lambda: (
        log_has("node-b", "catching up with the cluster before applying its config"), "catch-up line not found"), timeout=5)
    hold("RM-25", "moved camera keeps its ESC resource and config on both nodes after the rejoin", moved_camera_intact)
    lo, hi = state.get("decommissioned_ms", 0), state.get("rejoin_ms", 0)
    leaked = query("node-a", "SELECT count(*) FROM transaction_log WHERE peer_guid='node-b' AND timestamp BETWEEN ? AND ?",
                   (lo + 1000, hi))
    n = leaked[0][0] if leaked else -1
    print("%s RM-22: no node-b entry written while decommissioned reached node-a%s" %
          ("PASS" if lo and hi and n == 0 else "FAIL", "" if n == 0 else " -- %d entries in [%d, %d]" % (n, lo, hi)))
    if not (lo and hi and n == 0):
        failed += 1

sys.exit(failed)
