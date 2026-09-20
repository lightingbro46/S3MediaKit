#!/usr/bin/env python3
"""Dựng báo cáo Markdown từ các report_<phase>.json mà check_stress.py sinh ra.

Dùng:
    make_report.py <work_dir> [work_dir2 ...] --out report.md [--title "..."]

Mỗi work_dir là một lần chạy (ví dụ một thư mục cho bản tier-on, một cho tier-off).
"""
import argparse
import json
import os
import sys

PHASE_ORDER = ["ramp", "steady", "onvif", "flap", "schedule", "retention",
               "failover", "rebalance", "recover", "final"]


def load_reports(work):
    out = {}
    for name in os.listdir(work):
        if not (name.startswith("report_") and name.endswith(".json")):
            continue
        phase = name[len("report_"):-len(".json")]
        try:
            out[phase] = json.load(open(os.path.join(work, name)))
        except (OSError, ValueError):
            continue
    return out


def ordered(reports):
    known = [p for p in PHASE_ORDER if p in reports]
    rest = sorted(p for p in reports if p not in PHASE_ORDER)
    return known + rest


def fmt(value, digits=0):
    if value is None:
        return "—"
    if digits:
        return ("%%.%df" % digits) % value
    return "%d" % value


def latency_table(reports, phases):
    lines = ["| Phase | probe p50 | probe p95 | probe max | ptz p50 | ptz p95 | ptz max | storage p95 |",
             "|---|---|---|---|---|---|---|---|"]
    for p in phases:
        load = reports[p].get("load") or {}
        probe = load.get("probe") or {}
        ptz = load.get("ptz") or {}
        st = load.get("storage") or {}
        if not (probe or ptz or st):
            continue
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            p, fmt(probe.get("p50_ms")), fmt(probe.get("p95_ms")), fmt(probe.get("max_ms")),
            fmt(ptz.get("p50_ms")), fmt(ptz.get("p95_ms")), fmt(ptz.get("max_ms")),
            fmt(st.get("p95_ms"))))
    return lines if len(lines) > 2 else []


def stream_table(reports, phases):
    lines = ["| Phase | luồng ở server | đang ghi | RTSP đang phát | teardown | lịch khớp/lệch |",
             "|---|---|---|---|---|---|"]
    for p in phases:
        r = reports[p]
        s = r.get("streams", {})
        sc = r.get("schedule") or {}
        lines.append("| %s | %s | %s | %s | %s | %s/%s |" % (
            p, fmt(r.get("server_streams")), fmt(r.get("server_recording")),
            fmt(s.get("rtsp_playing")), fmt(s.get("rtsp_teardowns")),
            fmt(sc.get("match")), fmt(sc.get("mismatch"))))
    return lines


def resource_table(reports, phases):
    lines = ["| Phase | node | RSS cuối (MB) | RSS đỉnh (MB) | threads | fd | CPU tb (%) |",
             "|---|---|---|---|---|---|---|"]
    for p in phases:
        for node, r in sorted((reports[p].get("resources") or {}).items()):
            lines.append("| %s | %s | %s | %s | %s | %s | %s |" % (
                p, node, fmt(r.get("rss_end_mb"), 0), fmt(r.get("rss_peak_mb"), 0),
                fmt(r.get("threads_peak")), fmt(r.get("fds_peak")),
                fmt(r.get("cpu_pct_avg"), 0)))
    return lines


def record_table(reports, phases):
    lines = ["| Phase | node | file mp4 | dung lượng (MB) | camera có bản ghi | timefile |",
             "|---|---|---|---|---|---|"]
    for p in phases:
        for node, d in sorted((reports[p].get("nodes") or {}).items()):
            lines.append("| %s | %s | %s | %s | %s | %s |" % (
                p, node, fmt(d.get("record_files")), fmt(d.get("record_mb"), 1),
                fmt(d.get("cameras_with_record")), fmt(d.get("timefiles"))))
    return lines


def render_run(work, title):
    reports = load_reports(work)
    if not reports:
        return ["", "## %s" % title, "", "_Không có dữ liệu trong `%s`._" % work]
    phases = ordered(reports)
    inv_path = os.path.join(work, "inventory.json")
    inv = json.load(open(inv_path)) if os.path.exists(inv_path) else {}

    out = ["", "## %s" % title, "",
           "- Thư mục: `%s`" % work,
           "- Quy mô: %s camera / %s luồng trên %s node"
           % (inv.get("cameras", "?"), inv.get("streams", "?"),
              len(inv.get("nodes", [])) or "?"),
           "- Phase đã chạy: %s" % ", ".join(phases), ""]

    out += ["### Độ trễ API", ""] + (latency_table(reports, phases) or ["_không có tải REST._"]) + [""]
    out += ["### Luồng và lịch ghi", ""] + stream_table(reports, phases) + [""]
    out += ["### Ghi hình", ""] + record_table(reports, phases) + [""]
    out += ["### Tài nguyên", ""] + resource_table(reports, phases) + [""]

    final = reports.get("final") or reports.get(phases[-1])
    fails = final.get("failures") or []
    out += ["### Bất biến", ""]
    if fails:
        out += ["Vi phạm:", ""] + ["- %s" % f for f in fails] + [""]
    else:
        out += ["Không có bất biến nào bị vi phạm.", ""]

    sync = final.get("sync")
    if sync and "tables" in sync:
        out += ["Đồng bộ DB giữa các node: **%s**"
                % ("hội tụ" if sync.get("converged") else "LỆCH"), ""]
        out += ["| Bảng | Giống nhau | Số dòng |", "|---|---|---|"]
        for table, v in sorted(sync["tables"].items()):
            out.append("| `%s` | %s | %s |" % (
                table, "có" if v["same"] else "**không**",
                ", ".join("%s=%s" % kv for kv in sorted(v["rows"].items()))))
        out.append("")

    onvif = final.get("onvif") or {}
    if onvif.get("calls"):
        out += ["### ONVIF", "",
                "- Thiết bị đã gọi: %s" % onvif.get("devices"),
                "- Tổng lệnh: %s (fault %s)" % (onvif.get("calls"), onvif.get("faults")),
                "- Theo operation: %s" % ", ".join(
                    "`%s`=%d" % kv for kv in (onvif.get("top_ops") or {}).items()), ""]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("works", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default="Báo cáo stress test VMS")
    ap.add_argument("--names", default="", help="tên từng run, ngăn bởi dấu phẩy")
    args = ap.parse_args()

    names = [n.strip() for n in args.names.split(",")] if args.names else []
    lines = ["# %s" % args.title, ""]
    for i, work in enumerate(args.works):
        title = names[i] if i < len(names) and names[i] else os.path.basename(work.rstrip("/"))
        lines += render_run(work, title)

    with open(args.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("đã ghi %s" % args.out)


if __name__ == "__main__":
    main()
