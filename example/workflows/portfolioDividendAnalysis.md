# Portfolio Dividend Analysis — CSV-Driven Per-Item AI Workflow

## Executive Summary

The **portfolioDividendAnalysis** workflow demonstrates JarvisAgent's
**per-item fan-out** capability driven by a local CSV file. It reads a
60-position investment portfolio, enriches it with **live dividend data from a
real source**, dispatches one AI call per position to format the figures and
add qualitative analysis, aggregates all results into a single portfolio
summary, and stamps a **deterministically-computed grand total** onto it.

This is a four-task pipeline that showcases:

- **Real-data enrichment + AI-as-formatter** — a Stage-0 Python task pulls
  authoritative dividend figures from the sibling `planner` app's database, so
  the AI works with ground-truth numbers instead of recalling (often stale)
  values from training.
- **`csv` filter source** — reads rows from a CSV file with header-based
  field binding.
- **`per_item` mode with `{{binding.field}}` template substitution** — each
  CSV row is injected into PROB file paths and inline content.
- **`cntx_files` glob patterns** — the downstream summary task dynamically
  collects all per-item outputs using `PROB_*.output.txt` instead of
  hardcoding 60 file references.
- **Deterministic aggregation** — a final Python task recomputes the exact
  allocation-weighted yield from the enriched data and prepends it to the
  summary, because language models drift when summing dozens of figures.
- **Four-stage pipeline** — enrich (Python) → fan-out (per position) →
  aggregate (AI summary) → verify totals (Python).

---

## 1. Big Picture

```
+-----------------------+
|  exportPlannerData    |   reads port62pos.csv + planner DB
|  python (Stage 0)     | --> writes port62pos_enriched.csv
+-----------+-----------+    (Symbol,Name,Percentage,DividendYield,
            |                 AnnualDividendPerShare,Price,AsOf)
  depends_on|
            v
+----------------+  60 positions   +-----------------------+
|  positions     |  per item       |  lookupDividend       |
|  csv filter    | -------------->  |  ai_call (per_item)   |
|  enriched CSV  | ------+          |  60 AI calls          |
+----------------+       |          +-----------+-----------+
                         |                      | depends_on
                         |  glob collect        v
                         +--- PROB_*.output -> +-----------------------+
                                               |  portfolioSummary     |
                                               |  ai_call (single)     |
                                               +-----------+-----------+
                                                 depends_on|
                                                           v
                                               +-----------------------+
                                               |  stampPortfolioTotals |
                                               |  python (Stage 3)     |
                                               |  exact total → report |
                                               +-----------------------+
```

### What happens when you click Run

1. **Stage 0 — enrichment.** The `exportPlannerData` Python task joins
   `port62pos.csv` (Symbol, Name, Percentage) against the `planner` app's
   `tickers` table and writes `port62pos_enriched.csv` beside it, adding
   authoritative `DividendYield`, `AnnualDividendPerShare`, `Price`, and `AsOf`
   columns. Symbols absent from the planner get blank columns — never a guess.

2. **Filter evaluation** — `FilterEngine` reads `port62pos_enriched.csv` (60
   data rows + 1 header), producing 60 `FilterItem` objects with the enriched
   fields bound under `pos`.

3. **Fan-out** — The runtime creates 60 child task instances
   (`lookupDividend#0` through `lookupDividend#59`). Each child receives the
   enriched CSV fields as `{{pos.*}}` template variables.

4. **Template substitution** — For each child, `{{pos.Symbol}}` and
   `{{pos.row_number_padded}}` are replaced in both the PROB filename
   (e.g. `PROB_NVDA_037.txt`) and its content (which carries the authoritative
   figures).

5. **AI dispatch** — 60 async AI requests run in parallel. Each **copies the
   supplied figures through unchanged**, computes the weighted contribution,
   and adds a 5-year trend label + one-line note.

6. **Aggregation** — Once all 60 children succeed, `portfolioSummary` becomes
   ready. Its `cntx_files` glob `../01_lookupDividend/PROB_*.output.txt`
   dynamically collects all 60 output files, and a single AI call produces the
   final portfolio summary.

7. **Verify totals** — `stampPortfolioTotals` recomputes the exact
   allocation-weighted yield straight from `port62pos_enriched.csv` and prepends
   a "Verified Portfolio Totals (computed)" block to the AI summary, writing the
   combined `portfolio_report.md`. This is the authoritative grand total — the
   model's own summation is not trusted for the headline number.

---

## 2. Prerequisites

### AI Provider Key

An AI provider key (e.g. `"openai"`) must exist in the JarvisAgent KeyManager.
The workflow defaults specify the `gpt-4.1-mini` interface.

### Dividend data source — the `planner` app

