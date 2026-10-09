#!/usr/bin/env python3
"""Build the performance report (CSV files + PDF) for one test run.

Reads the PERF lines emitted by the execution manager(s):

    YYYY-MM-DD HH:MM:SS,mmm [PERF] component: iteration=<n> task_id=<id>
        start_req_ns=.. end_req_ns=.. start_res_ns=.. end_res_ns=..
        q_em_tw_ms=.. t_in_tw_ms=.. q_tw_em_ms=.. total_ms=.. core_id=<n>

and writes into --out:
    perf.csv     every PERF record
    stats.csv    per task / metric statistics
    report.pdf   cover, statistics tables, one Gantt page per iteration
"""
import argparse
import csv
import datetime
import re
import sys
import textwrap
from collections import defaultdict
from pathlib import Path

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Patch, Rectangle

matplotlib.rcParams["pdf.fonttype"] = 42

INT_FIELDS = ["iteration", "task_id", "start_req_ns", "end_req_ns",
              "start_res_ns", "end_res_ns", "core_id"]
MS_FIELDS = ["q_em_tw_ms", "t_in_tw_ms", "q_tw_em_ms", "total_ms"]
FIELDS = INT_FIELDS[:2] + INT_FIELDS[2:6] + MS_FIELDS + ["core_id"]

PERF_RE = re.compile(r"\[PERF\]\s+([^\s:]+):\s+(.*)$")
KV_RE = re.compile(r"(\w+)=(-?\d+(?:\.\d+)?)")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
ACTIVATE_RE = re.compile(r"Activate Task (\d+) \(([^)]+)\)")
START_MS_RE = re.compile(r"(\d+)\s*ms\]?:")

# Categorical slots 2, 1, 3 of the reference palette (blue = the task itself,
# the two queue hops get the other two hues).
C_Q_EM_TW = "#eb6834"
C_T_IN_TW = "#2a78d6"
C_Q_TW_EM = "#1baf7a"
INK = "#1a1a19"
INK_2 = "#52514e"
INK_MUTED = "#8a8985"
GRID = "#e4e3de"
BAND = "#ecebe6"

PAGE = (11.69, 8.27)  # A4 landscape, inches


def parse_logs(paths):
    records, malformed = [], 0
    plan_from_log = {}
    for path in paths:
        with open(path, errors="replace") as fh:
            for raw in fh:
                line = ANSI_RE.sub("", raw.rstrip("\n"))
                m = PERF_RE.search(line)
                if m:
                    kv = dict(KV_RE.findall(m.group(2)))
                    if any(f not in kv for f in FIELDS):
                        malformed += 1
                        continue
                    rec = {f: int(kv[f]) for f in INT_FIELDS}
                    rec.update({f: float(kv[f]) for f in MS_FIELDS})
                    rec["source"] = Path(path).name
                    records.append(rec)
                    continue
                if "Activate Task" in line:
                    sm = START_MS_RE.search(line)
                    for tid, name in ACTIVATE_RE.findall(line):
                        entry = plan_from_log.setdefault(int(tid), {"image": name})
                        if sm:
                            entry["start"] = int(sm.group(1))
    return records, malformed, plan_from_log


def load_manifest(path):
    if not path:
        return {}, {}
    try:
        import yaml
        with open(path) as fh:
            data = yaml.safe_load(fh)
        sched = data["schedule"]
        plan = {}
        for t in sched.get("tasks", []):
            plan[int(t["id"])] = {
                "image": str(t["image"]),
                "start": t.get("start"),
                "deadline": t.get("deadline"),
                "cpu_affinity": t.get("cpu_affinity"),
            }
        meta = {"name": sched.get("name"), "iterations": sched.get("iterations")}
        return plan, meta
    except Exception as exc:  # report still works without the manifest
        print(f"[WARN] could not read manifest '{path}': {exc}", file=sys.stderr)
        return {}, {}


def build_plan(manifest_plan, log_plan, task_ids):
    plan = {}
    for tid in task_ids:
        entry = {"image": f"task_{tid}", "start": None, "deadline": None}
        entry.update({k: v for k, v in log_plan.get(tid, {}).items() if v is not None})
        entry.update({k: v for k, v in manifest_plan.get(tid, {}).items() if v is not None})
        plan[tid] = entry
    return plan


