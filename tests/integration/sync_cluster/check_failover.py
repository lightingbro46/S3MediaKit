#!/usr/bin/env python3
"""Checks of the failover scenario (run_cluster.sh <dir> failover).

Usage: check_failover.py <cluster_dir> <step>
    a_live      camera cam-fo-1 recorded by node-a (its primary server)
    b_failover  node-a is offline, the service handed the camera to node-b
    a_back      node-a is online again and took the camera back, node-b was told to drop it
    retention   node-b keeps what it recorded during the failover until the camera's
                keepArchivedMaxFor expires. With FO_RETENTION_WAIT=1 it also waits for that
                expiry (hours) and checks that the data is then removed.

Timestamps of the steps are kept in <cluster_dir>/failover_state.json. Every check prints one
`PASS|FAIL|INFO <id> <text>` line; the exit status is the number of FAIL lines.
Needs ffmpeg/ffprobe; nodes run with manager.enableAuthorize=0, so no JWT is involved.
"""
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

WORK, STEP = sys.argv[1], sys.argv[2]
CAM = "cam-fo-1"
MAIN = CAM + "-main"
PORT = {"node-a": 18081, "node-b": 18082}
RTSP_PORT = {"node-a": 18551, "node-b": 18552}
STATE_PATH = os.path.join(WORK, "failover_state.json")
SOURCE_STATE = os.path.join(WORK, "rtsp_state.json")
KEEP_SEC = int(os.environ.get("FO_KEEP_HOURS", "1")) * 3600

failures = 0


def report(ok, sid, text, detail=""):
    global failures
    if ok is None:
        tag = "INFO"
    else:
        tag = "PASS" if ok else "FAIL"
        failures += 0 if ok else 1
    print("%s %s %s%s" % (tag, sid, text, (" — " + detail) if detail else ""), flush=True)


def load_state():
    try:
        with open(STATE_PATH) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def save_state(state):
    with open(STATE_PATH + ".tmp", "w") as fh:
        json.dump(state, fh, indent=2)
    os.replace(STATE_PATH + ".tmp", STATE_PATH)


# ---------------------------------------------------------------- HTTP
def http_get(node, path, params=None, timeout=30):
    """Return (status, body bytes); status 0 means the node could not be reached."""
    url = "http://127.0.0.1:%d%s" % (PORT[node], path)
    if params:
        url += "?" + urllib.parse.urlencode(params)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except (urllib.error.URLError, OSError):
        return 0, b""


def http_json(node, path, params=None, timeout=30):
    status, body = http_get(node, path, params, timeout)
    try:
        return status, json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        return status, {}


def node_online(node):
    return http_get(node, "/media/mserver/healthcheck", timeout=5)[0] == 200


def node_secret(node):
    # MediaServer dumps config.ini with CRLF line endings.
    try:
        with open(os.path.join(WORK, node, "config.ini"), errors="replace") as fh:
            for line in fh:
                if line.startswith("secret="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def live_stream(node):
    """(app, stream) row of cam-fo-1 main in getMediaList, merged over schemas, or None."""
    _, obj = http_json(node, "/index/api/getMediaList", {"secret": node_secret(node)})
    row = None
    for r in obj.get("data") or []:
        if r.get("app") == CAM and r.get("stream") == MAIN:
            row = row or {"recording": False, "alive": 0}
            row["recording"] |= bool(r.get("isRecordingMP4"))
            row["alive"] = max(row["alive"], r.get("aliveSecond", 0) or 0)
    return row


def timeline(node, start, end):
    """periods [(start, end, mediaServerId)] of /media/esc/recordedTimePeriod (periodType=1)."""
    status, obj = http_json(node, "/media/esc/recordedTimePeriod",
                            {"cameraId": CAM, "startTime": int(start), "endTime": int(end),
                             "periodType": 1, "detail": 0}, timeout=60)
    if status != 200 or obj.get("code") != 0:
        return None, "http=%s code=%s msg=%s" % (status, obj.get("code"), obj.get("msg"))
    out = []
    for p in (obj.get("data") or {}).get("periods") or []:
        s = int(p.get("startTime", 0))
        out.append((s, s + int(p.get("duration", 0)), p.get("mediaServerId", "")))
    return sorted(out), ""


def overlap(periods, lo, hi, server=None):
    total = 0
    for s, e, srv in periods:
        if server is None or srv == server:
            total += max(0, min(e, hi) - max(s, lo))
    return total


def source_playing():
    """RTSP sessions currently playing from rtsp_source.py (i.e. servers pulling the camera)."""
    try:
        with open(SOURCE_STATE) as fh:
            st = json.load(fh)
    except (OSError, ValueError):
        return None
    return st.get("playing") if time.time() - st.get("at", 0) < 30 else None


# ---------------------------------------------------------------- media
def ffprobe(url, timeout=20):
    cmd = ["ffprobe", "-v", "error", "-rw_timeout", str(timeout * 1000000),
           "-show_entries", "stream=codec_type,width,height", "-of", "json"]
    if url.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd.append(url)
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=timeout + 5, text=True)
    except subprocess.TimeoutExpired:
        return "timeout"
    if out.returncode != 0:
        return (out.stderr.strip().splitlines() or ["rc=%d" % out.returncode])[-1][:200]
    for s in json.loads(out.stdout or "{}").get("streams", []):
        if s.get("codec_type") == "video":
            return "%sx%s" % (s.get("width"), s.get("height"))
    return "no video track"


