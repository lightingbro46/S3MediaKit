#!/usr/bin/env python3
"""Checks of the speaker config sync scenario (run_cluster.sh <dir> speaker).

Usage: check_speaker.py <cluster_dir> <sp1|sp2|sp3|sp4|sp5|sp6>
Each check polls both ESC databases until it holds or SP_WAIT_SEC (default 60) runs out.
Exit code: number of failed checks.
"""
import os
import sqlite3
import sys
import time

CLUSTER, PHASE = sys.argv[1], sys.argv[2]
WAIT_SEC = int(os.environ.get("SP_WAIT_SEC", "60"))
# How long a "must not happen" check keeps watching once the node has applied its config.
SETTLE_SEC = int(os.environ.get("SP_SETTLE_SEC", "20"))
NODES = ("node-a", "node-b")
SPEAKER, SPEAKER_OFF, CAMERA_OFF = "spk-sync-1", "spk-sync-off", "cam-sync-off"
SPEAKER_XTYPE = "76cfa478e99445cb8eb5beb00438d46e"

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


def resource(node, guid):
    rows = query(node, "SELECT parent_guid, name, url, xtype_guid FROM vms_resource WHERE guid=?", (guid,))
    return rows[0] if rows else None


def kv(node, guid):
    return dict(query(node, "SELECT name, value FROM vms_kvpair WHERE resource_guid=?", (guid,)))


def active_owner(node, guid):
    rows = query(node, "SELECT owner_peer_id FROM vms_resource_assignment WHERE resource_guid=? AND released_at=0",
                 (guid,))
    return rows[0][0] if rows else None


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


def on_both(fn):
    def pred():
        details = []
        for n in NODES:
            ok, d = fn(n)
            if not ok:
                details.append("%s: %s" % (n, d))
        return not details, "; ".join(details)
    return pred


def speaker_is(parent, name):
    def fn(n):
        r, k = resource(n, SPEAKER), kv(n, SPEAKER)
        ok = (r is not None and r[0] == parent and k.get("name") == name
              and k.get("preferedMediaServer") == parent)
        return ok, "resource=%s name=%s preferedMediaServer=%s" % (r, k.get("name"), k.get("preferedMediaServer"))
    return on_both(fn)


def absent(guid):
    return on_both(lambda n: (resource(n, guid) is None and not kv(n, guid),
                              "resource=%s kv=%d" % (resource(n, guid), len(kv(n, guid)))))


if PHASE == "sp1":
    check("SP-01", "speaker of node-a is in both ESC databases", speaker_is("node-a", SPEAKER))
    check("SP-02", "resource type Speaker, url and playback mode synced", on_both(lambda n: (
        resource(n, SPEAKER) is not None and resource(n, SPEAKER)[3] == SPEAKER_XTYPE
        and resource(n, SPEAKER)[2] == "http://127.0.0.1:9" and kv(n, SPEAKER).get("audioPlaybackMode") == "1",
        "resource=%s audioPlaybackMode=%s" % (resource(n, SPEAKER), kv(n, SPEAKER).get("audioPlaybackMode")))))
    check("SP-03", "ONVIF account is not synced", on_both(lambda n: (
        not any(k.startswith("onvif") for k in kv(n, SPEAKER)), "keys=%s" % sorted(kv(n, SPEAKER)))))
elif PHASE == "sp2":
    check("SP-10", "renamed speaker reaches both nodes", speaker_is("node-a", "spk-renamed"))
elif PHASE in ("sp3", "sp4"):
    tag = "SP-2" if PHASE == "sp3" else "SP-3"
    if check(tag + "0", "speaker moved to node-b in both ESC databases", speaker_is("node-b", "spk-renamed")):
        hold(tag + "1", "node-a dropping the speaker keeps node-b's record", speaker_is("node-b", "spk-renamed"))
elif PHASE == "sp5":
    # The camera's assignment proves node-b applied its new config (ownership is still written).
    if check("SP-40", "node-b runs %s (assignment written with sync_config_to_db=0)" % CAMERA_OFF,
             lambda: (active_owner("node-b", CAMERA_OFF) == "node-b", "owner=%s" % active_owner("node-b", CAMERA_OFF))):
        hold("SP-41", "camera config is not written (camera.sync_config_to_db=0)", absent(CAMERA_OFF))
        hold("SP-42", "new speaker is not written (speaker.sync_config_to_db=0)", absent(SPEAKER_OFF))
        hold("SP-43", "speaker rename stays local", speaker_is("node-b", "spk-renamed"))
elif PHASE == "sp6":
    check("SP-50", "deleted speaker (resource + kvpairs) is removed from both ESC databases", absent(SPEAKER))
else:
    print("unknown phase %s" % PHASE)
    sys.exit(1)

sys.exit(failed)