def stats_rows(records, plan):
    by_task = defaultdict(list)
    for r in records:
        by_task[r["task_id"]].append(r)
    rows = []
    for tid in sorted(by_task):
        for metric in MS_FIELDS:
            v = np.array([r[metric] for r in by_task[tid]])
            rows.append({
                "task_id": tid,
                "image": plan[tid]["image"],
                "metric": metric,
                "n": len(v),
                "mean": float(v.mean()),
                "std": float(v.std(ddof=1)) if len(v) > 1 else 0.0,
                "min": float(v.min()),
                "max": float(v.max()),
                "median": float(np.median(v)),
                "p99": float(np.percentile(v, 99)),
            })
    return rows


def write_csvs(out, records, rows, plan):
    with open(out / "perf.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["iteration", "task_id", "image"] + FIELDS[2:] + ["source"])
        for r in sorted(records, key=lambda r: (r["iteration"], r["start_req_ns"])):
            w.writerow([r["iteration"], r["task_id"], plan[r["task_id"]]["image"]]
                       + [r[f] for f in FIELDS[2:]] + [r["source"]])
    with open(out / "stats.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["task_id", "image", "metric", "n", "mean_ms", "std_ms",
                    "min_ms", "max_ms", "median_ms", "p99_ms"])
        for s in rows:
            w.writerow([s["task_id"], s["image"], s["metric"], s["n"]]
                       + [f"{s[k]:.3f}" for k in ("mean", "std", "min", "max", "median", "p99")])


def footer(fig, title, page_no, page_total):
    fig.text(0.04, 0.02, title, fontsize=8, color=INK_MUTED, ha="left")
    fig.text(0.96, 0.02, f"page {page_no}/{page_total}", fontsize=8,
             color=INK_MUTED, ha="right")


def cover_page(pdf, info):
    fig = plt.figure(figsize=PAGE)
    fig.text(0.08, 0.84, "Performance report", fontsize=28, color=INK, weight="bold")
    fig.text(0.08, 0.78, f"{info['project']}  |  {info['test_name']}", fontsize=15, color=INK_2)
    y = 0.66
    for label, value in info["lines"]:
        wrapped = textwrap.wrap(str(value), 85) or [""]
        fig.text(0.27, y, "\n".join(wrapped), fontsize=11, color=INK, va="top", linespacing=1.4)
        fig.text(0.08, y, label, fontsize=11, color=INK_MUTED, va="top")
        y -= 0.05 * len(wrapped)
    footer(fig, info["footer"], 1, info["pages"])
    pdf.savefig(fig)
    plt.close(fig)


def stats_pages(pdf, rows, info, first_page_no):
    header = ["Task ID", "Image name", "Metric", "Mean", "Std Dev",
              "Min", "Max", "Median", "p99"]
    tasks = sorted({s["task_id"] for s in rows})
    per_page = 6
    chunks = [tasks[i:i + per_page] for i in range(0, len(tasks), per_page)]
    ns = sorted({s["n"] for s in rows})
    n_txt = str(ns[0]) if len(ns) == 1 else f"{ns[0]}-{ns[-1]}"
    for ci, chunk in enumerate(chunks):
        fig = plt.figure(figsize=PAGE)
        fig.text(0.04, 0.94, "Statistics summary", fontsize=18, color=INK, weight="bold")
        fig.text(0.04, 0.905,
                 f"All values in milliseconds  |  samples per task: {n_txt} iteration(s)"
                 + (f"  |  tasks {chunk[0]}-{chunk[-1]}" if len(chunks) > 1 else ""),
                 fontsize=10, color=INK_2)
        body, groups = [], []
        for gi, tid in enumerate(chunk):
            for mi, s in enumerate(x for x in rows if x["task_id"] == tid):
                body.append([str(tid) if mi == 0 else "", s["image"] if mi == 0 else "",
                             s["metric"]] + [f"{s[k]:.3f}" for k in
                                             ("mean", "std", "min", "max", "median", "p99")])
                groups.append(gi)
        ax = fig.add_axes([0.04, 0.07, 0.92, 0.80])
        ax.axis("off")
        tbl = ax.table(cellText=body, colLabels=header, loc="upper center",
                       colWidths=[0.07, 0.17, 0.14] + [0.105] * 6, cellLoc="right")
        tbl.auto_set_font_size(False)
        tbl.set_fontsize(9.5)
        tbl.scale(1, 1.55)
        for (r, c), cell in tbl.get_celld().items():
            cell.set_edgecolor(GRID)
            cell.set_linewidth(0.6)
            if r == 0:
                cell.set_facecolor("#f2f1ec")
                cell.set_text_props(weight="bold", color=INK)
                cell.set_edgecolor(INK_MUTED)
            else:
                cell.set_facecolor("#ffffff" if groups[r - 1] % 2 == 0 else "#f8f8f5")
                cell.set_text_props(color=INK)
            cell.get_text().set_va("center")
            if c in (0, 1, 2):
                cell._loc = "left"
                cell.get_text().set_ha("left")
                cell.PAD = 0.04
        footer(fig, info["footer"], first_page_no + ci, info["pages"])
        pdf.savefig(fig)
        plt.close(fig)
    return len(chunks)


