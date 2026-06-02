# @jarvis-script
# @short: Compute exact portfolio dividend totals and prepend them to the AI summary
# @params: context (reads _workflow_base_directory + _file_input_0); reportPath (output file)
# @description: Stage 3 of portfolioDividendAnalysis. Recomputes the allocation-weighted
#   dividend yield directly from port62pos_enriched.csv (the same ground-truth data the
#   per-item stage used), so the grand total is exact arithmetic rather than the language
#   model's summation (LLMs drift when adding dozens of figures). Prepends a "Verified
#   Portfolio Totals (computed)" block to the AI summary and writes the combined report.
# @outputs: portfolio_report.md — verified totals header + the AI executive summary

import csv
import os


def _num(s):
    s = (s or "").strip().rstrip("%")
    try:
        return float(s)
    except ValueError:
        return 0.0


def stamp(context=None, reportPath=None, **kwargs):
    context = context or {}
    base = context.get("_workflow_base_directory") or context.get("_task_working_directory") or "."
    enriched = os.path.join(base, "port62pos_enriched.csv")
    if not os.path.exists(enriched):
        return {"error": f"enriched CSV not found at '{enriched}'"}

    contribs = []  # (symbol, allocation%, yield%, weightedContribution%)
    with open(enriched) as f:
        for r in csv.DictReader(f):
            alloc = _num(r.get("Percentage"))
            yld = _num(r.get("DividendYield"))
            contribs.append((r["Symbol"].strip(), alloc, yld, alloc * yld / 100.0))

    n = len(contribs)
    total = sum(c[3] for c in contribs)                 # exact allocation-weighted yield
    alloc_sum = sum(c[1] for c in contribs)
    simple_avg = (sum(c[2] for c in contribs) / n) if n else 0.0
    zero = [c[0] for c in contribs if c[2] <= 0.0]
    payers = n - len(zero)
    top = sorted(contribs, key=lambda c: c[3], reverse=True)[:5]

    block = [
        "# Verified Portfolio Totals (computed deterministically)",
        "",
        f"- **Allocation-weighted dividend yield: {total:.2f}%** "
        f"(exact sum of {n} weighted contributions; allocations total {alloc_sum:.1f}%)",
        f"- Simple average yield across positions: {simple_avg:.2f}%",
        f"- Positions: {n}  |  dividend payers: {payers}  |  zero-dividend: {len(zero)}"
        + (f" ({', '.join(zero)})" if zero else ""),
        "- Top contributors: " + ", ".join(f"{s} ({wc:.4f}%)" for s, _, _, wc in top),
        "",
        "> Computed from the enriched portfolio data, not by the language model — the "
        "authoritative grand total. The narrative below is the model's qualitative analysis.",
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

    out = reportPath or os.path.join(
        context.get("_task_working_directory", "."), "portfolio_report.md")
    with open(out, "w") as g:
        g.write(header + summary_text)

    return {"total_yield_pct": round(total, 4), "positions": n,
            "payers": payers, "zero_dividend": ",".join(zero) if zero else "none",
            "report": out}
