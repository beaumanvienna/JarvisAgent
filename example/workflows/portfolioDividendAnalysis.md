# Portfolio Dividend Analysis — CSV-Driven Per-Item AI Workflow

## Executive Summary

The **portfolioDividendAnalysis** workflow demonstrates JarvisAgent's
**per-item fan-out** capability driven by a local CSV file, paired with a
**deterministic verification stage** that does the arithmetic a language model
shouldn't be trusted with. It reads a 60-position investment portfolio,
dispatches one AI call per position to look up dividend data as structured
JSON, aggregates the results into a narrative summary, then **computes the
allocation-weighted portfolio yield as an exact sum in Python** and prepends
that figure to the report — removing the model's summation drift, while making
the limits of that exactness explicit.

This is a three-stage pipeline that showcases:

- **`csv` filter source** — reads rows from a CSV file with header-based
  field binding.
- **`per_item` mode with `{{binding.field}}` template substitution** — each
  CSV row is injected into PROB file paths and inline content.
- **Structured output (`output_schema`)** — each per-item reply is validated
  JSON (`{symbol, dividend_yield_pct, …}`), so a downstream script can parse it
  reliably instead of scraping prose.
- **`cntx_files` glob patterns** — the summary task dynamically collects all
  per-item outputs using `PROB_*.output.json` instead of hardcoding 60
  references.
- **Reasoning + deterministic compute (with an honest caveat)** — the AI looks
  up yields (reasoning); a Python task computes the *exact* allocation-weighted
  sum of those yields (arithmetic), removing the model's summation drift. The
  catch this example makes explicit: an exact sum is only as accurate as its
  inputs — feed it AI-estimated yields and you get an exact total *of
  estimates*, not ground truth.

---

## 1. Big Picture

```
                                                  +-----------------------+
                                    per item      |  lookupDividend       |
+----------------+  60 positions  ------------->  |  ai_call (per_item)   |
|  positions     |                                |  60 AI calls → JSON   |
|  csv filter    | ------+                        +-----------+-----------+
|  port62pos     |       |                                    | depends_on
+-------+--------+       |   glob collect                     v
        |               +--- PROB_*.output.json --> +-----------------------+
        |  authoritative                            |  portfolioSummary     |
        |  allocations                              |  ai_call (single)     |
        |                                           +-----------+-----------+
        |                                                       | depends_on
        |                                                       v
        |                                           +-----------------------+
        +-----------------------------------------> |  verifyTotals         |
              port62pos.csv  +  per-item JSON        |  python (deterministic)|
                                                     |  → portfolio_report.md |
                                                     +-----------------------+
```

### What happens when you click Run

1. **Filter evaluation** — `FilterEngine` reads `port62pos.csv` (60 data rows
   + 1 header), producing 60 `FilterItem` objects with fields `Symbol`, `Name`,
   and `Percentage`.

2. **Fan-out** — The runtime creates 60 child task instances
   (`lookupDividend#0` through `lookupDividend#59`). Each child receives
   the CSV fields as `{{pos.*}}` template variables.

3. **Template substitution** — For each child, `{{pos.Symbol}}` and
   `{{pos.row_number_padded}}` are replaced in both the PROB filename
   (e.g. `PROB_NVDA_037.txt`) and its content.

4. **AI dispatch (structured)** — 60 async AI requests run in parallel. Each
   asks the model for the position's dividend yield, per-share dividend, and
   5-year trend, validated against `output_schema`. A validated reply lands at
   `PROB_<SYM>_<NN>.output.json`.

5. **Aggregation** — Once all 60 children succeed, `portfolioSummary` becomes
   ready. Its glob `../01_lookupDividend/PROB_*.output.json` collects all 60
   JSON replies as context, and a single AI call writes the narrative summary
   (including its own *estimate* of the total yield).

6. **Deterministic verification** — `verifyTotals` (Python) joins the
   AI-supplied yields with the *authoritative* allocations in `port62pos.csv`,
   recomputes the allocation-weighted yield as exact arithmetic, and prepends a
   **"Verified Portfolio Totals (computed deterministically)"** block to the AI
   summary, writing the combined `portfolio_report.md`.

