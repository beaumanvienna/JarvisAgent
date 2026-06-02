# @jarvis-script
# @short: Enrich port62pos.csv with live dividend data from the planner DB
# @params: (context-only; reads _workflow_base_directory; DB path via PLANNER_DB env or context['planner_db'])
# @description: Stage 0 of portfolioDividendAnalysis. Joins the workflow's
#   port62pos.csv (Symbol,Name,Percentage) against the sibling `planner` app's
#   SQLite `tickers` table (symbol, current_price, ttm_dividend, yield_pct,
#   last_fetched) and writes port62pos_enriched.csv beside it with authoritative
#   DividendYield / AnnualDividendPerShare / Price / AsOf columns. The per-item
#   ai_call then copies these numbers through verbatim instead of recalling
#   (stale/hallucinated) figures from training. Symbols absent from the planner
#   get blank columns — never a guessed value.
# @outputs: port62pos_enriched.csv (beside port62pos.csv in the workflow base dir)

import csv
import os
import sqlite3

# Default matches the documented planner location (doc/.. example/workflows/
# portfolioDividendAnalysis.md §12.2); override with PLANNER_DB to run elsewhere.
DEFAULT_DB = "/home/beaumanvienna/dev/planner/backend/planner.db"


def export(context=None, **kwargs):
    context = context or {}
    base = context.get("_workflow_base_directory") or context.get("_task_working_directory") or "."
    src = os.path.join(base, "port62pos.csv")
    out = os.path.join(base, "port62pos_enriched.csv")
    db = context.get("planner_db") or os.environ.get("PLANNER_DB") or DEFAULT_DB

    if not os.path.exists(db):
        return {"error": f"planner DB not found at '{db}' (set PLANNER_DB or context.planner_db)"}
    if not os.path.exists(src):
        return {"error": f"source CSV not found at '{src}'"}

    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    tk = {r["symbol"]: r for r in conn.execute("SELECT * FROM tickers")}
    conn.close()

    total = 0
    matched = 0
    missing = []
    with open(src) as f, open(out, "w", newline="") as g:
        # lineterminator="\n": the j9t CSV filter splits on "\n" and does not strip a
        # trailing "\r", so csv's default CRLF would leave the last column bound as
        # "AsOf\r" and break {{pos.AsOf}}. Unix newlines keep every column clean.
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["Symbol", "Name", "Percentage", "DividendYield",
                    "AnnualDividendPerShare", "Price", "AsOf"])
        for r in csv.DictReader(f):
            s = r["Symbol"].strip()
            total += 1
            t = tk.get(s)
            if not t:  # symbol not in planner — leave blank, never guess
                w.writerow([s, r["Name"].strip(), r["Percentage"].strip(), "", "", "", ""])
                missing.append(s)
                continue
            matched += 1
            w.writerow([s, r["Name"].strip(), r["Percentage"].strip(),
                        f'{t["yield_pct"]:.2f}%', f'{t["ttm_dividend"]:.2f}',
                        f'{t["current_price"]:.2f}', (t["last_fetched"] or "")[:10]])

    return {"enriched_csv": out, "rows": total, "matched": matched,
            "missing": ",".join(missing) if missing else "none"}
