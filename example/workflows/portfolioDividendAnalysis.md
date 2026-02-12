# Portfolio Dividend Analysis — CSV-Driven Per-Item AI Workflow

## Executive Summary

The **portfolioDividendAnalysis** workflow demonstrates JarvisAgent's
**per-item fan-out** capability driven by a local CSV file. It reads a
60-position investment portfolio, dispatches one AI call per position to
look up dividend data, then aggregates all results into a single portfolio
summary.

This is a two-task pipeline that showcases:

- **`csv` filter source** — reads rows from a CSV file with header-based
  field binding.
- **`per_item` mode with `{{binding.field}}` template substitution** — each
  CSV row is injected into PROB file paths and inline content.
- **`cntx_files` glob patterns** — the downstream summary task dynamically
  collects all per-item outputs using `PROB_*.output.txt` instead of
  hardcoding 60 file references.
- **Two-stage AI pipeline** — fan-out (per position) → aggregate (single
  summary).

---

## 1. Big Picture

```
                                                  +-----------------------+
                                    per item      |  lookupDividend       |
+----------------+  60 positions  ------------->  |  ai_call (per_item)   |
|  positions     |                                |  60 AI calls          |
|  csv filter    | ------+                        +-----------+-----------+
|  port62pos     |       |                                    |
+----------------+       |                          depends_on|
                         |                                    v
                         |                        +-----------------------+
                         |   glob collect         |  portfolioSummary     |
                         +---  PROB_*.output  --> |  ai_call (single)     |
                                                  |  1 AI call            |
                                                  +-----------------------+
```

### What happens when you click Run

1. **Filter evaluation** — `FilterEngine` reads `port62pos.csv` (60 data rows
   + 1 header), producing 60 `FilterItem` objects with fields `Symbol`, `Name`,
   and `Percentage`.

2. **Fan-out** — The runtime creates 60 child task instances
   (`lookupDividend#0` through `lookupDividend#59`). Each child receives
   the CSV fields as `{{pos.*}}` template variables.

3. **Template substitution** — For each child, `{{pos.Symbol}}` is replaced
   in both the PROB filename (`PROB_NVDA.txt`) and its content.

4. **AI dispatch** — 60 async AI requests are dispatched in parallel. Each
   asks the model to look up dividend yield, compute weighted contribution,
   and report the 5-year trend.

5. **Completion** — Output files `PROB_LQD.output.txt` through
   `PROB_UPS.output.txt` are written as responses arrive.

6. **Aggregation** — Once all 60 children succeed, `portfolioSummary` becomes
   ready. Its `cntx_files` glob pattern `../01_lookupDividend/PROB_*.output.txt`
   dynamically collects all 60 output files. A single AI call produces the
   final portfolio summary.

---

## 2. Prerequisites

### AI Provider Key

An AI provider key (e.g. `"openai"`) must exist in the JarvisAgent KeyManager.
The workflow defaults specify `provider: "openai"` and `model: "gpt-4.1-mini"`.

### CSV Data File

`port62pos.csv` must be present in the `workflows/` directory alongside the
`.jcwf` file.

---

## 3. Workflow File Structure

```
example/workflows/
  portfolioDividendAnalysis.jcwf    ← workflow definition
  portfolioDividendAnalysis.md      ← this document
  port62pos.csv                     ← 60-position portfolio (Symbol, Name, Percentage)
```

At runtime, all files are copied to the `workflows/` directory.

---

## 4. CSV Data Format

`port62pos.csv` has 60 data rows with a header:

```csv
Symbol,Name,Percentage
LQD,iShares iBoxx USD Investment Grade Corporate Bond ETF,7.11%
BNDX,Vanguard Total International Bond ETF,5.32%
WELL,Welltower Inc.,4.48%
JPM,JPMorgan Chase & Co.,4.28%
...
UPS,United Parcel Service Inc.,0.05%
```

The filter binding `"pos"` makes these available as `{{pos.Symbol}}`,
`{{pos.Name}}`, and `{{pos.Percentage}}`.

---

## 5. JCWF Anatomy

### 5.1 Filter Definition

```jsonc
"filters": [
  {
    "id": "positions",
    "source": {
      "kind": "csv",
      "path": "port62pos.csv",
      "delimiter": ",",
      "has_header": true
    },
    "binding": "pos",
    "max_items": 100
  }
]
```

Key points:

- **`kind: "csv"`** — reads a local CSV file via `FilterEngine::EvaluateCsv()`.
- **`has_header: true`** — first row is treated as column names, not data.
- **`binding: "pos"`** — all values are injected with the `pos.` prefix.
- **`max_items: 100`** — safety cap (the CSV has 60 rows).

