#!/usr/bin/env python3
"""Health monitor checks of the 2-node cluster (run_cluster.sh <dir> monitor).

Usage: check_monitor.py <cluster_dir> <step> [timeout_sec]
  steps: m1_ok           node-a sees node-b, sync caught up, database checked, cluster ok
         m2_peer_down    node-b stopped: node-a cluster warning (cluster.peers_unreachable), not critical
         m3_peer_back    node-b back: node-a cluster ok again
         m4_hook_down    hook API stopped: webhook failures, cluster warning (webhook.failure_pct)
         m5_pool_ok      MINIO pool on fake_s3.py (port 19000) created: pool OK, storage not critical
         m6_pool_down    fake_s3.py stopped: pool OFFLINE, storage critical (pool.health)
Polls GET /media/monitor/summary on node-a with the api.secret header until the step holds.
"""
import json
import os
import sys
import time
import urllib.request

CLUSTER, STEP = sys.argv[1], sys.argv[2]
TIMEOUT = int(sys.argv[3]) if len(sys.argv) > 3 else 180
URL = "http://127.0.0.1:18081/media/monitor/summary"


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


def api(path, payload):
    req = urllib.request.Request("http://127.0.0.1:18081" + path, data=json.dumps(payload).encode(),
                                 headers={"secret": secret(), "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())


def prepare():
    if STEP == "m5_pool_ok":
        # Database checks every 10 s instead of 60 s (monitor.slowIntervalSec is editable).
        print("config/set:", api("/media/monitor/config/set", {"monitor.slowIntervalSec": 10}).get("code"))
        pool = {"name": "monitor-fake-s3", "type": "MINIO", "tier": "COLD", "endpoint": "http://127.0.0.1:19000",
                "bucket": "monitor", "base_path": "monitor", "access_key": "probe", "secret_key": "probe",
                "health_check_enabled": True}
        ret = api("/media/mserver/storage/pool/create", pool)
        print("pool/create: code=%s msg=%s" % (ret.get("code"), ret.get("msg")))


def fake_pool(d):
    for p in d["storage"]["tier"].get("pools", []):
        if p["name"] == "monitor-fake-s3":
            return p
    return None


def m5_pool_ok(d):
    p = fake_pool(d)
    return (p is not None and p["health_status"] == "OK" and not p["health_stale"]
            and d["components"]["storage"] != "critical")


def m6_pool_down(d):
    p = fake_pool(d)
    return (p is not None and p["health_status"] == "OFFLINE" and d["components"]["storage"] == "critical"
            and "pool.health" in reasons(d, "storage"))


def reasons(d, component):
    return [r["metric"] for r in d.get("reasons", []) if r.get("component") == component]


def m1_ok(d):
    cl = d["cluster"]
    peers = {p["id"]: p for p in cl["sync"].get("peers", [])}
    return (cl["peers_known"] == 1 and cl["peers_unreachable"] == 0 and cl["sync"].get("bootstrapped")
            and "node-b" in peers and peers["node-b"]["last_ok_age_sec"] < 60
            and d["storage"]["database"].get("write_ms") is not None
            and d["components"]["cluster"] == "ok")


def m2_peer_down(d):
    return (d["cluster"]["peers_unreachable"] == 1 and d["components"]["cluster"] == "warning"
            and "cluster.peers_unreachable" in reasons(d, "cluster"))


def m3_peer_back(d):
    return d["cluster"]["peers_unreachable"] == 0 and d["components"]["cluster"] == "ok"


def m4_hook_down(d):
    return (d["cluster"]["webhook"]["failures"] > 0 and d["components"]["cluster"] == "warning"
            and "webhook.failure_pct" in reasons(d, "cluster"))


STEPS = {"m1_ok": m1_ok, "m2_peer_down": m2_peer_down, "m3_peer_back": m3_peer_back, "m4_hook_down": m4_hook_down,
         "m5_pool_ok": m5_pool_ok, "m6_pool_down": m6_pool_down}


def brief(d):
    cl = d["cluster"]
    pool = fake_pool(d) or {}
    return ("overall=%s cluster=%s storage=%s pool=%s peers=%s/%s sync=%s webhook=%s db_ms=%s reasons=%s" % (
        d["overall"], d["components"]["cluster"], d["components"]["storage"], pool.get("health_status"),
        cl["peers_unreachable"],
        cl["peers_known"], [(p["id"], p["last_ok_age_sec"]) for p in cl["sync"].get("peers", [])],
        {k: cl["webhook"][k] for k in ("requests", "failures")}, d["storage"]["database"].get("write_ms"),
        [r["component"] + "/" + r["metric"] for r in d.get("reasons", [])]))


def main():
    check = STEPS[STEP]
    prepare()
    start, last = time.time(), None
    while time.time() - start < TIMEOUT:
        try:
            last = summary()
            if check(last):
                # The cluster component never takes the node to critical here.
                if last["components"]["cluster"] == "critical":
                    break
                print("PASS %s after %.0fs: %s" % (STEP, time.time() - start, brief(last)))
                return 0
        except Exception as ex:  # node restarting
            last = {"error": str(ex)}
        time.sleep(5)
    print("FAIL %s: %s" % (STEP, brief(last) if last and "error" not in last else last))
    return 1


if __name__ == "__main__":
    sys.exit(main())