---

## 2. Prerequisites

### AI Provider Key

An AI provider key must exist in the JarvisAgent KeyManager (unless you point
the workflow at a keyless local interface). The workflow's `defaults.ai`
specifies a provider/model; change it to whatever you have configured.

### CSV Data File

`port62pos.csv` must be present in the workflow folder alongside the canvas
(it ships inside the `.jcwf` container and is extracted on load).

### Python

The `verifyTotals` stage runs `scripts/portfolio_totals.py` via the embedded
Python engine — no third-party packages, only the standard library.

---

## 3. Workflow File Structure

```
example/workflows/
  portfolioDividendAnalysis.jcwf    ← workflow container (global.json + canvas + CSV)
  portfolioDividendAnalysis.md      ← this document
scripts/
  portfolio_totals.py               ← the deterministic verifier (shared script)
```

The `.jcwf` is a zip container holding `global.json`, the canvas JSON, and
`port62pos.csv`; it is extracted into the runtime `workflows/` folder on load.

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
`{{pos.Name}}`, and `{{pos.Percentage}}`. The allocations in this file are the
**authoritative** source of truth for the weighting — the AI is never asked to
compute weighted contributions.

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

- **`kind: "csv"`** — reads a local CSV file via `FilterEngine::EvaluateCsv()`.
- **`has_header: true`** — first row is treated as column names, not data.
- **`binding: "pos"`** — all values are injected with the `pos.` prefix.
- **`max_items: 100`** — safety cap (the CSV has 60 rows).

### 5.2 Available Template Variables

| Variable | Source | Example Value |
|----------|--------|---------------|
| `{{pos.Symbol}}` | CSV column "Symbol" | `NVDA` |
| `{{pos.Name}}` | CSV column "Name" | `NVIDIA Corporation` |
| `{{pos.Percentage}}` | CSV column "Percentage" | `3.75%` |
| `{{pos.row_number_padded}}` | 1-based row, zero-padded | `006` |
| `{{pos.row_number}}` | 1-based row number | `6` |
| `{{pos.index}}` | 0-based row index | `5` |

### 5.3 Stage 1 — `lookupDividend` (per_item, structured output)

```jsonc
"lookupDividend": {
  "type": "ai_call",
  "mode": "per_item",
  "filter": "positions",
  "working_directory": "../../queue/portfolioDividendAnalysis/01_lookupDividend",
  "output_schema": {
    "type": "object",
    "properties": {
      "symbol": { "type": "string" },
      "name": { "type": "string" },
      "dividend_yield_pct": { "type": "number" },
      "annual_dividend_per_share": { "type": "number" },
      "dividend_trend": { "type": "string", "enum": ["Growing","Stable","Declining","Unknown"] },
      "notes": { "type": "string" }
    },
    "required": ["symbol", "dividend_yield_pct"],
    "additionalProperties": false
  },
  "output_retries": 5,
  "queue_binding": {
    "prob_files": [{
      "path": "PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt",
      "content": "Symbol: {{pos.Symbol}}\nName: {{pos.Name}}\nPortfolio Allocation: {{pos.Percentage}}\n"
    }]
    /* + stng_files / task_files / cntx_files (inline guidance) */
  }
}
```

- **`mode: "per_item"`** + **`filter: "positions"`** — fan-out of 60 children.
- **`prob_files`** — template-substituted per child; `{{pos.row_number_padded}}`
  guarantees a unique path per row (a static per-item PROB path would collide
  all 60 rows onto one file).
- **`output_schema` + `output_retries`** — the reply is validated JSON; on a
  schema mismatch the runtime re-asks (up to 5 times). A validated reply lands
  at `PROB_<SYM>_<NN>.output.json`.
- The AI supplies the *yield* only; weighted contributions are computed
  downstream from the authoritative allocations.

### 5.4 Stage 2 — `portfolioSummary` (single, with glob)