def decode(url, media_sec):
    """Decode media_sec seconds; return (ok, detail)."""
    cmd = ["ffmpeg", "-hide_banner", "-nostats", "-loglevel", "error", "-rw_timeout", "20000000",
           "-i", url, "-t", str(media_sec), "-map", "0:v:0", "-f", "null", "-"]
    t0 = time.time()
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=media_sec * 6 + 30, text=True)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    errs = [l for l in out.stderr.splitlines() if l.strip()]
    return out.returncode == 0, "%.1fs wall, rc=%d %s" % (time.time() - t0, out.returncode,
                                                          " | ".join(errs[-2:])[:200])


def media_url(node, path):
    return "http://127.0.0.1:%d%s" % (PORT[node], path)


def file_start(path):
    """Epoch of a recording from `YYYY-MM-DD-HH-MM-SS-<n>.mp4[.enc]` (local time), else mtime."""
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-(\d{2})", os.path.basename(path))
    if m:
        return int(time.mktime(tuple(int(x) for x in m.groups()) + (0, 0, -1)))
    return int(os.path.getmtime(path))


def recordings(node):
    """{path: start epoch} of cam-fo-1 recordings under the node directory."""
    out = {}
    pattern = os.path.join(WORK, node, "**", CAM, "**", "*.mp4*")
    for f in glob.glob(pattern, recursive=True):
        try:
            out[f] = file_start(f)
        except OSError:
            pass
    return out


def assignments(node):
    """vms_resource_assignment rows of cam-fo-1 in the node's ESC db."""
    try:
        con = sqlite3.connect("file:%s?mode=ro" % os.path.join(WORK, node, "db", "esc.sqlite"), uri=True)
        con.execute("PRAGMA busy_timeout=3000")
        rows = con.execute("SELECT owner_peer_id, assigned_at, released_at, assign_type "
                           "FROM vms_resource_assignment WHERE resource_guid=? ORDER BY assigned_at",
                           (CAM,)).fetchall()
        con.close()
        return rows
    except sqlite3.Error as exc:
        return "error: %s" % exc


def kv_value(node, name):
    try:
        con = sqlite3.connect("file:%s?mode=ro" % os.path.join(WORK, node, "db", "esc.sqlite"), uri=True)
        con.execute("PRAGMA busy_timeout=3000")
        row = con.execute("SELECT value FROM vms_kvpair WHERE resource_guid=? AND name=?",
                          (CAM, name)).fetchone()
        con.close()
        return row[0] if row else None
    except sqlite3.Error as exc:
        return "error: %s" % exc


def check_source(sid, expected):
    playing = source_playing()
    report(playing == expected, sid, "camera (rtsp_source) has exactly %d pulling server" % expected,
           "playing=%s" % playing)