### 5.2 Available Template Variables

Each child task instance has access to:

| Variable | Source | Example Value |
|----------|--------|---------------|
| `{{pos.Symbol}}` | CSV column "Symbol" | `NVDA` |
| `{{pos.Name}}` | CSV column "Name" | `NVIDIA Corporation` |
| `{{pos.Percentage}}` | CSV column "Percentage" | `3.75%` |
| `{{pos.index}}` | 0-based row index | `5` |
| `{{pos.row_number}}` | 1-based row number | `6` |
| `{{pos.line}}` | Raw CSV line | `NVDA,NVIDIA Corporation,3.75%` |
| `{{pos.col_0}}` | Positional column 0 | `NVDA` |

### 5.3 Task 1 — `lookupDividend` (per_item)

```jsonc
"lookupDividend": {
  "type": "ai_call",
  "mode": "per_item",
  "filter": "positions",
  "working_directory": "../queue/portfolioDividendAnalysis/01_lookupDividend",
  "queue_binding": {
    "stng_files": [{ "path": "STNG_succinct.txt", "content": "..." }],
    "task_files": [{ "path": "TASK_dividendLookup.txt", "content": "..." }],
    "cntx_files": [{ "path": "CNTX_portfolio.txt", "content": "..." }],
    "prob_files": [{
      "path": "PROB_{{pos.Symbol}}.txt",
      "content": "Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nPortfolio Allocation: {{pos.Percentage}}\n"
    }]
  }
}
```

- **`mode: "per_item"`** + **`filter: "positions"`** — triggers fan-out of
  60 children.
- **`prob_files`** — template-substituted per child. Produces `PROB_LQD.txt`,
  `PROB_BNDX.txt`, ..., `PROB_UPS.txt`.
- **`cntx_files`** — inline content providing portfolio-level context
  (shared by all children).
- The AI is instructed to return a structured report with yield, weighted
  contribution, and 5-year trend.

### 5.4 Task 2 — `portfolioSummary` (single, with glob)

```jsonc
"portfolioSummary": {
  "type": "ai_call",
  "mode": "single",
  "depends_on": ["lookupDividend"],
  "working_directory": "../queue/portfolioDividendAnalysis/02_portfolioSummary",
  "queue_binding": {
    "stng_files": [{ "path": "STNG_succinct.txt", "content": "..." }],
    "task_files": [{ "path": "TASK_portfolioSummary.txt", "content": "..." }],
    "prob_files": [{ "path": "PROB_summarize.txt", "content": "..." }],
    "cntx_files": [
      "../01_lookupDividend/PROB_*.output.txt"
    ]
  }
}
```

- **`depends_on: ["lookupDividend"]`** — waits for all 60 children to
  complete before starting.
- **`cntx_files` glob pattern** — `PROB_*.output.txt` dynamically collects
  all per-item output files. The runtime expands this at execution time,
  sorts matches lexicographically, and materializes each as a `CNTX_*` file.
  This means the workflow adapts automatically if the CSV changes to 30 or
  100 positions.
- The AI receives all 60 dividend reports as context and produces a single
  executive summary.

---

## 6. Runtime Artifacts

After a successful run:

```
queue/portfolioDividendAnalysis/01_lookupDividend/
  STNG_succinct.txt                     ← written once (shared)
  TASK_dividendLookup.txt               ← written once (shared)
  CNTX_portfolio.txt                    ← written once (shared)

  PROB_LQD.txt                          ← per-item input (position details)
  PROB_LQD.output.txt                   ← per-item output (dividend report)
  PROB_BNDX.txt
  PROB_BNDX.output.txt
  ...
  PROB_UPS.txt
  PROB_UPS.output.txt                   ← 60 pairs total

queue/portfolioDividendAnalysis/02_portfolioSummary/
  STNG_succinct.txt
  TASK_portfolioSummary.txt
  PROB_summarize.txt
  PROB_summarize.output.txt             ← final portfolio summary

  CNTX_PROB_LQD.txt                    ← materialized from glob (60 files)
  CNTX_PROB_BNDX.txt
  ...
  CNTX_PROB_UPS.txt
```

A filter manifest is written to:

```
workflows/positions/positions.manifest.json
```

This enables incremental re-runs: on subsequent executions, only positions
whose CSV row has changed are re-evaluated.

---

## 7. Sample Output

### Per-Position Report (PROB_NVDA.output.txt)