```jsonc
"portfolioSummary": {
  "type": "ai_call",
  "mode": "single",
  "depends_on": ["lookupDividend"],
  "working_directory": "../../queue/portfolioDividendAnalysis/02_portfolioSummary",
  "queue_binding": {
    "prob_files": [{ "path": "PROB_summarize.txt", "content": "..." }],
    "cntx_files": ["../01_lookupDividend/PROB_*.output.json"]
    /* + stng_files / task_files */
  }
}
```

- **`depends_on: ["lookupDividend"]`** — waits for all 60 children.
- **`cntx_files` glob** — `PROB_*.output.json` dynamically collects all per-item
  replies; the runtime expands it at execution time, sorts matches, and
  materializes each as a `CNTX_*` file. Adapts automatically to any row count.
- The AI produces the narrative summary and its own *estimated* total yield —
  which the next stage replaces with the exact figure.

### 5.5 Stage 3 — `verifyTotals` (python, deterministic)

```jsonc
"verifyTotals": {
  "type": "python",
  "depends_on": ["portfolioSummary"],
  "working_directory": "../../queue/portfolioDividendAnalysis/03_verifyTotals",
  "params": { "module": "portfolio_totals", "function": "stamp" },
  "file_inputs": ["../02_portfolioSummary/PROB_summarize.output.txt"],
  "file_outputs": ["portfolio_report.md"]
}
```

- **`params.module`/`function`** — runs `scripts/portfolio_totals.py::stamp`.
- **`file_inputs`** — the AI summary (reached in the script as
  `context['_file_input_0']`); the script also globs the sibling
  `01_lookupDividend/PROB_*.output.json` replies and reads `port62pos.csv` from
  `context['_workflow_base_directory']`.
- **What it computes** — for every position it joins the *authoritative*
  allocation (CSV) with the *AI-supplied* yield (JSON): `allocation × yield ÷
  100`, summed across all matched positions. That exact total, plus the top
  contributors and payer counts, is prepended to the AI summary as an
  exact-totals block — `portfolio_report.md`.

This is the point of the example: **the AI reasons (looks up yields); Python
does the exact aggregation (the weighted sum), removing summation drift.** But
note the honest twist observed in a real run — the AI's *holistic* estimate
(~3.85%) and the exact sum of its own per-position lookups (~4.23%) disagreed.
The arithmetic is flawless; the per-position yields it summed simply ran a
little high, and neither number is guaranteed to be ground truth. The lesson
isn't "Python beats the model at math" — it's **verify your inputs, not just
your arithmetic.** A deterministic stage like this becomes authoritative only
when fed authoritative data (a real dividend export) rather than model lookups.

---

## 6. Runtime Artifacts

After a successful run:

```
queue/portfolioDividendAnalysis/01_lookupDividend/
  STNG_succinct.txt / TASK_dividendLookup.txt / CNTX_portfolio.txt  ← shared
  PROB_LQD_001.txt          ← per-item input (position details)
  PROB_LQD_001.output.json  ← per-item structured reply (validated JSON)
  ...                       ← 60 input/output pairs total

queue/portfolioDividendAnalysis/02_portfolioSummary/
  PROB_summarize.txt
  PROB_summarize.output.txt ← AI narrative summary (free text)
  CNTX_PROB_LQD_001.txt     ← materialized from the .json glob (60 files)
  ...

queue/portfolioDividendAnalysis/03_verifyTotals/
  portfolio_report.md       ← verified-totals header + the AI summary (final output)
```

---

## 7. Sample Output

### Per-Position Reply (`PROB_AAPL_037.output.json`)

```json
{
  "symbol": "AAPL",
  "name": "Apple Inc.",
  "dividend_yield_pct": 0.55,
  "annual_dividend_per_share": 0.94,
  "dividend_trend": "Growing",
  "notes": "Dividend has steadily increased over the past five years."
}
```

### Final Report (`portfolio_report.md`)