# ---------------------------------------------------------------- steps
def step_a_live(state):
    row = live_stream("node-a")
    report(bool(row), "FO-01", "node-a pulls the camera", str(row))
    report(bool(row and row["recording"]), "FO-02", "node-a records the camera")
    recs = recordings("node-a")
    report(len(recs) > 0, "FO-03", "node-a has recordings of the camera", "%d file" % len(recs))
    check_source("FO-04", 1)
    state["a_files"] = len(recs)


def step_b_failover(state):
    now = int(time.time())
    t_b = state["t_b_start"]
    report(not node_online("node-a"), "FO-10", "node-a is offline")

    row = live_stream("node-b")
    report(bool(row), "FO-11", "node-b pulls the camera in failover mode", str(row))
    report(bool(row and row["recording"]), "FO-12", "node-b records the camera")
    check_source("FO-13", 1)

    rows = assignments("node-b")
    active = [r for r in rows if r[2] == 0] if isinstance(rows, list) else []
    report(len(active) == 1 and active[0][0] == "node-b" and active[0][3] == 1, "FO-14",
           "node-b DB: the only active assignment is node-b/FAILOVER", str(rows))

    # conf/config.ini keeps enable_rtmp=0, so there is no FLV muxer to try.
    for sid, url, kind in (("FO-15", media_url("node-b", "/media/live/%s/%s.live.mp4" % (CAM, MAIN)), "http-fmp4"),
                           ("FO-16", "rtsp://127.0.0.1:%d/%s/%s" % (RTSP_PORT["node-b"], CAM, MAIN), "rtsp")):
        res = ffprobe(url)
        report("x" in res and res[0].isdigit(), sid, "live %s from node-b" % kind, res)

    recs = {f: s for f, s in recordings("node-b").items() if s >= t_b - 5}
    report(len(recs) > 0, "FO-17", "node-b recorded the camera after the failover", "%d file" % len(recs))

    periods, err = timeline("node-b", t_b - 60, now)
    b_sec = overlap(periods or [], t_b, now, "node-b")
    report(b_sec > 0, "FO-18", "timeline on node-b covers the failover window with node-b data",
           err or "%ds of %ds, periods=%s" % (b_sec, now - t_b, periods))

    stamp = None
    for s, e, srv in periods or []:
        if srv == "node-b" and e - s >= 10:
            stamp = s + 3
            break
    if stamp is None:
        report(False, "FO-19", "replay from node-b", "no node-b period of 10s or more")
    else:
        ok, detail = decode(media_url("node-b", "/media/record/%s/%s/vod/%d.live.mp4" % (CAM, MAIN, stamp)), 5)
        report(ok, "FO-19", "replay from node-b at %d (failover recording)" % stamp, detail)
        state["b_stamp"] = stamp


def step_a_back(state):
    now = int(time.time())
    t_a, t_b, t_back = state["t_a_start"], state["t_b_start"], state["t_back"]

    row = live_stream("node-a")
    report(bool(row), "FO-20", "node-a pulls the camera again", str(row))
    report(bool(row and row["recording"]), "FO-21", "node-a records the camera again")
    row_b = live_stream("node-b")
    report(row_b is None, "FO-22", "node-b no longer pulls the camera", str(row_b))
    check_source("FO-23", 1)

    for node in ("node-a", "node-b"):
        rows = assignments(node)
        active = [r for r in rows if r[2] == 0] if isinstance(rows, list) else []
        report(len(active) == 1 and active[0][0] == "node-a", "FO-24",
               "%s DB: the only active assignment is node-a" % node, str(rows))

    periods, err = timeline("node-a", t_a - 60, now)
    b_sec = overlap(periods or [], t_b, t_back, "node-b")
    a_sec = overlap(periods or [], t_a, now, "node-a")
    window = t_back - t_b
    # node-b needs a few seconds to connect and start its first segment; half the window is plenty.
    report(periods is not None and b_sec >= window // 2, "FO-25",
           "timeline on node-a returns node-b data for the failover window",
           err or "node-b %ds of %ds window, node-a %ds, periods=%s" % (b_sec, window, a_sec, periods))
    report(a_sec > 0, "FO-26", "timeline on node-a still returns its own data")

    stamp = state.get("b_stamp")
    if stamp:
        status, body = http_get("node-a", "/media/esc/recordedThumnail", {"cameraId": CAM, "pos": stamp}, 60)
        report(status == 200 and body[:2] == b"\xff\xd8", "FO-27",
               "thumbnail on node-a at %d is forwarded to node-b" % stamp, "http=%d %d bytes" % (status, len(body)))
        ok, detail = decode(media_url("node-b", "/media/record/%s/%s/vod/%d.live.mp4" % (CAM, MAIN, stamp)), 5)
        report(ok, "FO-28", "replay from node-b of the failover recording after node-a is back", detail)
        # A VOD URL is not forwarded between nodes: the player picks the node from the period's
        # mediaServerId. Informational only.
        ok, detail = decode(media_url("node-a", "/media/record/%s/%s/vod/%d.live.mp4" % (CAM, MAIN, stamp)), 3)
        report(None, "FO-29", "replay of the failover recording through node-a: %s" % ("ok" if ok else "not served"), detail)

    state["b_files"] = {f: s for f, s in recordings("node-b").items() if t_b - 5 <= s <= t_back}


