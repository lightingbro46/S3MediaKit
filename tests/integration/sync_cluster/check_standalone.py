#!/usr/bin/env python3
"""Standalone servers (serverLocationId 0) joining a server group (run_cluster.sh <dir> standalone).

Usage: check_standalone.py <cluster_dir> <step>
  steps: sa1  both nodes standalone: no ESC sync (summary state, sync API refused), nothing logged,
              the changes are kept for later, each ESC database has only its own camera
         sa2  both nodes in server group 1: sync runs, the changes made while standalone are replayed,
              each node has the other's camera
         sa3  back to 0: sync stopped again, new changes not logged
Reads the nodes' ESC databases and GET /media/monitor/summary (api.secret header).
"""
import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CLUSTER, STEP = sys.argv[1], sys.argv[2]
WAIT_SEC = int(os.environ.get("SA_WAIT_SEC", "180"))
NODES = ("node-a", "node-b")
HTTP_PORT = {"node-a": 18081, "node-b": 18082}
CAMERA = {"node-a": "cam-sa-a", "node-b": "cam-sa-b"}
OTHER = {"node-a": "node-b", "node-b": "node-a"}

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


def own_log_count(node):
    rows = query(node, "SELECT count(*) FROM transaction_log WHERE peer_guid=?", (node,))
    return rows[0][0] if rows else 0


def recorded_changes(node):
    rows = query(node, "SELECT count(*) FROM misc_data WHERE key LIKE 'STANDALONE_CHANGE:%'")
    return rows[0][0] if rows else 0


def secret(node):
    # MediaServer dumps config.ini with CRLF line endings.
    for line in open(os.path.join(CLUSTER, node, "config.ini"), encoding="utf-8", errors="replace"):
        if line.startswith("secret="):
            return line.split("=", 1)[1].strip()
    return ""


def summary(node):
    req = urllib.request.Request("http://127.0.0.1:%d/media/monitor/summary" % HTTP_PORT[node],
                                 headers={"secret": secret(node)})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            body = json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return {}
    return body.get("data", {}) if body.get("code") == 0 else {}


def sync_state(node):
    d = summary(node)
    return d.get("standalone"), d.get("cluster", {}).get("sync", {}).get("state")


def changes_status(target, requester):
    """HTTP status of /media/esc/sync/changes on `target` called as `requester`."""
    body = json.dumps({"peer": requester, "db": "db-check", "limit": 1, "cursors": [], "ack_cursors": []}).encode()
    req = urllib.request.Request("http://127.0.0.1:%d/media/esc/sync/changes" % HTTP_PORT[target], data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status
    except urllib.error.HTTPError as err:
        return err.code
    except (urllib.error.URLError, OSError):
        return 0


def check(code, desc, pred, timeout=None):
    """Poll pred() -> (ok, detail) until ok or timeout."""
    global failed
    deadline = time.time() + (WAIT_SEC if timeout is None else timeout)
    while True:
        ok, detail = pred()
        if ok or time.time() >= deadline:
            break
        time.sleep(2)
    print("%s %s: %s%s" % ("PASS" if ok else "FAIL", code, desc, "" if ok else " -- " + detail))
    if not ok:
        failed += 1
    return ok


if STEP == "sa1":
    for n in NODES:
        check("SA-1 " + n, "summary: standalone, sync state standalone",
              lambda n=n: (sync_state(n) == (True, "standalone"), "got %s" % (sync_state(n),)))
        check("SA-2 " + n, "own camera in its ESC database, the other's not",
              lambda n=n: (has_resource(n, CAMERA[n]) and not has_resource(n, CAMERA[OTHER[n]]),
                           "own=%s other=%s" % (has_resource(n, CAMERA[n]), has_resource(n, CAMERA[OTHER[n]]))))
        check("SA-3 " + n, "no own transaction_log entry, changes recorded for later",
              lambda n=n: (own_log_count(n) == 0 and recorded_changes(n) > 0,
                           "log=%d recorded=%d" % (own_log_count(n), recorded_changes(n))))
        check("SA-4 " + n, "sync/changes refused to the other node (403)",
              lambda n=n: (changes_status(n, OTHER[n]) == 403, "status=%s" % changes_status(n, OTHER[n])),
              timeout=10)
elif STEP == "sa2":
    for n in NODES:
        check("SA-5 " + n, "sync running in server group 1",
              lambda n=n: (sync_state(n) == (False, "running"), "got %s" % (sync_state(n),)))
        check("SA-6 " + n, "the other node's camera (made while standalone) arrived",
              lambda n=n: (has_resource(n, CAMERA[OTHER[n]]), "missing %s" % CAMERA[OTHER[n]]))
        check("SA-7 " + n, "recorded changes replayed to the log",
              lambda n=n: (recorded_changes(n) == 0 and own_log_count(n) > 0,
                           "log=%d recorded=%d" % (own_log_count(n), recorded_changes(n))))
        check("SA-8 " + n, "sync/changes answered to the other node (200)",
              lambda n=n: (changes_status(n, OTHER[n]) == 200, "status=%s" % changes_status(n, OTHER[n])),
              timeout=10)
elif STEP == "sa3":
    for n in NODES:
        check("SA-9 " + n, "standalone again, sync stopped",
              lambda n=n: (sync_state(n) == (True, "standalone"), "got %s" % (sync_state(n),)))
    # Camera status changes (offline source) keep coming: they are recorded, not logged.
    before = {n: own_log_count(n) for n in NODES}
    time.sleep(30)
    for n in NODES:
        check("SA-10 " + n, "no new own transaction_log entry",
              lambda n=n: (own_log_count(n) == before[n], "log=%d before=%d" % (own_log_count(n), before[n])),
              timeout=0)
else:
    print("unknown step %s" % STEP)
    sys.exit(2)
sys.exit(failed)
