# bookSummaryPipeline Workflow – Per‑Item Fan‑Out & AI Summarization

## Executive Summary

The **bookSummaryPipeline** workflow demonstrates the full **per_item fan‑out pipeline** in JarvisAgent: converting a PDF book to markdown, extracting chapter titles, fanning out one AI call per chapter, and combining all summaries into a single document.

At its core, this workflow shows:

- how `text_lines` filters drive per‑item task expansion,
- how shell, python, and ai_call tasks chain via dataflow,
- how `queue_binding` templates expand per‑item variables (`{{ch.text}}`, `{{ch.row_number_padded}}`),
- and how python tasks collect AI outputs via glob patterns.

---

## Pipeline Overview

```
in.pdf
  │
  ▼
┌──────────────────┐
│ convertToMarkdown │  shell: markitdown PDF → Markdown
│  (01_)            │
└────────┬─────────┘
         ▼
┌──────────────────┐
│ extractChapters   │  python: regex‑extract chapter titles → chapters.txt
│  (02_)            │
└────────┬─────────┘
         ▼
┌──────────────────┐
│ summarizeChapter  │  ai_call × N: one AI call per chapter (per_item)
│  (03_) [fan‑out]  │  filter: "chapters" (text_lines from chapters.txt)
└────────┬─────────┘
         ▼
┌──────────────────┐
│ combineSummaries  │  python: glob PROB_*.output.txt → book_summary.md
│  (04_)            │
└──────────────────┘
```

---

## Task Details

### 1. convertToMarkdown (shell)

Converts the input PDF to markdown using `markitdown`.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/runMarkitdown.sh` |
| Input | `../../in.pdf` (relative to working dir) |
| Output | `in.md` |
| Timeout | 30 s |

### 2. extractChapters (python)

Regex‑extracts chapter titles from the markdown and writes one title per line.

| Field | Value |
|-------|-------|
| Type | `python` |
| Module | `extractChapters` |
| Function | `run(**kwargs)` |
| Input | `../01_convertToMarkdown/in.md` (via `_file_input_0` in context) |
| Output | `chapters.txt` |
| Dataflow outputs | `outputPath`, `chapter_count`, `ai_output_glob` |

The `ai_output_glob` output constructs the glob pattern for the downstream combine task, pointing at `queue/bookSummaryPipeline/03_summarizeChapter/PROB_*.output.txt`.

### 3. summarizeChapter (ai_call, per_item)

Fans out one AI request per chapter title. Uses the `chapters` filter (type `text_lines`) which reads `chapters.txt` and expands each non‑empty line into a filter row.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `per_item` |
| Filter | `chapters` |
| Working dir | `../queue/bookSummaryPipeline/03_summarizeChapter` |

#### Queue Binding

| File | Content |
|------|---------|
| `STNG_style.txt` | Markdown formatting constraints, 300‑word limit |
| `CNTX_book.txt` | Book context (Vulkan graphics programming) |
| `TASK_summarize.txt` | Instruction: summarize topic, key concepts, chapter progression |
| `PROB_{{ch.row_number_padded}}.txt` | Per‑chapter prompt with `{{ch.text}}` |

Each expanded task instance (`summarizeChapter#0`, `#1`, …) writes its own `PROB_NNN.txt` and receives its corresponding `PROB_NNN.output.txt`.

### 4. combineSummaries (python)

Globs all AI output files and combines them into a single `book_summary.md`.

| Field | Value |
|-------|-------|
| Type | `python` |
| Module | `combineSummaries` |
| Function | `run(**kwargs)` |
| Dataflow input | `input_glob` ← `extractChapters.ai_output_glob` |
| Output | `book_summary.md` |

---

## Filter Configuration

```json
{
  "id": "chapters",
  "source": {
    "kind": "text_lines",
    "path": "bookSummaryPipeline/02_extractChapters/chapters.txt",
    "skip_empty": true
  },
  "binding": "ch",
  "max_items": 100
}
```

The `text_lines` filter reads the chapter list and exposes each line as:

- `{{ch.text}}` — the chapter title
- `{{ch.row_number_padded}}` — zero‑padded index (000, 001, …)
- `{{ch.row_number}}` — unpadded index

These variables are expanded in the `queue_binding` templates during per‑item materialization.

---

## Dataflow

```
extractChapters.ai_output_glob  →  combineSummaries.input_glob
```

This passes the glob pattern from the extraction step to the combine step, decoupling the two python tasks from hard‑coded paths.

---

## Running

```bash
# Manual start (no context required)
curl -s -X POST http://localhost:8080/api/workflows/bookSummaryPipeline/run

# Clean before re-run to ensure fresh session managers
curl -s -X DELETE http://localhost:8080/api/workflows/bookSummaryPipeline/clean
```

The workflow expects `in.pdf` to exist at `workflows/bookSummaryPipeline/../../in.pdf` (i.e. `workflows/in.pdf`).

---

## Key Concepts Demonstrated

- **text_lines filter** — line‑based fan‑out from a generated file
- **per_item expansion** — `FanOutPerItemChildren` creates N task instances from one task definition
- **queue_binding templates** — `{{ch.text}}` and `{{ch.row_number_padded}}` expanded per instance
- **python ↔ ai_call dataflow** — glob pattern passed between python tasks and consumed after fan‑out completes
- **Makefile semantics** — freshness checking enables fast re‑runs when outputs are up to date