def step_retention(state):
    t_back = state["t_back"]
    kept = state.get("b_files") or {}
    report(len(kept) > 0, "FO-30", "node-b recorded files during the failover", "%d file" % len(kept))

    keep = kv_value("node-b", "keepArchivedMaxFor")
    report(keep == str(KEEP_SEC), "FO-31", "node-b ESC keepArchivedMaxFor = %ds" % KEEP_SEC, "value=%s" % keep)

    def premature(now):
        # A file may go once its start is older than the retention; allow a minute of slack.
        present = recordings("node-b")
        return [f for f, s in kept.items() if f not in present and now < s + KEEP_SEC - 60]

    missing = premature(time.time())
    report(not missing, "FO-32", "node-b keeps the failover recordings before retention expiry",
           "%d file removed early: %s" % (len(missing), missing[:3]) if missing else "")

    periods, err = timeline("node-a", state["t_b_start"], t_back)
    b_sec = overlap(periods or [], state["t_b_start"], t_back, "node-b")
    report(b_sec > 0, "FO-33", "timeline on node-a still returns node-b data", err or "%ds" % b_sec)

    if os.environ.get("FO_RETENTION_WAIT") != "1":
        report(None, "FO-34", "retention expiry not awaited (set FO_RETENTION_WAIT=1, takes > %dh)"
               % (KEEP_SEC // 3600))
        return

    # Expiry is age based: removal is due once the newest failover file is KEEP_SEC old. The
    # cutoff is aligned down to storage.tieringSplitGranularitySeconds and applied by a periodic
    # cycle, hence the grace.
    grace = int(os.environ.get("FO_RETENTION_GRACE_SEC", "5400"))
    deadline = max(kept.values()) + KEEP_SEC + grace
    early = set()
    print("waiting until %s for node-b retention (poll 60s)" % time.strftime("%H:%M:%S", time.localtime(deadline)),
          flush=True)
    while time.time() < deadline:
        early.update(premature(time.time()))
        present = recordings("node-b")
        if not any(f in present for f in kept):
            break
        time.sleep(60)
    report(not early, "FO-35", "no failover recording removed before its retention expired",
           "%d early: %s" % (len(early), sorted(early)[:3]) if early else "")
    left = [f for f in kept if f in recordings("node-b")]
    report(not left, "FO-36", "node-b removed the failover recordings after retention expired",
           "%d file left: %s" % (len(left), left[:3]) if left else "")
    periods, err = timeline("node-a", state["t_b_start"], t_back)
    b_sec = overlap(periods or [], state["t_b_start"], t_back, "node-b")
    report(b_sec == 0, "FO-37", "timeline on node-a no longer returns expired node-b data",
           err or "%ds, periods=%s" % (b_sec, periods))


def main():
    state = load_state()
    steps = {"a_live": step_a_live, "b_failover": step_b_failover,
             "a_back": step_a_back, "retention": step_retention}
    if STEP.startswith("mark:"):
        # run_cluster.sh records when each service decision was applied.
        state[STEP[5:]] = int(time.time())
        save_state(state)
        return 0
    if STEP not in steps:
        print("unknown step %s" % STEP)
        return 2
    print("---- failover check: %s" % STEP, flush=True)
    steps[STEP](state)
    save_state(state)
    return failures


if __name__ == "__main__":
    sys.exit(main())
