#!/usr/bin/env python3
"""Compare the synced ESC tables of node-a and node-b (password values are masked)."""
import os
import sqlite3
import sys

CLUSTER = sys.argv[1]
TABLES = {
    "vms_resource": "SELECT guid, parent_guid, name, url FROM vms_resource ORDER BY guid",
    "vms_kvpair": "SELECT resource_guid, name, CASE WHEN name='password' THEN '***' ELSE value END "
                  "FROM vms_kvpair ORDER BY resource_guid, name",
    "vms_resource_status": "SELECT guid, status FROM vms_resource_status ORDER BY guid",
    "vms_resource_assignment": "SELECT assignment_guid, resource_guid, owner_peer_id, assigned_at, released_at, "
                               "assign_type FROM vms_resource_assignment ORDER BY assigned_at, assignment_guid",
    "transaction_sequence": "SELECT peer_guid, db_guid, sequence FROM transaction_sequence ORDER BY peer_guid",
}


def dump(node):
    con = sqlite3.connect("file:%s?mode=ro" % os.path.join(CLUSTER, node, "db", "esc.sqlite"), uri=True)
    con.execute("PRAGMA busy_timeout=3000")
    out = {t: con.execute(q).fetchall() for t, q in TABLES.items()}
    out["log_count"] = con.execute("SELECT peer_guid, count(*), max(sequence), sum(timestamp_hi=2) "
                                   "FROM transaction_log GROUP BY peer_guid").fetchall()
    con.close()
    return out


a, b = dump("node-a"), dump("node-b")
all_same = True
for t in list(TABLES) + ["log_count"]:
    same = a[t] == b[t]
    # transaction_log legitimately differs: rows received through a snapshot are not copied.
    informational = t == "log_count"
    if not informational:
        all_same &= same
    print("== %s: %s%s" % (t, "SAME" if same else "DIFF", " (info)" if informational else ""))
    if not same or "-v" in sys.argv:
        print("  node-a:", a[t])
        print("  node-b:", b[t])
print("ALL_SAME" if all_same else "DIVERGED")