Stage 0 reads the sibling `planner` project's SQLite database (table `tickers`,
columns `symbol, current_price, ttm_dividend, yield_pct, last_fetched`).
`ttm_dividend` is the **forward** annual $/share (suspended payers read `$0`);
`yield_pct` is the dividend yield. The path defaults to the planner's database
and can be overridden with the `PLANNER_DB` environment variable. Refresh the
planner's market data before a run if you want current prices in the `AsOf`
column.

### CSV Data File

`port62pos.csv` ships inside the `.jcwf` and is extracted to the workflow's
runtime directory; Stage 0 derives `port62pos_enriched.csv` from it.

---

## 3. Workflow File Structure

```
example/workflows/
  portfolioDividendAnalysis.jcwf    ← workflow definition (bundles port62pos.csv)
  portfolioDividendAnalysis.md      ← this document
scripts/
  export_dividends.py               ← Stage-0 enrichment (module: export_dividends)
  portfolio_totals.py               ← Stage-3 deterministic totals (module: portfolio_totals)
```

At runtime the `.jcwf` is extracted into the `workflows/` directory; Stage 0
writes `port62pos_enriched.csv` beside the extracted `port62pos.csv`.

---

## 4. CSV Data Format

The bundled source `port62pos.csv` has 60 data rows with a header:

```csv
Symbol,Name,Percentage
LQD,iShares iBoxx USD Investment Grade Corporate Bond ETF,7.11%
JPM,JPMorgan Chase & Co.,4.28%
...
UPS,United Parcel Service Inc.,0.05%
```

Stage 0 writes the **enriched** `port62pos_enriched.csv` that the filter
actually reads — the same rows plus authoritative dividend columns:

```csv
Symbol,Name,Percentage,DividendYield,AnnualDividendPerShare,Price,AsOf
LQD,iShares iBoxx ...,7.11%,4.54%,4.94,108.93,2026-06-01
JPM,JPMorgan Chase & Co.,4.28%,2.02%,6.00,296.58,2026-06-01
...
```

The filter binding `"pos"` makes every column available as `{{pos.<Column>}}`.

---

## 5. JCWF Anatomy

### 5.1 Filter Definition

```jsonc
"filters": [
  {
    "id": "positions",
    "source": {
      "kind": "csv",
      "path": "port62pos_enriched.csv",
      "delimiter": ",",
      "has_header": true
    },
    "binding": "pos",
    "max_items": 100
  }
]
```

Key points:

- **`path: "port62pos_enriched.csv"`** — the file Stage 0 produces. Because
  `lookupDividend` `depends_on` Stage 0, the enriched file always exists before
  the filter is evaluated.
- **`kind: "csv"`** — reads a local CSV via `FilterEngine::EvaluateCsv()`.
- **`has_header: true`** — first row is column names, not data.
- **`binding: "pos"`** — all values are injected with the `pos.` prefix.
- **`max_items: 100`** — safety cap (the CSV has 60 rows).

### 5.2 Available Template Variables

Each child task instance has access to:

| Variable | Source | Example Value |
|----------|--------|---------------|
| `{{pos.Symbol}}` | CSV column "Symbol" | `NVDA` |
| `{{pos.Name}}` | CSV column "Name" | `NVIDIA Corporation` |
| `{{pos.Percentage}}` | CSV column "Percentage" | `3.75%` |
| `{{pos.DividendYield}}` | enriched column | `0.03%` |
| `{{pos.AnnualDividendPerShare}}` | enriched column | `0.04` |
| `{{pos.Price}}` | enriched column | `141.22` |
| `{{pos.AsOf}}` | enriched column (data date) | `2026-06-01` |
| `{{pos.index}}` | 0-based row index | `5` |
| `{{pos.row_number}}` | 1-based row number | `6` |
| `{{pos.row_number_padded}}` | zero-padded row number | `037` |

### 5.3 Task 0 — `exportPlannerData` (python)

```jsonc
"exportPlannerData": {
  "type": "python",
  "working_directory": "../../queue/portfolioDividendAnalysis/00_exportPlannerData",
  "params": { "module": "export_dividends", "function": "export" }
}
```

- Runs `scripts/export_dividends.py::export`, which reads `port62pos.csv` and
  the planner DB and writes `port62pos_enriched.csv` into the workflow base
  directory (via `context["_workflow_base_directory"]`).
- It is the **single source of factual numbers** — the AI never invents them.

### 5.4 Task 1 — `lookupDividend` (per_item)

