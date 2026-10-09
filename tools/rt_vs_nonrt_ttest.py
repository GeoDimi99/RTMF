#!/usr/bin/env python3
"""
rt_vs_nonrt_ttest.py

Compares, inside one implementation (e.g. choreography-mqueue), the experiment
with real-time task threads (SCHED_FIFO/RR) against one or more experiments with
non real-time task threads (SCHED_OTHER, nice 0 or mapped nice) using two
t-tests on the raw samples of perf.csv:

  - Student t-test (pooled variance, assumes equal variances)
  - Welch t-test   (unequal variances, Welch-Satterthwaite degrees of freedom)

One test per (non-RT experiment, task, metric). Both tests are two-sided and
unpaired (the iterations of two experiments are independent runs). The p-values
are corrected with Holm-Bonferroni inside each family (= one non-RT experiment).

Only the Python standard library and PyYAML are required: the Student t
distribution is computed through the regularized incomplete beta function.

Usage (--rt and --nonrt are directory names inside <implementation>/results/;
the implementation is the current directory unless --impl is given; the CSV is
written to <implementation>/results/ unless --out is given, together with a
human-readable PDF report with the same name, unless --no-pdf is given):
  cd choreography-mqueue
  python3 ../tools/rt_vs_nonrt_ttest.py --rt test_single_core_1 --nonrt test_single_core_nort_4

  python3 tools/rt_vs_nonrt_ttest.py --impl orchestration-grpc --rt test_single_core_1 --nonrt test_single_core_nort_2
  python3 tools/rt_vs_nonrt_ttest.py --impl choreography-zeromq --rt test_single_core_1 \
      --nonrt test_single_core_nort_1 test_single_core_nort_2
  python3 tools/rt_vs_nonrt_ttest.py --impl ... --rt ... --nonrt ... --skip-iterations 1 --alpha 0.05 --out ttest.csv
"""

import argparse
import csv
import math
import statistics
import sys
import textwrap
from collections import defaultdict
from pathlib import Path

import yaml

IMPLEMENTATIONS_ROOT = Path(__file__).resolve().parent.parent  # jeff/, containing the implementations
DEFAULT_METRICS = ["total_ms", "t_in_tw_ms", "q_em_tw_ms", "q_tw_em_ms"]
RT_POLICIES = {"fifo", "rr"}
NON_RT_POLICIES = {"normal", "other", "batch"}


# ----------------- Student t distribution -----------------

def _betacf(a, b, x):
    """Continued fraction of the incomplete beta function (modified Lentz)."""
    tiny = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, 1000):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return h