```
Symbol: NVDA
Name: NVIDIA Corporation
Allocation: 3.75%
Dividend Yield: 0.03%
Annual Dividend/Share: $0.04
Weighted Contribution: 0.0011%
Dividend Trend (5yr): Growing
Notes: Minimal dividend; growth-focused semiconductor company.
```

### Portfolio Summary (PROB_summarize.output.txt)

The summary includes:

1. **Estimated Total Portfolio Dividend Yield** — sum of all weighted
   contributions (e.g. ~3.02%)
2. **Top 10 Dividend Contributors** — sorted table by weighted contribution
3. **Bottom 10 Contributors** — positions with lowest yield
4. **Dividend Trend Overview** — e.g. 44 Growing, 13 Stable, 3 Declining
5. **Asset Class Breakdown** — Equities, REITs, Bond ETFs, Other
6. **Key Observations** — bullet points on income profile, concentration
   risk, and suggestions

---

## 8. Key Features Demonstrated

### 8.1 CSV Filter Source (JCWF v1.1)

The simplest filter kind — reads a local file, parses header and rows,
and produces one `FilterItem` per data row.

**How it works internally:**

1. `FilterEngine::Evaluate()` dispatches to `EvaluateCsv()`.
2. The header row is parsed to determine column names.
3. Each data row becomes a `FilterItem` with named fields (from header)
   and positional fields (`col_0`, `col_1`, ...).
4. Range filtering and `max_items` are applied during reading.

### 8.2 Glob Patterns in `cntx_files` (JCWF v1.1)

The summary task uses `"../01_lookupDividend/PROB_*.output.txt"` to
dynamically discover all per-item outputs at execution time.

**How it works internally:**

1. `ExpandCntxFileGlobs()` detects `*` or `?` in the path.
2. The parent directory is resolved relative to the task working directory.
3. All regular files matching the filename pattern are collected and sorted.
4. Each match is expanded into an individual `cntx_files` entry.
5. Materialization copies each as `CNTX_<basename>` in the task folder.

This eliminates the need to hardcode 60 file paths and adapts automatically
to any number of positions.

### 8.3 Two-Stage Per-Item → Aggregate Pipeline

This is a common pattern for large-scale AI workflows:

1. **Stage 1 (fan-out):** Per-item tasks run in parallel, each producing
   a focused report.
2. **Stage 2 (aggregate):** A single task collects all reports and
   synthesizes a summary.

The `depends_on` relationship ensures Stage 2 never starts until all
Stage 1 children have completed.

---

## 9. Key C++ Components

| Component | Role in This Workflow |
|-----------|----------------------|
| `FilterEngine::EvaluateCsv` | Parses CSV with header, delimiter, range support |
| `FilterManifestManager` | Writes manifest for incremental freshness tracking |
| `WorkflowRuntimeManager` | Per-item fan-out + aggregation of child results |
| `AiCallTaskExecutor` | Template substitution, queue file materialization |
| `ExpandCntxFileGlobs` | Glob expansion for dynamic `cntx_files` collection |

---

## 10. How to Run

```bash
# 1. Ensure the workflow and CSV are in the runtime folder
cp example/workflows/portfolioDividendAnalysis.jcwf workflows/
cp example/workflows/port62pos.csv workflows/

# 2. Ensure an AI provider key exists in AI Keys (e.g. "openai")

# 3. Start JarvisAgent
./bin/Release/jarvisAgent

# 4. Open the web UI → Workflows → Portfolio Dividend Analysis → Run
```

A full run takes approximately 25–30 seconds (60 parallel AI calls for
Stage 1, then 1 call for Stage 2).

---

## 11. Adapting This Workflow

### Different Portfolio

Replace `port62pos.csv` with any CSV that has `Symbol`, `Name`, and
`Percentage` columns. The workflow adapts to any number of rows
automatically — both the per-item fan-out and the glob-based summary
collection scale with the data.

### Different AI Provider

Change the `defaults.ai` section:

```jsonc
"defaults": {
  "ai": {
    "provider": "google",
    "model": "gemini-2.5-flash"
  }
}
```

### Adding a Third Stage

To extend the pipeline (e.g. generate a PDF report from the summary),
add a shell or Python task with `depends_on: ["portfolioSummary"]`:

```jsonc
"generateReport": {
  "type": "python",
  "depends_on": ["portfolioSummary"],
  "working_directory": "../workflows/portfolioDividendAnalysis/03_report",
  "file_inputs": [
    "../../queue/portfolioDividendAnalysis/02_portfolioSummary/PROB_summarize.output.txt"
  ],
  "file_outputs": ["portfolio_report.pdf"]
}
```

---