```jsonc
"lookupDividend": {
  "type": "ai_call",
  "mode": "per_item",
  "filter": "positions",
  "depends_on": ["exportPlannerData"],
  "working_directory": "../../queue/portfolioDividendAnalysis/01_lookupDividend",
  "queue_binding": {
    "stng_files": [{ "path": "STNG_succinct.txt", "content": "..." }],
    "task_files": [{ "path": "TASK_dividendLookup.txt", "content": "..." }],
    "cntx_files": [{ "path": "CNTX_portfolio.txt", "content": "..." }],
    "prob_files": [{
      "path": "PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt",
      "content": "Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nPortfolio Allocation: {{pos.Percentage}}\nDividend Yield: {{pos.DividendYield}}\nAnnual Dividend/Share: ${{pos.AnnualDividendPerShare}}\nPrice: ${{pos.Price}}  (as of {{pos.AsOf}})\n"
    }]
  }
}
```

- **`depends_on: ["exportPlannerData"]`** — guarantees the enriched CSV is
  fresh before the fan-out evaluates the filter.
- **`mode: "per_item"`** + **`filter: "positions"`** — fans out to 60 children.
- **`prob_files`** — template-substituted per child, carrying the authoritative
  figures. Produces `PROB_LQD_001.txt` … `PROB_UPS_060.txt`; the
  `{{pos.row_number_padded}}` suffix keeps filenames unique.
- The TASK instructs the model to **copy the supplied yield / per-share / price
  through unchanged**, compute `Weighted Contribution = Allocation% ×
  DividendYield% ÷ 100`, and add only a 5-year trend label and a one-line note.

### 5.5 Task 2 — `portfolioSummary` (single, with glob)

```jsonc
"portfolioSummary": {
  "type": "ai_call",
  "mode": "single",
  "depends_on": ["lookupDividend"],
  "working_directory": "../../queue/portfolioDividendAnalysis/02_portfolioSummary",
  "queue_binding": {
    "stng_files": [{ "path": "STNG_succinct.txt", "content": "..." }],
    "task_files": [{ "path": "TASK_portfolioSummary.txt", "content": "..." }],
    "prob_files": [{ "path": "PROB_summarize.txt", "content": "..." }],
    "cntx_files": [ "../01_lookupDividend/PROB_*.output.txt" ]
  }
}
```

- **`depends_on: ["lookupDividend"]`** — waits for all 60 children.
- **`cntx_files` glob** — `PROB_*.output.txt` dynamically collects all per-item
  outputs at execution time, sorted lexicographically, each materialized as a
  `CNTX_*` file. The workflow adapts automatically to any number of positions.
- The AI receives all 60 (now accurate) dividend reports and produces a single
  executive summary.

### 5.6 Task 3 — `stampPortfolioTotals` (python)

```jsonc
"stampPortfolioTotals": {
  "type": "python",
  "depends_on": ["portfolioSummary"],
  "working_directory": "../../queue/portfolioDividendAnalysis/03_stampTotals",
  "file_inputs": ["../02_portfolioSummary/PROB_summarize.output.txt"],
  "file_outputs": ["portfolio_report.md"],
  "params": { "module": "portfolio_totals", "function": "stamp" },
  "outputs": { "reportPath": { "type": "string" } }
}
```

- Runs `scripts/portfolio_totals.py::stamp`. It recomputes the exact
  allocation-weighted yield (`Σ allocation% × yield% ÷ 100`) directly from
  `port62pos_enriched.csv` — the same ground-truth data the per-item stage used.
- **`file_inputs`** resolves the AI summary (sibling `02_portfolioSummary`
  output), injected as `context["_file_input_0"]`; **`outputs.reportPath`**
  (the `Path` suffix triggers the convention) passes the resolved
  `portfolio_report.md` path as a kwarg.
- It prepends a **"Verified Portfolio Totals (computed)"** block (exact total,
  payer/zero counts, top contributors) to the AI's narrative — language models
  drift when adding dozens of figures, so the headline number is computed in
  Python, not by the model.

---

## 6. Runtime Artifacts

After a successful run:

```
workflows/portfolioDividendAnalysis/
  port62pos.csv                         ← bundled source
  port62pos_enriched.csv                ← written by Stage 0

queue/portfolioDividendAnalysis/01_lookupDividend/
  STNG_succinct.txt / TASK_dividendLookup.txt / CNTX_portfolio.txt   ← shared
  PROB_LQD_001.txt / PROB_LQD_001.output.txt                         ← per-item pair
  ...                                                                ← 60 pairs total

queue/portfolioDividendAnalysis/02_portfolioSummary/
  STNG_succinct.txt / TASK_portfolioSummary.txt / PROB_summarize.txt
  PROB_summarize.output.txt             ← AI executive summary
  CNTX_PROB_LQD_001.txt ...             ← 60 files materialized from the glob

queue/portfolioDividendAnalysis/03_stampTotals/
  portfolio_report.md                   ← final deliverable: verified totals + AI summary
```

---

## 7. Sample Output

### Per-Position Report (PROB_NVDA_037.output.txt)

