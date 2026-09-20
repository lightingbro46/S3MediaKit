#!/usr/bin/env python3
"""Thu thập bằng chứng và kiểm bất biến sau mỗi phase của stress test.

Dùng:
    check_stress.py <work_dir> --phase <tên>   # báo cáo ngắn sau một phase
    check_stress.py <work_dir> --final         # báo cáo tổng + mã thoát pass/fail

Mã thoát: 0 = mọi bất biến đạt, 1 = có bất biến bị vi phạm.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
from collections import defaultdict

# Ngưỡng cho các bất biến "cứng". Nới ở đây nếu máy chạy yếu hơn máy tham chiếu.
THRESHOLDS = {
    # healthcheck chạy thẳng trên poller; nếu p95 vượt ngưỡng nghĩa là có việc
    # blocking đang chiếm poller (xem docs/threading-blocking-work.md)
    "probe_p95_ms": 1000.0,
    "probe_max_ms": 5000.0,
    # tỉ lệ luồng phải lên được, trừ các cohort offline/flaky
    "stream_live_ratio": 0.80,
    # Độ trễ PTZ: healthcheck sạch KHÔNG đủ để kết luận node khoẻ. Khi vài camera ONVIF
    # chậm lấp đầy pool device(), lệnh PTZ tắc hoàn toàn trong khi poller vẫn nhanh và
    # endpoint đọc-cache vẫn nhanh — xem mục V4 của REPORT-2026-09-20.md.
    "ptz_p95_ms": 15000.0,
    "ptz_timeout_ratio": 0.25,
    # số dòng log được phép
    "max_db_locked": 0,
    "max_failed_apply": 0,
}

SYNC_TABLES = {
    "vms_resource": "SELECT guid, parent_guid, name, url FROM vms_resource ORDER BY guid",
    "vms_kvpair": "SELECT resource_guid, name, CASE WHEN name='password' THEN '***' ELSE value END "
                  "FROM vms_kvpair ORDER BY resource_guid, name",
    "vms_resource_status": "SELECT guid, status FROM vms_resource_status ORDER BY guid",
    "vms_resource_assignment": "SELECT resource_guid, owner_peer_id, assign_type "
                               "FROM vms_resource_assignment WHERE released_at IS NULL "
                               "ORDER BY resource_guid",
}


# Chế độ ghi theo RecordMode (manager/Camera/RecordPolicy.h) và hệ quả thực tế ở
# StreamSink.cpp: 0=NoRecord, 1=RecordLowResAndMotion (chỉ luồng phụ ghi liên tục,
# luồng chính chỉ ghi theo sự kiện), 2=RecordOnlyMotion (chỉ luồng chính, theo sự
# kiện), 3=RecordAlways (cả hai luồng ghi liên tục).
# Kịch bản không bơm sự kiện chuyển động nên mọi chế độ "theo sự kiện" => không ghi.
CONTINUOUS_BY_MODE = {
    0: {"main": False, "sub": False},
    1: {"main": False, "sub": True},
    2: {"main": False, "sub": False},
    3: {"main": True, "sub": True},
}

WORK_DAYS = set(range(5))
OFFICE_HOURS = set(range(8, 18))
DAY_HOURS = set(range(6, 20))

# cohort -> hàm trả RecordMode tại (thứ, giờ); phải khớp SCHEDULES trong setup_stress.py
COHORT_MODE = {
    "always": lambda d, h: 3,
    "office": lambda d, h: 3 if (d in WORK_DAYS and h in OFFICE_HOURS) else 0,
    "motion_night": lambda d, h: 3 if h in DAY_HOURS else 2,
    "lowres_motion": lambda d, h: 1,
    "retention_auto": lambda d, h: 3,
    "long_retention": lambda d, h: 3,
    "no_record": lambda d, h: 0,
    "flaky": lambda d, h: 3,
    "offline": lambda d, h: 3,
}

# cohort có cờ notRecordSubStream trong setup_stress.py
NO_SUB_RECORD = {"long_retention"}
# cohort tắt hẳn enableRecording
RECORDING_DISABLED = {"no_record"}
# cohort có nguồn không ổn định: bỏ qua khi đối chiếu lịch
UNSTABLE = {"offline", "flaky"}


# Phase `schedule_flip` hoán hồ sơ của hai cohort office <-> always. Sau phase đó
# phải kiểm theo lịch mới, nếu không toàn bộ hai cohort sẽ bị báo lệch oan — và quan
# trọng hơn: kiểm theo lịch mới chính là cách chứng minh việc đổi lịch đã có hiệu lực.
FLIPPED = {"always": "office", "office": "always"}


def expected_continuous(cohort, stream_kind, weekday, hour, flipped=False):
    """Luồng này có đang phải ghi liên tục theo hồ sơ lập lịch không?"""
    if cohort in RECORDING_DISABLED:
        return False
    schedule_cohort = FLIPPED.get(cohort, cohort) if flipped else cohort
    mode = COHORT_MODE[schedule_cohort](weekday, hour)
    want = CONTINUOUS_BY_MODE[mode][stream_kind]
    if stream_kind == "sub" and cohort in NO_SUB_RECORD:
        return False
    return want


def load_json(path, default=None):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default if default is not None else {}


def nodes_of(work):
    return sorted(d for d in os.listdir(work)
                  if d.startswith("node-") and os.path.isdir(os.path.join(work, d)))


# ------------------------------------------------------------------ log
def scan_logs(work, node):
    """Đếm các mẫu log đáng ngại trong toàn bộ stdout của một node."""
    patterns = {
        "crash": re.compile(r"## signal:"),
        "db_locked": re.compile(r"database is locked"),
        "failed_apply": re.compile(r"Failed to apply"),
        "relay_recv": re.compile(r"Relay received"),
        "relay_fail": re.compile(r"Relay pull from .* failed"),
        "stale_skip": re.compile(r"skip stale"),
        "onvif_fail": re.compile(r"Onvif .*(failed|error)", re.I),
        "reconnect": re.compile(r"[Rr]e-?connect"),
        "exception": re.compile(r"[Ee]xception occurred"),
        "poller_warn": re.compile(r"task cost too much time|执行时间过长"),
    }
    counts = defaultdict(int)
    d = os.path.join(work, node)
    for name in sorted(os.listdir(d)):
        if not name.startswith("stdout"):
            continue
        try:
            with open(os.path.join(d, name), errors="replace") as fh:
                for line in fh:
                    for key, rx in patterns.items():
                        if rx.search(line):
                            counts[key] += 1
        except OSError:
            continue
    return dict(counts)


# ------------------------------------------------------------------ ghi hình
def scan_records(work, node):
    """Đếm file mp4 và dung lượng theo camera dưới thư mục ghi của node."""
    root = os.path.join(work, node, "record")
    per_camera = defaultdict(lambda: {"files": 0, "bytes": 0})
    total_files = total_bytes = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        mp4 = [f for f in filenames if f.endswith(".mp4")]
        if not mp4:
            continue
        # .../record/<app>/<device_id>/<stream_id>/<date>/<file>.mp4
        rel = os.path.relpath(dirpath, root).split(os.sep)
        camera = rel[1] if len(rel) > 1 else rel[0]
        for f in mp4:
            try:
                size = os.path.getsize(os.path.join(dirpath, f))
            except OSError:
                continue
            per_camera[camera]["files"] += 1
            per_camera[camera]["bytes"] += size
            total_files += 1
            total_bytes += size
    return {"total_files": total_files, "total_bytes": total_bytes,
            "cameras_with_record": len(per_camera),
            "per_camera": {k: v for k, v in per_camera.items()}}


def scan_timefiles(work, node):
    """Đếm file index thời gian (.tf/.idx) — StorageManager dựng lại chúng khi dọn."""
    root = os.path.join(work, node, "record")
    count = 0
    for _dirpath, _dirnames, filenames in os.walk(root):
        count += sum(1 for f in filenames if f.endswith(".tf") or f.endswith(".idx"))
    return count


# ------------------------------------------------------------------ db
def node_secret(work, node):
    path = os.path.join(work, node, "config.ini")
    try:
        with open(path, errors="replace") as fh:
            for line in fh:
                if line.startswith("secret="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def media_list(work, node, port):
    """Hỏi thẳng MediaServer danh sách luồng đang sống (nguồn sự thật phía server)."""
    import urllib.error
    import urllib.request
    url = ("http://127.0.0.1:%d/index/api/getMediaList?secret=%s"
           % (port, node_secret(work, node)))
    try:
        with urllib.request.urlopen(url, timeout=20) as resp:
            payload = json.loads(resp.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        return {"error": str(exc)}
    if payload.get("code") != 0:
        return {"error": "code=%s" % payload.get("code")}

    # getMediaList trả một dòng cho mỗi schema (rtsp/hls/fmp4); gộp theo (app, stream)
    streams = {}
    for row in payload.get("data") or []:
        key = (row.get("app", ""), row.get("stream", ""))
        cur = streams.setdefault(key, {"recording": False, "bytes_speed": 0, "alive": 0})
        cur["recording"] |= bool(row.get("isRecordingMP4"))
        cur["bytes_speed"] = max(cur["bytes_speed"], row.get("bytesSpeed", 0) or 0)
        cur["alive"] = max(cur["alive"], row.get("aliveSecond", 0) or 0)
    return {"streams": streams}


def check_schedules(work, inv, streams, flipped=False):
    """Đối chiếu trạng thái ghi thực tế với hồ sơ lập lịch của từng cohort."""
    cohort_of = {}
    for cohort, ids in inv.get("by_cohort", {}).items():
        for cam_id in ids:
            cohort_of[cam_id] = cohort

    # Lịch dùng 0=Thứ Hai (RecordPolicy.cpp::toRecordScheduleDay); tm_wday của Python
    # cũng đã là 0=Thứ Hai nên dùng thẳng.
    now = time.localtime()
    weekday = now.tm_wday
    hour = now.tm_hour

    match = mismatch = skipped = 0
    examples = []
    for (app, stream), info in sorted(streams.items()):
        cohort = cohort_of.get(app)
        if cohort is None:
            skipped += 1
            continue
        if cohort in UNSTABLE:
            skipped += 1
            continue
        kind = "sub" if stream.endswith("-sub") else "main"
        want = expected_continuous(cohort, kind, weekday, hour, flipped)
        got = info["recording"]
        if want == got:
            match += 1
        else:
            mismatch += 1
            if len(examples) < 8:
                examples.append("%s/%s [%s] mong đợi=%s thực tế=%s"
                                % (app, stream, cohort, want, got))
    return {"weekday": weekday, "hour": hour, "match": match, "flipped": flipped,
            "mismatch": mismatch, "skipped": skipped, "examples": examples}


def db_path(work, node, name):
    return os.path.join(work, node, "db", name)


def dump_sync_tables(work, node):
    path = db_path(work, node, "esc.sqlite")
    if not os.path.exists(path):
        return None
    try:
        con = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
        con.execute("PRAGMA busy_timeout=5000")
        out = {}
        for table, query in SYNC_TABLES.items():
            try:
                out[table] = con.execute(query).fetchall()
            except sqlite3.Error:
                out[table] = None
        con.close()
        return out
    except sqlite3.Error as exc:
        return {"error": str(exc)}


def compare_sync(work, node_names):
    if len(node_names) < 2:
        return {"skipped": "chỉ có một node"}
    dumps = {n: dump_sync_tables(work, n) for n in node_names}
    base_name = node_names[0]
    base = dumps[base_name]
    if not base:
        return {"error": "không đọc được esc.sqlite của %s" % base_name}
    result = {"converged": True, "tables": {}}
    for table in SYNC_TABLES:
        rows = {n: (dumps[n] or {}).get(table) for n in node_names}
        same = all(rows[n] == rows[base_name] for n in node_names)
        sizes = {n: (len(rows[n]) if rows[n] is not None else None) for n in node_names}
        result["tables"][table] = {"same": same, "rows": sizes}
        result["converged"] &= same
    return result


def db_size(work, node):
    out = {}
    d = os.path.join(work, node, "db")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if os.path.isfile(p):
                out[f] = os.path.getsize(p)
    return out


# ------------------------------------------------------------------ tài nguyên
def resource_summary(work):
    path = os.path.join(work, "resources.jsonl")
    per_node = defaultdict(list)
    try:
        with open(path) as fh:
            for line in fh:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                per_node[rec["node"]].append(rec)
    except OSError:
        return {}
    out = {}
    for node, recs in per_node.items():
        if not recs:
            continue
        rss = [r["rss_kb"] for r in recs]
        out[node] = {
            "samples": len(recs),
            "rss_start_mb": round(rss[0] / 1024.0, 1),
            "rss_end_mb": round(rss[-1] / 1024.0, 1),
            "rss_peak_mb": round(max(rss) / 1024.0, 1),
            "threads_peak": max(r["threads"] for r in recs),
            "fds_peak": max(r["fds"] for r in recs),
        }
        span = recs[-1]["at"] - recs[0]["at"]
        ticks = recs[-1]["cpu_ticks"] - recs[0]["cpu_ticks"]
        if span > 0:
            out[node]["cpu_pct_avg"] = round(ticks / 100.0 / span * 100.0, 1)
    return out


# ------------------------------------------------------------------ báo cáo
def evaluate(work, phase, final):
    inv = load_json(os.path.join(work, "inventory.json"))
    node_names = nodes_of(work)
    expected_streams = inv.get("streams", 0)
    # cohort offline không bao giờ lên được, flaky thì lên xuống liên tục
    by_cohort = inv.get("by_cohort", {})
    unreachable = len(by_cohort.get("offline", [])) * 2
    reachable = max(1, expected_streams - unreachable)

    failures = []
    report = {"phase": phase, "at": time.strftime("%H:%M:%S"), "nodes": {}}

    for node in node_names:
        logs = scan_logs(work, node)
        records = scan_records(work, node)
        report["nodes"][node] = {
            "logs": logs,
            "record_files": records["total_files"],
            "record_mb": round(records["total_bytes"] / 1048576.0, 1),
            "cameras_with_record": records["cameras_with_record"],
            "timefiles": scan_timefiles(work, node),
            "db_bytes": db_size(work, node),
        }
        if logs.get("crash"):
            failures.append("%s: có stack trace (## signal:) trong log" % node)
        if logs.get("db_locked", 0) > THRESHOLDS["max_db_locked"]:
            failures.append("%s: %d dòng 'database is locked'" % (node, logs["db_locked"]))
        if logs.get("failed_apply", 0) > THRESHOLDS["max_failed_apply"]:
            failures.append("%s: %d dòng 'Failed to apply'" % (node, logs["failed_apply"]))

    # Nguồn sự thật phía server: gộp getMediaList của mọi node
    all_streams = {}
    ports = inv.get("ports", {})
    for node in node_names:
        port = ports.get(node, {}).get("http")
        if not port:
            continue
        ml = media_list(work, node, port)
        if "error" in ml:
            report["nodes"][node]["media_list_error"] = ml["error"]
            continue
        all_streams.update(ml["streams"])
    # Chỉ riêng phase `schedule` chạy với hồ sơ đã hoán. Các phase sau đều gọi lại
    # setup_stress.py phase <...>, mà hàm đó dựng lại camera từ SCHEDULES gốc nên
    # hoán đổi bị hoàn tác — các phase không cộng dồn lên nhau.
    flipped = phase == "schedule"
    report["schedule"] = check_schedules(work, inv, all_streams, flipped)
    report["server_streams"] = len(all_streams)
    report["server_recording"] = sum(1 for v in all_streams.values() if v["recording"])

    api_state = load_json(os.path.join(work, "api_state.json"))
    rtsp_state = load_json(os.path.join(work, "rtsp_state.json"))
    onvif_state = load_json(os.path.join(work, "onvif_state.json"))
    report["streams"] = {
        "expected_total": expected_streams,
        "expected_reachable": reachable,
        "hook_known": api_state.get("streams_known", 0),
        "hook_live": api_state.get("streams_live", 0),
        "rtsp_sessions": rtsp_state.get("sessions", 0),
        "rtsp_playing": rtsp_state.get("playing", 0),
        "rtsp_teardowns": rtsp_state.get("teardowns", 0),
    }
    report["onvif"] = {
        "devices": onvif_state.get("devices", 0),
        "calls": onvif_state.get("calls", 0),
        "faults": onvif_state.get("faults", 0),
        "top_ops": dict(sorted(onvif_state.get("by_op", {}).items(),
                               key=lambda kv: -kv[1])[:8]),
    }

    load_state = load_json(os.path.join(work, "load_%s.json" % phase)) if phase else {}
    if load_state:
        report["load"] = load_state
        probe = load_state.get("probe")
        if probe:
            if probe["p95_ms"] > THRESHOLDS["probe_p95_ms"]:
                failures.append("healthcheck p95=%.0fms > %.0fms — nghi poller bị chiếm"
                                % (probe["p95_ms"], THRESHOLDS["probe_p95_ms"]))
            if probe["max_ms"] > THRESHOLDS["probe_max_ms"]:
                failures.append("healthcheck max=%.0fms > %.0fms — nghi poller bị chiếm"
                                % (probe["max_ms"], THRESHOLDS["probe_max_ms"]))
        ptz = load_state.get("ptz")
        if ptz:
            if ptz["p95_ms"] > THRESHOLDS["ptz_p95_ms"]:
                failures.append("PTZ p95=%.0fms > %.0fms — nghi pool device() bị lấp"
                                % (ptz["p95_ms"], THRESHOLDS["ptz_p95_ms"]))
            # timeout phía client không vào được thống kê độ trễ, phải đếm riêng
            errors = load_state.get("transport_errors") or {}
            timeouts = sum(v for k, v in errors.items() if k.startswith("ptz:"))
            total = ptz["count"] + timeouts
            if total and timeouts / float(total) > THRESHOLDS["ptz_timeout_ratio"]:
                failures.append("PTZ timeout %d/%d (%.0f%%) — điều khiển camera coi như chết"
                                % (timeouts, total, timeouts * 100.0 / total))

    if len(node_names) > 1:
        report["sync"] = compare_sync(work, node_names)
        if final and not report["sync"].get("converged", True):
            failures.append("DB ESC chưa hội tụ giữa các node")

    report["resources"] = resource_summary(work)

    if final:
        live = report["streams"]["rtsp_playing"]
        ratio = live / float(reachable)
        report["streams"]["live_ratio"] = round(ratio, 3)
        if ratio < THRESHOLDS["stream_live_ratio"]:
            failures.append("chỉ %d/%d luồng còn phát (%.0f%% < %.0f%%)"
                            % (live, reachable, ratio * 100,
                               THRESHOLDS["stream_live_ratio"] * 100))

    report["failures"] = failures
    return report, failures


def render(report):
    print("---- %s (%s)" % (report["phase"], report["at"]))
    for node, data in sorted(report["nodes"].items()):
        logs = data["logs"]
        print("  %-8s record=%d file/%.1fMB cam=%d tf=%d | locked=%d failed_apply=%d "
              "relay_recv=%d onvif_fail=%d exc=%d%s"
              % (node, data["record_files"], data["record_mb"], data["cameras_with_record"],
                 data["timefiles"], logs.get("db_locked", 0), logs.get("failed_apply", 0),
                 logs.get("relay_recv", 0), logs.get("onvif_fail", 0),
                 logs.get("exception", 0),
                 "  *** CRASH ***" if logs.get("crash") else ""))
    s = report["streams"]
    print("  streams  mong đợi=%d (khả dụng %d) server=%d đang_ghi=%d rtsp_playing=%d teardown=%d"
          % (s["expected_total"], s["expected_reachable"], report.get("server_streams", 0),
             report.get("server_recording", 0), s["rtsp_playing"], s["rtsp_teardowns"]))
    sc = report.get("schedule")
    if sc:
        print("  lịch ghi khớp=%d lệch=%d bỏ qua=%d (thứ=%d giờ=%d)"
              % (sc["match"], sc["mismatch"], sc["skipped"], sc["weekday"], sc["hour"]))
        for ex in sc["examples"][:4]:
            print("      lệch: %s" % ex)
    o = report["onvif"]
    print("  onvif    devices=%d calls=%d faults=%d top=%s"
          % (o["devices"], o["calls"], o["faults"],
             ", ".join("%s:%d" % kv for kv in list(o["top_ops"].items())[:5])))
    if "load" in report:
        for group in ("probe", "ptz", "storage"):
            g = report["load"].get(group)
            if g:
                codes = ", ".join("%s:%d" % kv for kv in sorted(g["codes"].items())[:5])
                print("  %-8s n=%-6d p50=%-7.0f p95=%-7.0f max=%-8.0f codes[%s]"
                      % (group, g["count"], g["p50_ms"], g["p95_ms"], g["max_ms"], codes))
        errs = report["load"].get("transport_errors")
        if errs:
            print("  lỗi kết nối: %s" % errs)
    if "sync" in report and "tables" in report["sync"]:
        parts = ["%s=%s" % (t, "SAME" if v["same"] else "DIFF")
                 for t, v in sorted(report["sync"]["tables"].items())]
        print("  sync     %s -> %s" % (", ".join(parts),
                                       "HỘI TỤ" if report["sync"]["converged"] else "LỆCH"))
    for node, r in sorted(report.get("resources", {}).items()):
        print("  %-8s rss %.0f->%.0fMB (đỉnh %.0f) threads=%d fds=%d cpu~%.0f%%"
              % (node, r["rss_start_mb"], r["rss_end_mb"], r["rss_peak_mb"],
                 r["threads_peak"], r["fds_peak"], r.get("cpu_pct_avg", 0)))
    for f in report["failures"]:
        print("  FAIL: %s" % f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--phase", default="")
    ap.add_argument("--final", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    report, failures = evaluate(args.work, args.phase or "final", args.final)
    if args.json:
        print(json.dumps(report, indent=1, ensure_ascii=False))
    else:
        render(report)

    out = os.path.join(args.work, "report_%s.json" % (args.phase or "final"))
    with open(out, "w") as fh:
        json.dump(report, fh, indent=1, ensure_ascii=False)

    if args.final:
        print("")
        print("KẾT QUẢ: %s (%d bất biến bị vi phạm) — chi tiết: %s"
              % ("ĐẠT" if not failures else "KHÔNG ĐẠT", len(failures), out))
        sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