def _betai(a, b, x):
    """Regularized incomplete beta function I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    ln_front = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log1p(-x)
    if x < (a + 1.0) / (a + b + 2.0):
        return math.exp(ln_front) * _betacf(a, b, x) / a
    return 1.0 - math.exp(ln_front) * _betacf(b, a, 1.0 - x) / b


def t_two_sided_p(t, df):
    """Two-sided p-value of the Student t distribution with df degrees of freedom."""
    if math.isnan(t) or df <= 0:
        return float("nan")
    if math.isinf(t):
        return 0.0
    return _betai(df / 2.0, 0.5, df / (df + t * t))


def t_critical(alpha, df):
    """Two-sided critical value: P(|T| > t) = alpha (found by bisection)."""
    lo, hi = 0.0, 1.0
    while t_two_sided_p(hi, df) > alpha:
        hi *= 2.0
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if t_two_sided_p(mid, df) > alpha:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


# ----------------- Tests -----------------

def student_ttest(x, y):
    """Student t-test with pooled variance. Returns (t, df, p)."""
    n1, n2 = len(x), len(y)
    m1, m2 = statistics.fmean(x), statistics.fmean(y)
    v1, v2 = statistics.variance(x), statistics.variance(y)
    df = n1 + n2 - 2
    sp2 = ((n1 - 1) * v1 + (n2 - 1) * v2) / df
    se = math.sqrt(sp2 * (1.0 / n1 + 1.0 / n2))
    t = (m2 - m1) / se if se > 0 else (0.0 if m1 == m2 else math.copysign(math.inf, m2 - m1))
    return t, df, t_two_sided_p(t, df)


def welch_ttest(x, y):
    """Welch t-test (unequal variances). Returns (t, df, p, se)."""
    n1, n2 = len(x), len(y)
    m1, m2 = statistics.fmean(x), statistics.fmean(y)
    a, b = statistics.variance(x) / n1, statistics.variance(y) / n2
    se = math.sqrt(a + b)
    if se == 0:
        return (0.0 if m1 == m2 else math.copysign(math.inf, m2 - m1)), float(n1 + n2 - 2), (1.0 if m1 == m2 else 0.0), 0.0
    t = (m2 - m1) / se
    df = (a + b) ** 2 / (a * a / (n1 - 1) + b * b / (n2 - 1))
    return t, df, t_two_sided_p(t, df), se


def hedges_g(x, y):
    """Standardized mean difference (y - x) with small-sample correction."""
    n1, n2 = len(x), len(y)
    sp = math.sqrt(((n1 - 1) * statistics.variance(x) + (n2 - 1) * statistics.variance(y)) / (n1 + n2 - 2))
    if sp == 0:
        return float("nan")
    d = (statistics.fmean(y) - statistics.fmean(x)) / sp
    return d * (1.0 - 3.0 / (4.0 * (n1 + n2) - 9.0))


def holm(pvalues):
    """Holm-Bonferroni adjusted p-values (same order as the input)."""
    m = len(pvalues)
    order = sorted(range(m), key=lambda i: pvalues[i])
    adjusted = [0.0] * m
    running = 0.0
    for rank, i in enumerate(order):
        running = max(running, min(1.0, (m - rank) * pvalues[i]))
        adjusted[i] = running
    return adjusted


# ----------------- Data loading -----------------

def load_samples(perf_csv, metrics, skip_iterations):
    """Returns {(task_id, image): {metric: [values]}} reading perf.csv."""
    samples = defaultdict(lambda: defaultdict(list))
    with open(perf_csv, newline="") as f:
        for row in csv.DictReader(f):
            if int(row["iteration"]) < skip_iterations:
                continue
            key = (int(row["task_id"]), row["image"])
            for metric in metrics:
                samples[key][metric].append(float(row[metric]))
    return samples


def read_sched(manifest_path):
    """Returns (set of policies, list of priorities) of the tasks in the manifest."""
    with open(manifest_path) as f:
        tasks = yaml.safe_load(f)["schedule"]["tasks"]
    return {str(t["policy"]).lower() for t in tasks}, [int(t["priority"]) for t in tasks]


def describe(policies, priorities):
    if policies & RT_POLICIES:
        return "/".join(sorted(policies)) + " prio " + ",".join(str(p) for p in sorted(set(priorities)))
    if all(p == 0 for p in priorities):
        return "/".join(sorted(policies)) + " nice 0"
    return "/".join(sorted(policies)) + " nice " + ",".join(str(p) for p in sorted(set(priorities)))


# ----------------- PDF report -----------------

METRIC_INFO = {
    "total_ms":   ("End-to-end response time", "request sent by the execution manager -> result received"),
    "t_in_tw_ms": ("Execution time", "task thread start -> task thread end"),
    "q_em_tw_ms": ("Dispatch latency", "request sent -> task thread start"),
    "q_tw_em_ms": ("Result latency", "task thread end -> result received"),
}

# Reference palette (dataviz skill): light surface, ink, categorical slots 1-2
SURFACE, INK, INK_2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e4e3df"
C_RT, C_NONRT = "#2a78d6", "#eb6834"
PAGE = (11.69, 8.27)  # A4 landscape, inches


def fmt_p(p):
    if math.isnan(p):
        return "n/a"
    return "< 0.001" if p < 0.001 else f"{p:.3f}"


def effect_label(g):
    a = abs(g)
    if math.isnan(a):
        return "n/a"
    return "negligible" if a < 0.2 else "small" if a < 0.5 else "medium" if a < 0.8 else "large"


def verdict(r):
    """One short human sentence for a result row (based on the Welch test)."""
    if not r["welch_significant"]:
        return "no evidence of a difference"
    word = "slower" if r["diff_nonrt_minus_rt"] > 0 else "faster"
    return f"non-RT {abs(r['diff_nonrt_minus_rt']):.2f} ms {word} ({effect_label(r['hedges_g'])} effect)"


def write_pdf(path, rows, rt_samples, nrt_samples_by_name, args, project):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch

    plt.rcParams.update({
        "font.size": 9, "text.color": INK, "axes.labelcolor": INK_2, "axes.edgecolor": MUTED,
        "xtick.color": INK_2, "ytick.color": INK_2, "axes.facecolor": SURFACE,
        "figure.facecolor": "white", "axes.spines.top": False, "axes.spines.right": False,
    })

    families = defaultdict(list)
    for r in rows:
        families[r["nonrt_experiment"]].append(r)
    first = rows[0]
    page = [0]

    def new_page(title, subtitle=None):
        fig = plt.figure(figsize=PAGE)
        fig.text(0.04, 0.95, title, fontsize=15, weight="bold", va="top")
        if subtitle:
            fig.text(0.04, 0.905, subtitle, fontsize=9.5, color=INK_2, va="top")
        return fig

    def close_page(pdf, fig):
        page[0] += 1
        fig.text(0.04, 0.02, f"{project} - RT vs non-RT t-tests", fontsize=7.5, color=MUTED)
        fig.text(0.96, 0.02, f"page {page[0]}", fontsize=7.5, color=MUTED, ha="right")
        pdf.savefig(fig)
        plt.close(fig)

    with PdfPages(path) as pdf:
        # ---- Page 1: summary in plain language ----
        fig = new_page(f"RT vs non-RT task threads - {project}",
                       "Student and Welch t-tests on the per-iteration timings of perf.csv")
        y = 0.84
        def line(text, size=9.5, color=INK, weight="normal", indent=0.0, gap=0.03):
            nonlocal y
            fig.text(0.04 + indent, y, text, fontsize=size, color=color, weight=weight, va="top")
            y -= gap

        def ensure_space(needed):
            nonlocal fig, y
            if y - needed < 0.075:
                close_page(pdf, fig)
                fig = new_page("Key results (continued)")
                y = 0.86

        line("Experiments compared", 11, weight="bold", gap=0.036)
        line(f"RT        {first['rt_experiment']}   ({first['rt_config']})", indent=0.01)
        for name, fam in families.items():
            line(f"non-RT  {name}   ({fam[0]['nonrt_config']})", indent=0.01)
        y -= 0.01
        line("Method", 11, weight="bold", gap=0.036)
        line(f"{first['n_rt']} RT and {first['n_nonrt']} non-RT iterations per task (first {args.skip_iterations} "
             f"iteration(s) dropped as warm-up); one test per task and metric.", indent=0.01)
        line(f"Two-sided, unpaired tests; p-values Holm-corrected within each comparison; significance level "
             f"alpha = {args.alpha}.", indent=0.01)
        line("Welch's test is the reference (it does not assume equal variances); Student's is reported "
             "for comparison.", indent=0.01)
        y -= 0.01

        for name, fam in families.items():
            tot = [r for r in fam if r["metric"] == "total_ms"] or fam
            ensure_space(0.036 + 0.028 * (len(tot) + 1))
            line(f"Key results: end-to-end response time, {first['rt_experiment']} vs {name}", 11,
                 weight="bold", gap=0.036)
            for r in tot:
                mark = "-" if not r["welch_significant"] else ("+" if r["diff_nonrt_minus_rt"] > 0 else "-")
                line(f"Task {r['task_id']}:  RT {r['mean_rt']:.1f} ms  vs  non-RT {r['mean_nonrt']:.1f} ms  ->  "
                     f"{verdict(r)},  Welch p {fmt_p(r['welch_p_holm'])}", indent=0.01, gap=0.028)
            n_sig = sum(r["welch_significant"] for r in fam)
            n_disagree = sum(r["welch_significant"] != r["student_significant"] for r in fam)
            line(f"Overall: {n_sig} of {len(fam)} task/metric tests significant (Welch); Student and Welch "
                 f"disagree on {n_disagree}.", indent=0.01, color=INK_2)
            y -= 0.012

        fig.text(0.04, 0.055, "Note: with ~100 samples per side, even tiny differences are significant. Judge "
                 "importance by the difference in ms and the effect size, not by the p-value.",
                 fontsize=8.5, color=INK_2)
        close_page(pdf, fig)

        for name, fam in families.items():
            cfg = f"{first['rt_experiment']} ({first['rt_config']})  vs  {name} ({fam[0]['nonrt_config']})"
            metrics = [m for m in args.metrics if any(r["metric"] == m for r in fam)]

            # ---- Mean difference with 95% CI, one panel per metric ----
            fig = new_page("Difference in mean time: non-RT minus RT",
                           f"{cfg}.  Right of 0 = non-RT slower.  Bars: Welch 95% confidence interval.")
            ncols = 2
            nrows = math.ceil(len(metrics) / ncols)
            for i, metric in enumerate(metrics):
                ax = fig.add_axes([0.07 + (i % ncols) * 0.47, 0.1 + (nrows - 1 - i // ncols) * (0.74 / nrows),
                                   0.38, 0.74 / nrows - 0.13])
                mr = sorted((r for r in fam if r["metric"] == metric), key=lambda r: r["task_id"])
                ys = list(range(len(mr)))[::-1]
                for yy, r in zip(ys, mr):
                    lo, hi, d = r["welch_ci_low"], r["welch_ci_high"], r["diff_nonrt_minus_rt"]
                    ax.plot([lo, hi], [yy, yy], color=C_RT, lw=2, solid_capstyle="round", zorder=2)
                    ax.plot(d, yy, "o", ms=7, color=C_RT, mfc=C_RT if r["welch_significant"] else SURFACE,
                            mew=2, zorder=3)
                    ax.annotate(f"{d:+.2f}", (max(hi, d), yy), xytext=(6, 0), textcoords="offset points",
                                va="center", fontsize=8, color=INK_2)
                ax.axvline(0, color=MUTED, lw=1, zorder=1)
                ax.set_yticks(ys, [f"Task {r['task_id']}" for r in mr])
                ax.grid(axis="x", color=GRID, lw=0.8)
                ax.set_axisbelow(True)
                xmin = min(min(r["welch_ci_low"] for r in mr), 0)
                xmax = max(max(r["welch_ci_high"] for r in mr), 0)
                pad = (xmax - xmin) * 0.18 or 1.0
                ax.set_xlim(xmin - pad * 0.3, xmax + pad)
                ax.set_ylim(-0.6, len(mr) - 0.4)
                title, desc = METRIC_INFO.get(metric, (metric, ""))
                ax.set_title(f"{title} ({metric})", loc="left", fontsize=10, weight="bold", pad=14)
                ax.text(0, 1.01, desc, transform=ax.transAxes, fontsize=7.5, color=MUTED)
                ax.set_xlabel("difference in mean [ms]")
            fig.legend(handles=[
                Line2D([], [], marker="o", ls="", ms=7, color=C_RT, mew=2, label="significant (Welch, Holm)"),
                Line2D([], [], marker="o", ls="", ms=7, color=C_RT, mfc=SURFACE, mew=2, label="not significant"),
            ], loc="upper right", bbox_to_anchor=(0.96, 0.97), frameon=False, ncols=2, fontsize=8.5)
            close_page(pdf, fig)

            # ---- Distributions of the main metric, one panel per task ----
            metric = "total_ms" if "total_ms" in metrics else metrics[0]
            nrt_samples = nrt_samples_by_name[name]
            keys = sorted(k for k in rt_samples if k in nrt_samples)
            title, _ = METRIC_INFO.get(metric, (metric, ""))
            fig = new_page(f"Distribution of {title.lower()} per task",
                           f"{cfg}.\nBox: 25th-75th percentile, line: median, whiskers: 1.5 IQR, dots: outliers. "
                           "Each panel has its own scale.")
            ncols = min(3, len(keys))
            nrows = math.ceil(len(keys) / ncols)
            for i, key in enumerate(keys):
                ax = fig.add_axes([0.07 + (i % ncols) * (0.9 / ncols),
                                   0.1 + (nrows - 1 - i // ncols) * (0.74 / nrows),
                                   0.9 / ncols - 0.07, 0.74 / nrows - 0.08])
                data = [rt_samples[key][metric], nrt_samples[key][metric]]
                bp = ax.boxplot(data, widths=0.5, patch_artist=True, showfliers=True,
                                medianprops=dict(color=INK, lw=1.5),
                                whiskerprops=dict(color=MUTED), capprops=dict(color=MUTED),
                                flierprops=dict(marker="o", ms=3, mec=MUTED, mfc="none"))
                for box, c in zip(bp["boxes"], (C_RT, C_NONRT)):
                    box.set(facecolor=c, edgecolor=c, alpha=0.85)
                ax.set_xticks([1, 2], ["RT", "non-RT"])
                ax.set_title(f"Task {key[0]} ({key[1]})", loc="left", fontsize=9.5, weight="bold")
                ax.set_ylabel(f"{metric} [ms]")
                ax.grid(axis="y", color=GRID, lw=0.8)
                ax.set_axisbelow(True)
            fig.legend(handles=[Patch(color=C_RT, label=f"RT ({first['rt_experiment']})"),
                                Patch(color=C_NONRT, label=f"non-RT ({name})")],
                       loc="upper right", bbox_to_anchor=(0.96, 0.97), frameon=False, ncols=2, fontsize=8.5)
            close_page(pdf, fig)

            # ---- Full results table ----
            fig = new_page("All results", f"{cfg}.  Means in ms (± standard deviation); p-values Holm-corrected.")
            header = ["Task", "Metric", "RT mean ± sd", "non-RT mean ± sd", "Difference [95% CI]",
                      "Student p", "Welch p", "Hedges' g", "Verdict (Welch)"]
            cells = []
            for r in fam:
                cells.append([
                    str(r["task_id"]), r["metric"],
                    f"{r['mean_rt']:.3f} ± {r['std_rt']:.3f}", f"{r['mean_nonrt']:.3f} ± {r['std_nonrt']:.3f}",
                    f"{r['diff_nonrt_minus_rt']:+.3f} [{r['welch_ci_low']:+.3f}, {r['welch_ci_high']:+.3f}]",
                    fmt_p(r["student_p_holm"]), fmt_p(r["welch_p_holm"]),
                    f"{r['hedges_g']:.2f} ({effect_label(r['hedges_g'])})", verdict(r),
                ])
            ax = fig.add_axes([0.03, 0.06, 0.94, 0.8])
            ax.axis("off")
            table = ax.table(cellText=cells, colLabels=header, loc="upper center", cellLoc="left",
                             colWidths=[0.04, 0.08, 0.11, 0.12, 0.17, 0.065, 0.065, 0.1, 0.25])
            table.auto_set_font_size(False)
            table.set_fontsize(7.5)
            table.scale(1, 1.25)
            for (row, col), cell in table.get_celld().items():
                cell.set_edgecolor(GRID)
                if row == 0:
                    cell.set_text_props(weight="bold", color=INK)
                    cell.set_facecolor("#f0efec")
                elif fam[row - 1]["metric"] == "total_ms":
                    cell.set_facecolor("#f6f5f2")
            close_page(pdf, fig)

        # ---- Last page: how to read ----
        fig = new_page("How to read this report")
        y = 0.86
        items = [
            ("Metrics", ""),
            *[(f"  {m}", f"{t}: {d}") for m, (t, d) in METRIC_INFO.items() if m in args.metrics],
            ("  ", "total_ms = q_em_tw_ms + t_in_tw_ms + q_tw_em_ms. Use total_ms for the overall effect: waiting "
                   "can move between the parts."),
            ("Difference", "mean(non-RT) - mean(RT). Positive = non-RT slower, negative = non-RT faster."),
            ("95% CI", "range that contains the true difference with 95% confidence (Welch). If it does not cross "
                       "0, the difference is significant."),
            ("p-value", "probability of a difference at least this large if the true means were equal. "
                        f"< {args.alpha} = significant. '< 0.001' means practically zero."),
            ("Holm", "correction for running many tests at once, so that 'significant' results are not "
                     "found by chance."),
            ("Student vs Welch", "Student assumes equal variances, Welch does not (reference). A much smaller "
                                 "Student p signals that the variances differ."),
            ("Hedges' g", "difference in units of standard deviation: < 0.2 negligible, < 0.5 small, < 0.8 "
                          "medium, otherwise large."),
            ("Caveats", "a non-significant result is not proof of equality; the t-test compares means only - "
                        "check the distributions page for jitter and outliers."),
        ]
        for k, v in items:
            fig.text(0.05, y, k, fontsize=9.5, weight="bold", va="top")
            fig.text(0.2, y, textwrap.fill(v, 125), fontsize=9.5, color=INK_2, va="top")
            y -= 0.05 + 0.028 * (len(textwrap.wrap(v, 125)) - 1 if v else 0)
        close_page(pdf, fig)


# ----------------- Main -----------------

def main():
    ap = argparse.ArgumentParser(description="Student and Welch t-tests: RT vs non-RT experiments.")
    ap.add_argument("--impl", type=Path, default=Path.cwd(),
                    help="implementation directory containing results/: a path, or just its name "
                         "(e.g. orchestration-grpc) from any directory (default: current directory)")
    ap.add_argument("--rt", required=True, help="RT experiment directory inside results/ (e.g. test_single_core_1)")
    ap.add_argument("--nonrt", required=True, nargs="+",
                    help="non-RT experiment directory(ies) inside results/ (e.g. test_single_core_nort_2); globs allowed")
    ap.add_argument("--metrics", nargs="+", default=DEFAULT_METRICS, help="perf.csv columns to test")
    ap.add_argument("--skip-iterations", type=int, default=1, help="warm-up iterations to drop (default: %(default)s)")
    ap.add_argument("--alpha", type=float, default=0.05, help="significance level (default: %(default)s)")
    ap.add_argument("--out", type=Path,
                    help="output CSV (default: <impl>/results/ttest_<rt>_vs_<nonrt>.csv)")
    ap.add_argument("--pdf", type=Path, help="output PDF report (default: same name as the CSV, .pdf)")
    ap.add_argument("--no-pdf", action="store_true", help="do not generate the PDF report")
    args = ap.parse_args()

    impl = args.impl.resolve()
    if not (impl / "results").is_dir() and not args.impl.is_absolute():
        # Allow the bare implementation name (e.g. orchestration-grpc) from any directory
        impl = (IMPLEMENTATIONS_ROOT / args.impl).resolve()
    project = impl.name
    results = impl / "results"
    if not results.is_dir():
        ap.error(f"{results} not found: --impl must be an implementation directory (e.g. choreography-mqueue)")

    rt_dir = results / args.rt
    if not (rt_dir / "perf.csv").is_file():
        ap.error(f"{rt_dir}/perf.csv not found")
    rt_policies, rt_prios = read_sched(rt_dir / "manifest.yaml")
    if not rt_policies <= RT_POLICIES:
        ap.error(f"{args.rt} is not a RT experiment (policy {rt_policies})")
    rt_samples = load_samples(rt_dir / "perf.csv", args.metrics, args.skip_iterations)

    nrt_dirs = sorted({d for pattern in args.nonrt for d in results.glob(pattern)})
    if not nrt_dirs:
        ap.error(f"no non-RT experiment matching {args.nonrt} in {results}")

    rows = []
    nrt_samples_by_name = {}
    for nrt_dir in nrt_dirs:
        if not (nrt_dir / "perf.csv").is_file():
            print(f"[SKIP] {nrt_dir.name}: perf.csv not found", file=sys.stderr)
            continue
        nrt_policies, nrt_prios = read_sched(nrt_dir / "manifest.yaml")
        if not nrt_policies <= NON_RT_POLICIES:
            print(f"[SKIP] {nrt_dir.name}: not a non-RT experiment (policy {nrt_policies})", file=sys.stderr)
            continue
        nrt_samples = load_samples(nrt_dir / "perf.csv", args.metrics, args.skip_iterations)
        nrt_samples_by_name[nrt_dir.name] = nrt_samples

        family = []
        for key in sorted(set(rt_samples) & set(nrt_samples)):
            for metric in args.metrics:
                x, y = rt_samples[key][metric], nrt_samples[key][metric]
                if len(x) < 2 or len(y) < 2:
                    continue
                s_t, s_df, s_p = student_ttest(x, y)
                w_t, w_df, w_p, w_se = welch_ttest(x, y)
                diff = statistics.fmean(y) - statistics.fmean(x)
                half = t_critical(args.alpha, w_df) * w_se
                family.append({
                    "project": project,
                    "rt_experiment": args.rt,
                    "rt_config": describe(rt_policies, rt_prios),
                    "nonrt_experiment": nrt_dir.name,
                    "nonrt_config": describe(nrt_policies, nrt_prios),
                    "task_id": key[0],
                    "image": key[1],
                    "metric": metric,
                    "n_rt": len(x),
                    "mean_rt": statistics.fmean(x),
                    "std_rt": statistics.stdev(x),
                    "n_nonrt": len(y),
                    "mean_nonrt": statistics.fmean(y),
                    "std_nonrt": statistics.stdev(y),
                    "diff_nonrt_minus_rt": diff,
                    "welch_ci_low": diff - half,
                    "welch_ci_high": diff + half,
                    "hedges_g": hedges_g(x, y),
                    "student_t": s_t,
                    "student_df": s_df,
                    "student_p": s_p,
                    "welch_t": w_t,
                    "welch_df": w_df,
                    "welch_p": w_p,
                })

        for test in ("student", "welch"):
            adjusted = holm([r[f"{test}_p"] for r in family])
            for r, p_adj in zip(family, adjusted):
                r[f"{test}_p_holm"] = p_adj
                r[f"{test}_significant"] = p_adj < args.alpha
        rows.extend(family)

    if not rows:
        print("No comparison performed.", file=sys.stderr)
        return 1

    if args.out is None:
        nrt_names = sorted({r["nonrt_experiment"] for r in rows})
        args.out = results / f"ttest_{args.rt}_vs_{nrt_names[0] if len(nrt_names) == 1 else 'nonrt'}.csv"

    fields = list(rows[0].keys())
    with open(args.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for r in rows:
            writer.writerow({k: (f"{v:.6g}" if isinstance(v, float) else v) for k, v in r.items()})

    # Console summary
    print(f"alpha={args.alpha}, Holm-corrected, two-sided; diff = mean(non-RT) - mean(RT) [ms]; "
          f"first {args.skip_iterations} iteration(s) dropped\n")
    current = None
    for r in rows:
        if r["nonrt_experiment"] != current:
            current = r["nonrt_experiment"]
            print(f"=== {r['project']}: {r['rt_experiment']} ({r['rt_config']})  vs  "
                  f"{r['nonrt_experiment']} ({r['nonrt_config']})")
            print(f"  {'task':<4} {'metric':<11} {'mean RT':>9} {'mean nRT':>9} {'diff':>9} "
                  f"{'Student p':>10} {'Welch p':>10} {'g':>7}  sig(S/W)")
        sig = ("*" if r["student_significant"] else "-") + "/" + ("*" if r["welch_significant"] else "-")
        print(f"  {r['task_id']:<4} {r['metric']:<11} {r['mean_rt']:9.3f} {r['mean_nonrt']:9.3f} "
              f"{r['diff_nonrt_minus_rt']:9.3f} {r['student_p_holm']:10.3g} {r['welch_p_holm']:10.3g} "
              f"{r['hedges_g']:7.2f}  {sig}")
    print(f"\nWritten {len(rows)} rows to {args.out}")

    if not args.no_pdf:
        pdf_path = args.pdf or args.out.with_suffix(".pdf")
        write_pdf(pdf_path, rows, rt_samples, nrt_samples_by_name, args, project)
        print(f"Written report to {pdf_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