```
Symbol: NVDA
Name: NVIDIA Corporation
Allocation: 3.75%
Dividend Yield: 0.03%
Annual Dividend/Share: $0.04
Price: $141.22 (as of 2026-06-01)
Weighted Contribution: 0.0011%
Dividend Trend (5yr): Growing
Notes: Minimal dividend; growth-focused semiconductor company.
```

The dividend yield, per-share, and price are passed through from the enriched
CSV unchanged; only the weighted contribution, trend label, and note are
produced by the AI.

### Final Report (03_stampTotals/portfolio_report.md)

The deliverable opens with the Stage-3 **Verified Portfolio Totals** block —
the exact allocation-weighted yield, payer/zero counts, and top contributors,
computed in Python — followed by the AI executive summary (estimated total,
top/bottom contributor tables, trend overview, asset-class breakdown, and
key observations). The verified header is the number to trust; the AI's own
stated total is qualitative narrative that can drift on large sums.

---

## 8. Key Features Demonstrated

### 8.1 Real-data enrichment, AI as formatter

Factual, time-sensitive numbers come from a real source (the planner DB) via
the Stage-0 Python task; the AI is reserved for what it does well — formatting,
the weighted-contribution arithmetic, and qualitative trend/notes. A `$0.00`
dividend (suspended payer) flows through correctly rather than being "corrected"
to a remembered value.

### 8.2 CSV Filter Source (JCWF v1.1)

`FilterEngine::Evaluate()` dispatches to `EvaluateCsv()`: the header row sets
column names, each data row becomes a `FilterItem` with named fields (from the
header) and positional fields (`col_0`, `col_1`, …); `max_items` and range
filtering apply during reading.

### 8.3 Glob Patterns in `cntx_files` (JCWF v1.1)

The summary task uses `"../01_lookupDividend/PROB_*.output.txt"` to discover all
per-item outputs at execution time. `ExpandCntxFileGlobs()` detects the wildcard,
resolves the parent directory relative to the task working directory, collects
and sorts the matches, and materializes each as `CNTX_<basename>` — no need to
hardcode 60 paths, and it scales with the data.

### 8.4 Deterministic bookends around the AI

A common shape for data-grounded AI workflows: a deterministic enrichment stage
produces ground-truth inputs, per-item tasks fan out in parallel, a single
aggregate task synthesizes the result, and a deterministic stage recomputes the
load-bearing numbers. The AI handles language and per-item formatting; Python
owns the facts and the arithmetic that must be exact (the grand total). `depends_on`
edges serialize the four stages: enrich → fan-out → aggregate → verify.

---

## 9. Key C++ Components

| Component | Role in This Workflow |
|-----------|----------------------|
| `PythonTaskExecutor` | Runs the Stage-0 enrichment + Stage-3 totals scripts |
| `FilterEngine::EvaluateCsv` | Parses the enriched CSV with header + delimiter |
| `WorkflowRuntimeManager` | Per-item fan-out + aggregation of child results |
| `AiCallTaskExecutor` | Template substitution, queue file materialization |
| `ExpandCntxFileGlobs` | Glob expansion for dynamic `cntx_files` collection |

---

## 10. How to Run

```bash
# 1. Ensure the .jcwf is in the runtime folder (extracts port62pos.csv)
cp example/workflows/portfolioDividendAnalysis.jcwf workflows/

# 2. Ensure an AI provider key exists in the Keys page (e.g. "openai")
#    and the planner DB is reachable (or set PLANNER_DB).

# 3. Start JarvisAgent, then: web UI → Workflows → Portfolio Dividend Analysis → Run
```

Stages 0 and 3 (Python) each run in well under a second; the 60 parallel
per-item calls plus the single summary dominate the wall-clock. The AI tasks use
the engine's size-aware timeout budget (no fixed per-task timeout), so the
summary call — which folds in all 60 reports — gets enough time.

---

## 11. Adapting This Workflow

### Different Portfolio

Replace `port62pos.csv` with any CSV that has `Symbol`, `Name`, and
`Percentage` columns. The enrichment, per-item fan-out, and glob-based summary
all scale with the row count automatically. Symbols not present in the planner
DB simply get blank dividend columns.

### Different data source

Point `export_dividends.py` at a different database or API (or set `PLANNER_DB`).
The contract is only that the enriched CSV gains the `DividendYield`,
`AnnualDividendPerShare`, `Price`, and `AsOf` columns the PROB template expects.

### Different AI Provider

Change the `defaults.ai` section in `global.json`:

```jsonc
"defaults": {
  "ai": { "provider": "google", "model": "gemini-2.5-flash" }
}
```

### Adding a Stage

Chain another shell or Python task with `depends_on: ["stampPortfolioTotals"]`
(e.g. to render a PDF from `portfolio_report.md`, or email/upload it).