```markdown
# Portfolio Totals — exact weighted sum of the AI's per-position yields

- **Allocation-weighted dividend yield: 4.23%** (exact sum of 60 weighted
  contributions; allocations total 100.0%)
- Simple average yield across positions: 3.59%
- Positions matched: 60  |  dividend payers: 58  |  zero-dividend: 2 (DIS, WBD)
- Top contributors: LQD (0.4024%), T (0.2639%), NLY (0.2525%), ...

> This is the **exact** allocation-weighted sum of the per-position yields the
> AI looked up — it removes summation drift, but its accuracy is bounded by
> those yields. For an authoritative figure, feed authoritative dividend data.

---
[the AI executive summary follows — in a real run its holistic estimate (~3.85%)
and this exact sum of the AI's per-position lookups (4.23%) disagreed: the
arithmetic is exact, the per-position inputs ran a little high. Verify the
inputs, not just the math.]
```

---

## 8. Key Features Demonstrated

### 8.1 CSV Filter Source

Reads a local file, parses header and rows, and produces one `FilterItem` per
data row (`FilterEngine::EvaluateCsv`), with `max_items` and range filtering.

### 8.2 Glob Patterns in `cntx_files`

The summary task uses `"../01_lookupDividend/PROB_*.output.json"` to discover
all per-item outputs at execution time (`ExpandCntxFileGlobs`): the parent dir
is resolved relative to the task working directory, matches are collected and
sorted, and each is materialized as `CNTX_<basename>`. No hardcoded paths; it
scales to any number of positions.

### 8.3 Reasoning + Deterministic Compute (the headline)

A common, powerful pattern for AI workflows that must produce trustworthy
numbers:

1. **Reasoning (AI):** per-item tasks look up qualitative/uncertain facts (the
   yields) in parallel, returned as structured JSON.
2. **Aggregation (script):** a Python task computes the exact weighted sum,
   immune to the model's summation drift.

`depends_on` sequences the three stages; structured output makes the hand-off
between AI and script reliable.

**The honest caveat this example surfaces:** a deterministic stage removes
*summation* error, not *input* error. In a real run, the exact sum of the AI's
per-position yields (4.23%) disagreed with the AI's own holistic estimate
(3.85%) — exact arithmetic over per-position lookups need not match the model's
gestalt, and neither is guaranteed accurate, because the per-position lookups
carry their own error. Exact arithmetic over approximate inputs is
precisely-wrong. The stage becomes genuinely authoritative only when its inputs
are authoritative (e.g. real dividend figures from a broker/database export
feeding the same arithmetic). Determinism guarantees *consistency and
auditability*, not *accuracy* — verify the inputs too.

---

## 9. Key C++ Components

| Component | Role in This Workflow |
|-----------|----------------------|
| `FilterEngine::EvaluateCsv` | Parses CSV with header, delimiter, range support |
| `WorkflowRuntimeManager` | Per-item fan-out + aggregation of child results |
| `AiCallTaskExecutor` | Template substitution, queue file materialization |
| `SchemaValidator` | Validates each per-item reply against `output_schema` |
| `ExpandCntxFileGlobs` | Glob expansion for dynamic `cntx_files` collection |
| `PythonEngine` | Runs the deterministic `verifyTotals` stage |

---

## 10. How to Run

1. Start a Studio instance (`./jarvisagent.sh`).
2. Ensure an AI provider is configured (or point `defaults.ai` at a keyless
   local interface).
3. Open the editor/dashboard → Workflows → **Portfolio Dividend Analysis** →
   Run. (Or via MCP/REST: run workflow `portfolioDividendAnalysis`.)

A full run takes roughly 30–60 seconds: 60 parallel structured AI calls
(Stage 1), one summary call (Stage 2), then the instant Python verifier
(Stage 3).

---

## 11. Adapting This Workflow

### Different Portfolio

Replace `port62pos.csv` with any CSV that has `Symbol`, `Name`, and
`Percentage` columns. Both the per-item fan-out and the glob-based summary
collection scale with the row count automatically; the verifier joins on
`Symbol`.

### Different AI Provider

Change the `defaults.ai` section to any configured interface name and model.

### Further Stages

The deterministic `verifyTotals` stage is itself the template for extending the
pipeline — e.g. add a shell task with `depends_on: ["verifyTotals"]` to render
`portfolio_report.md` to PDF.