def iteration_t0(recs, plan):
    """Time zero of an iteration: the earliest start_req minus its planned start."""
    offs = [r["start_req_ns"] - plan[r["task_id"]]["start"] * 1e6
            for r in recs if plan[r["task_id"]].get("start") is not None]
    return min(offs) if offs else min(r["start_req_ns"] for r in recs)


GANTT_TOP, GANTT_BOTTOM, GANTT_GAP = 8.27 * 0.86, 8.27 * 0.07, 0.85


def gantt_chunks(recs):
    """One chart (one core) per page."""
    return [[c] for c in sorted({r["core_id"] for r in recs})]


def gantt_page(pdf, it, recs, plan, t0, xmax, info, page_no, cores, part=1, nparts=1):
    by_core = defaultdict(list)
    for r in recs:
        if r["core_id"] in cores:
            by_core[r["core_id"]].append(r)
    cores = sorted(by_core)

    fig = plt.figure(figsize=PAGE)
    suffix = f"  -  core {cores[0]}" if nparts > 1 else ""
    fig.text(0.04, 0.945, f"Execution plan - iteration {it}{suffix}", fontsize=18,
             color=INK, weight="bold")
    fig.text(0.04, 0.912, "One chart per core; each bar is one task call.",
             fontsize=10, color=INK_2)
    handles = [Patch(facecolor=C_Q_EM_TW, label="q_em_tw_ms  (execution manager -> task wrapper)"),
               Patch(facecolor=C_T_IN_TW, label="t_in_tw_ms  (inside task wrapper; number = task id)"),
               Patch(facecolor=C_Q_TW_EM, label="q_tw_em_ms  (task wrapper -> execution manager)")]
    if any(plan[r["task_id"]].get("deadline") is not None for r in recs):
        handles.append(Patch(facecolor=BAND, label="planned window (start -> deadline)"))
    fig.legend(handles=handles, loc="upper right", bbox_to_anchor=(0.97, 0.965),
               fontsize=8.5, frameon=False, labelcolor=INK_2)

    rows_per_core = {c: sorted({r["task_id"] for r in by_core[c]}) for c in cores}
    total_rows = sum(len(v) for v in rows_per_core.values())
    top, bottom, gap = GANTT_TOP, GANTT_BOTTOM, GANTT_GAP
    pitch = max(0.12, min(0.62, (top - bottom - gap * len(cores)) / total_rows))
    left, width = 0.17, 0.79
    span = xmax
    min_w = 0.005 * span

    y_cursor = top
    axes = []
    for c in cores:
        ids = rows_per_core[c]
        h = pitch * len(ids)
        y_cursor -= 0.35
        ax = fig.add_axes([left, (y_cursor - h) / 8.27, width, h / 8.27])
        axes.append((ax, c, ids))
        y_cursor -= h + gap - 0.35
    for ax, c, ids in axes:
        pos = {tid: i for i, tid in enumerate(ids)}
        ax.set_xlim(0, span)
        ax.set_ylim(len(ids) - 0.5, -0.5)
        ax.set_yticks(range(len(ids)))
        ax.set_yticklabels([plan[t]["image"] for t in ids], fontsize=9, color=INK)
        ax.tick_params(axis="x", labelsize=8.5, colors=INK_2, length=0)
        ax.tick_params(axis="y", length=0)
        ax.grid(axis="x", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)
        for s in ax.spines.values():
            s.set_visible(False)
        ax.set_title(f"Core {c}", loc="left", fontsize=11, color=INK, weight="bold", pad=6)
        ax.set_xlabel("Time since iteration start (ms)", fontsize=8.5, color=INK_2)
        fig.canvas.draw()
        pts_per_ms = ax.get_window_extent().width * 72 / fig.dpi / span
        bar_h = min(0.5, 18.0 / (pitch * 72))
        for r in by_core[c]:
            y = pos[r["task_id"]]
            p = plan[r["task_id"]]
            if p.get("start") is not None and p.get("deadline") is not None:
                ax.add_patch(Rectangle((p["start"], y - 0.36), p["deadline"] - p["start"], 0.72,
                                       facecolor=BAND, edgecolor="none", zorder=1))
            x0 = (r["start_req_ns"] - t0) / 1e6
            x1 = (r["end_req_ns"] - t0) / 1e6
            x2 = (r["start_res_ns"] - t0) / 1e6
            x3 = (r["end_res_ns"] - t0) / 1e6
            w_in = max(x2 - x1, min_w)
            w_q1 = max(x1 - x0, min_w)
            w_q2 = max(x3 - x2, min_w)
            # queue hops are ~0.1 ms: give them a minimum visible width, growing away from
            # the task bar (leftwards for the request hop, rightwards for the result hop)
            segs = [(x1 - w_q1, w_q1, C_Q_EM_TW), (x1, w_in, C_T_IN_TW), (x1 + w_in, w_q2, C_Q_TW_EM)]
            for xs, w, col in segs:
                ax.add_patch(Rectangle((xs, y - bar_h / 2), w, bar_h, facecolor=col,
                                       edgecolor="#ffffff", linewidth=0.4, zorder=3))
            label = str(r["task_id"])
            if (x2 - x1) * pts_per_ms >= len(label) * 7 + 8:
                ax.text((x1 + x2) / 2, y, label, ha="center", va="center", fontsize=8.5,
                        color="#ffffff", weight="bold", zorder=4)
            else:
                ax.text(x1 + w_in + w_q2 + span * 0.006, y, label, ha="left", va="center",
                        fontsize=8.5, color=INK, weight="bold", zorder=4)
    fig.text(0.04, 0.045,
             "Segments narrower than a few pixels are drawn at a minimum visible width; "
             "exact values are in the statistics table and perf.csv.",
             fontsize=7.5, color=INK_MUTED)
    footer(fig, info["footer"], page_no, info["pages"])
    pdf.savefig(fig)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--logs", nargs="+", required=True, help="log files with PERF lines")
    ap.add_argument("--out", required=True, help="results directory")
    ap.add_argument("--manifest", help="manifest.yaml describing the execution plan")
    ap.add_argument("--project", default="project")
    ap.add_argument("--test-name", default="test")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    records, malformed, log_plan = parse_logs(args.logs)
    if not records:
        print("[ERROR] no PERF records found in the logs: nothing to report.", file=sys.stderr)
        return 2

    manifest_plan, meta = load_manifest(args.manifest)
    plan = build_plan(manifest_plan, log_plan, {r["task_id"] for r in records})
    rows = stats_rows(records, plan)
    write_csvs(out, records, rows, plan)

    by_it = defaultdict(list)
    for r in records:
        by_it[r["iteration"]].append(r)
    iterations = sorted(by_it)
    t0s = {it: iteration_t0(by_it[it], plan) for it in iterations}
    xmax = max((r["end_res_ns"] - t0s[it]) / 1e6 for it in iterations for r in by_it[it])
    xmax = max([xmax] + [p["deadline"] for p in plan.values() if p.get("deadline") is not None])
    xmax *= 1.03

    cores = sorted({r["core_id"] for r in records})
    n_stat_pages = -(-len({r["task_id"] for r in records}) // 6)
    gantt_parts = {it: gantt_chunks(by_it[it]) for it in iterations}
    pages = 1 + n_stat_pages + sum(len(v) for v in gantt_parts.values())
    info = {
        "project": args.project,
        "test_name": args.test_name,
        "pages": pages,
        "footer": f"{args.project} / {args.test_name}",
        "lines": [
            ("Generated", datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")),
            ("Schedule", meta.get("name") or "n/a"),
            ("Iterations in logs", f"{len(iterations)}  (#{iterations[0]} - #{iterations[-1]})"
             + (f", manifest declares {meta['iterations']}" if meta.get("iterations") else "")),
            ("Tasks", ", ".join(f"{t}={plan[t]['image']}" for t in sorted(plan))),
            ("Cores used", ", ".join(str(c) for c in cores)),
            ("PERF records", f"{len(records)}" + (f"  ({malformed} malformed skipped)" if malformed else "")),
            ("Log sources", ", ".join(sorted({r['source'] for r in records}))),
            ("Manifest", args.manifest or "none (task names read from the logs)"),
        ],
    }

    with PdfPages(out / "report.pdf") as pdf:
        cover_page(pdf, info)
        used = stats_pages(pdf, rows, info, 2)
        page_no = 2 + used
        for it in iterations:
            parts = gantt_parts[it]
            for pi, chunk in enumerate(parts, 1):
                gantt_page(pdf, it, by_it[it], plan, t0s[it], xmax, info, page_no,
                           chunk, pi, len(parts))
                page_no += 1

    print(f"[INFO] report: {out / 'report.pdf'}  ({pages} pages, {len(records)} PERF records)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
