#!/usr/bin/env python3
# @jarvis-script
# @short: Deterministically compute portfolio dividend totals and prepend them to the AI summary
# @params: context (reads _workflow_base_directory, _task_working_directory, _file_input_0);
#   peritemDir (relative dir holding the per-item PROB_*.output.json, vs the task working dir);
#   reportPath (optional explicit output file)
# @description: Stage 3 of portfolioDividendAnalysis — the deterministic aggregation stage. The
#   per-item ai_call stage looks up each position's dividend yield (reasoning j9t leaves to the
#   model); this stage does the exact arithmetic over those lookups. It reads each position's
#   structured reply (PROB_<SYM>_<NN>.output.json, the symbol from the templated filename, the yield
#   from the JSON), joins it against the allocations in port62pos.csv, and computes the
#   allocation-weighted portfolio yield as an exact sum — allocation x yield / 100 — rather than
#   leaving the grand total to the model's own summation (LLMs drift when adding dozens of figures).
#   IMPORTANT: this removes *summation* drift, not *input* error — the total is only as accurate as
#   the AI's per-position yields. For an authoritative figure, feed authoritative dividend data (a
#   broker or database export) in place of the AI lookups. It prepends a portfolio-totals block to
#   the AI executive summary and writes the combined report.
# @outputs: portfolio_report.md — verified totals header + the AI executive summary

import csv
import glob
import json
import os
import re


def _num(s):
    """Best-effort parse of a percentage/number that may carry a trailing % or $."""
    if isinstance(s, (int, float)):
        return float(s)
    s = (s or "").strip().lstrip("$").rstrip("%").strip()
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


def _symbol_from_filename(path):
    """PROB_<SYM>_<NN>.output.json -> <SYM> (the per-row {{pos.Symbol}} the fan-out templated in)."""
    name = os.path.basename(path)
    m = re.match(r"PROB_(.+)_\d+\.output\.json$", name)
    return m.group(1).strip() if m else ""


def _load_peritem_yields(peritem_dir):
    """Map Symbol -> dividend yield % from every structured per-item reply in peritem_dir."""
    yields = {}
    for path in sorted(glob.glob(os.path.join(peritem_dir, "PROB_*.output.json"))):
        symbol = _symbol_from_filename(path)
        if not symbol:
            continue
        try:
            with open(path) as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict):
            continue
        # Prefer the symbol the model echoed back; fall back to the filename token.
        symbol = (str(data.get("symbol")).strip() or symbol) if data.get("symbol") else symbol
        yields[symbol] = _num(data.get("dividend_yield_pct"))
    return yields


def stamp(context=None, reportPath=None, peritemDir=None, **kwargs):
    context = context or {}
    base = context.get("_workflow_base_directory") or context.get("_task_working_directory") or "."
    task_wd = context.get("_task_working_directory") or "."

    allocations_path = os.path.join(base, "port62pos.csv")
    if not os.path.exists(allocations_path):
        return {"error": f"allocations CSV not found at '{allocations_path}'"}

    peritem_dir = os.path.join(task_wd, peritemDir or "../01_lookupDividend")
    yields = _load_peritem_yields(peritem_dir)
    if not yields:
        return {"error": f"no per-item dividend replies found under '{peritem_dir}'"}

    # Join authoritative allocations (CSV) with model-supplied yields (per-item JSON).
    contribs = []  # (symbol, allocation%, yield%, weightedContribution%)
    missing = []   # positions with no matching per-item reply
    with open(allocations_path) as f:
        for r in csv.DictReader(f):
            symbol = (r.get("Symbol") or "").strip()
            if not symbol:
                continue
            alloc = _num(r.get("Percentage"))
            if symbol in yields:
                yld = yields[symbol]
                contribs.append((symbol, alloc, yld, alloc * yld / 100.0))
            else:
                missing.append(symbol)

    n = len(contribs)
    total = sum(c[3] for c in contribs)               # exact allocation-weighted yield
    alloc_sum = sum(c[1] for c in contribs)
    simple_avg = (sum(c[2] for c in contribs) / n) if n else 0.0
    zero = [c[0] for c in contribs if c[2] <= 0.0]
    payers = n - len(zero)
    top = sorted(contribs, key=lambda c: c[3], reverse=True)[:5]

    block = [
        "# Portfolio Totals — exact weighted sum of the AI's per-position yields",
        "",
        f"- **Allocation-weighted dividend yield: {total:.2f}%** "
        f"(exact sum of {n} weighted contributions; allocations total {alloc_sum:.1f}%)",
        f"- Simple average yield across positions: {simple_avg:.2f}%",
        f"- Positions matched: {n}  |  dividend payers: {payers}  |  zero-dividend: {len(zero)}"
        + (f" ({', '.join(zero)})" if zero else ""),
        "- Top contributors: " + ", ".join(f"{s} ({wc:.4f}%)" for s, _, _, wc in top),
    ]
    if missing:
        block.append(f"- Positions without a per-item reply (excluded): {', '.join(missing)}")
    block += [
        "",
        "> This is the **exact** allocation-weighted sum of the per-position yields the AI looked "
        "up — it removes the model's summation drift, but its accuracy is bounded by those yields "
        "(per-position AI lookups can run high or low). For an authoritative figure, feed "
        "authoritative dividend data (a broker or database export) in place of the AI lookups. "
        "The narrative below is the model's qualitative analysis.",
        "",
        "---",
        "",
    ]
    header = "\n".join(block)

    summary_path = context.get("_file_input_0", "")
    summary_text = ""
    if summary_path and os.path.exists(summary_path):
        with open(summary_path) as s:
            summary_text = s.read()

    out = reportPath or os.path.join(task_wd, "portfolio_report.md")
    with open(out, "w") as g:
        g.write(header + summary_text)

    return {"total_yield_pct": round(total, 4), "positions": n,
            "payers": payers, "zero_dividend": ",".join(zero) if zero else "none",
            "missing": ",".join(missing) if missing else "none", "report": out}
