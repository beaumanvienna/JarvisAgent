# Workflow Editor — Manual Test Plan

**Date:** 2026-02-24
**Purpose:** Verify that the workflow editor can create, save, validate, and run
complete JCWF workflows end-to-end. Three example workflows are tested, each
exercising different task types and features.

**Prerequisites:**
- JarvisAgent is running (`bin/Release/jarvisAgent` or `bin/Debug/jarvisAgent`)
- The workflow editor is accessible at `http://localhost:8080/workflow-editor/`
- At least one AI provider key is configured (OpenAI recommended)
- The file `workflows/port62pos.csv` exists (shipped with the repo)

**Security rule — shell tasks:**
Shell tasks **must** use a wrapper script located under `scripts/`. The
`params.command` field must start with `scripts/` — bare commands like `make`
are rejected by both the validator and the executor. Path traversal (`..`) in
the command path is also forbidden. All arguments are checked for shell
injection characters (`;`, `&`, `|`, `>`, `<`, quotes, backticks).

---

## Test 1: example-makefile — AI generates a Makefile, shell runs make

**Goal:** Create a 2-task linear pipeline: an `ai_call` task asks the AI to
generate a simple C "hello world" program with a Makefile, then a `shell`
task runs `make` to build it.

**Expected outcome:**
- Task 1 produces `PROB_generate.output.txt` containing a Makefile and hello.c
- Task 2 runs `make` using that output, producing a compiled `hello` binary
- Both task nodes turn green (Succeeded) in the editor canvas
- If `make` or `gcc` is not installed, task 2 will fail (expected on some systems)

### Pre-step: Create the wrapper script

Shell tasks require a script under `scripts/`. Create `scripts/runMake.sh`:

```bash
mkdir -p scripts
cat > scripts/runMake.sh << 'EOF'
#!/usr/bin/env bash
# runMake.sh — wrapper for 'make' (shell tasks must live under scripts/)
# Usage: scripts/runMake.sh <input_file> <output_file>
#
# 1. Extracts hello.c and Makefile from the AI-generated output
# 2. Runs make
set -euo pipefail
make
EOF
chmod +x scripts/runMake.sh
```

Run these commands from the JarvisAgent launch directory (where `bin/` lives).

### Steps

#### 1. Create the workflow

1. Open **http://localhost:8080/workflow-editor/**
2. Click the **Workflows** tab in the top navigation bar.
3. Click the **+ New** button (top right of the workflow list).
4. In the "Create New Workflow" modal, enter the id: `exampleMakefile`
5. Click **Create**. The editor opens with an empty canvas.

#### 2. Configure workflow metadata

In the **left sidebar**, scroll to the **Workflow** card:

| Field           | Value                        |
|-----------------|------------------------------|
| **label**       | `Example Makefile`           |
| **doc**         | `AI generates a C hello-world program with Makefile, then shell runs make.` |
| **base_directory** | `.`                       |
| **timeout_ms**  | `60000`                      |
| **ai.provider** | `openai`                     |
| **ai.model**    | `gpt-4.1-mini`               |

In the **Triggers** card:
- Make sure **manual_start** is checked (it should be by default).

#### 3. Add task nodes

In the **Nodes** card (top of the left sidebar):

1. Click **+ AI Call** → a new `ai_call` node appears on the canvas.
2. Click **+ Shell** → a new `shell` node appears on the canvas.

You should now see two nodes on the canvas.

#### 4. Configure task 1: generateCode (ai_call)

1. **Click the ai_call node** on the canvas to select it. The Inspector panel
   (right sidebar) shows its properties.
2. Fill in the fields:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Generate hello.c + Makefile` |
| **Type**             | `ai_call` (already set) |
| **Mode**             | `single` |
| **working_directory**| `../queue/exampleMakefile/01_generateCode` |
| **doc**              | `Ask AI to produce a minimal C hello-world with a Makefile.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

3. In the **Queue Binding** section (visible because type is ai_call):

   **STNG (Settings)** — click **+ entry**, then fill in:
   - **path:** `STNG_style.txt`
   - **content:**
     ```
     Be precise. Output only the requested file contents.
     Do not add explanations outside the file blocks.
     ```

   **TASK (Task)** — click **+ entry**, then fill in:
   - **path:** `TASK_generate.txt`
   - **content:**
     ```
     Generate two files:

     1. hello.c — a minimal C program that prints "Hello from JarvisAgent!"
     2. Makefile — a Makefile that compiles hello.c into an executable called "hello"

     Output each file in a clearly labeled code block:
       === hello.c ===
       <contents>
       === Makefile ===
       <contents>
     ```

   **PROB (Problem)** — click **+ entry**, then fill in:
   - **path:** `PROB_generate.txt`
   - **content:**
     ```
     Please generate the hello.c and Makefile as described in the task instructions.
     ```

   (Leave **CNTX** empty for this task.)

#### 5. Configure task 2: runMake (shell)

1. **Click the shell node** on the canvas.
2. Fill in:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Run make` |
| **Type**             | `shell` (already set) |
| **Mode**             | `single` |
| **working_directory**| `exampleMakefile/02_runMake` |
| **doc**              | `Runs make to compile the generated hello.c.` |
| **timeout_ms**       | `15000` |

3. **file_inputs** — click **+ file_input**, enter:
   `../../queue/exampleMakefile/01_generateCode/PROB_generate.output.txt`

4. **file_outputs** — click **+ file_output**, enter:
   `hello`

5. **params (JSON):**
   ```json
   {
     "command": "scripts/runMake.sh",
     "args": []
   }
   ```

> **Note:** The command **must** start with `scripts/` (security rule).
> This task will only succeed if `gcc` and `make` are installed on the system.
> The purpose of this test is to verify the editor can create and run the
> workflow — a shell-task failure due to missing tools is acceptable.

#### 6. Create the dependency edge

1. Hover over the **bottom handle** (small circle) of the `generateCode` node.
2. **Click and drag** from that handle to the **top handle** of the `runMake` node.
3. Release the mouse. An arrow should appear connecting them.

#### 7. Layout, validate, save, run

1. Click **Auto Layout** to arrange the nodes nicely.
2. Click **Validate** — check for errors in the sidebar. Fix any red messages.
3. Click **Save** — status should say "Saved."
4. Click **Run** — a new run appears in the sidebar's active runs section.
5. Watch the node colors: they go from gray (queued) → blue (running) → green
   (succeeded) or red (failed).

#### 8. Verify results

- Task 1 should turn green. Check that the file
  `queue/exampleMakefile/01_generateCode/PROB_generate.output.txt` exists
  and contains C code + a Makefile.
- Task 2 turns green if `make` succeeded, or red if tools are missing.
  Either result is valid for this test — the goal is to confirm the editor
  creates a runnable workflow.

---

## Test 2: stockAnalyzerTop6 — Analyze 6 positions from port62pos.csv

**Goal:** Create a per-item fan-out workflow that reads **only rows 5–10**
from `port62pos.csv` (JPM, T, SPG, BAC, D, JNJ), analyzes each stock's
dividend profile, and produces a summary report.

**Expected outcome:**
- The filter produces exactly 6 items (rows 5–10 of the CSV).
- Task 1 fans out into 6 parallel ai_call instances (one per stock).
- Task 2 aggregates the 6 reports into one summary.
- All task nodes turn green. Output files appear under `queue/stockAnalyzerTop6/`.

### Steps

#### 1. Create the workflow

1. **Workflows** tab → **+ New** → id: `stockAnalyzerTop6` → **Create**.

#### 2. Configure workflow metadata

| Field           | Value                        |
|-----------------|------------------------------|
| **label**       | `Stock Analyzer (Top 6)`     |
| **doc**         | `Analyzes 6 positions (rows 5-10) from port62pos.csv for dividend yield.` |
| **base_directory** | `.`                       |
| **timeout_ms**  | `60000`                      |
| **ai.provider** | `openai`                     |
| **ai.model**    | `gpt-4.1-mini`               |

**Triggers:** ensure **manual_start** is checked.

#### 3. Add a Filter node

1. In the **Nodes** card, click **+ Filter**. A purple filter node appears.
2. **Click the filter node** to select it. The Inspector shows filter details.
3. Click **Edit Filter…** to open the Filter Builder dialog.
4. Configure:

| Field          | Value          |
|----------------|----------------|
| **Filter ID**  | `positions`    |
| **Source kind** | `csv`         |
| **Path**       | `port62pos.csv` |
| **Delimiter**  | `,`            |
| **Has header** | checked        |
| **Range**      | `5-10`         |
| **Binding**    | `pos`          |
| **Max items**  | `100`          |

5. Click **Save** in the Filter Builder dialog.

#### 4. Add task nodes

1. Click **+ AI Call** → first ai_call node.
2. Click **+ AI Call** → second ai_call node.

#### 5. Configure task 1: analyzeStock (ai_call, per_item)

Click the first ai_call node, then fill in:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Analyze stock dividend` |
| **Mode**             | `per_item` |
| **Filter ID**        | `positions` (appears when mode = per_item) |
| **working_directory**| `../queue/stockAnalyzerTop6/01_analyzeStock` |
| **doc**              | `Per-item: look up dividend yield for one stock.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

**Queue Binding:**

**STNG** — add entry:
- **path:** `STNG_style.txt`
- **content:**
  ```
  Be succinct and precise. Report numbers clearly.
  Use a consistent structured format.
  Keep each position analysis under 150 words.
  ```

**TASK** — add entry:
- **path:** `TASK_dividendLookup.txt`
- **content:**
  ```
  For the stock position provided (PROB):

  1. Look up the current annual dividend yield (%)
  2. Look up the annual dividend per share ($)
  3. Given the portfolio allocation percentage, compute the
     weighted dividend contribution to total portfolio yield:
       weighted contribution = yield * allocation / 100
  4. State whether the dividend has been growing, stable,
     or declining over the past 5 years

  Format your response exactly as:
    Symbol: <TICKER>
    Name: <full name>
    Allocation: <X.XX%>
    Dividend Yield: <X.XX%>
    Annual Dividend/Share: $<X.XX>
    Weighted Contribution: <X.XXXX%>
    Dividend Trend (5yr): Growing | Stable | Declining
    Notes: <one-line note>
  ```

**CNTX** — add entry:
- **path:** `CNTX_portfolio.txt`
- **content:**
  ```
  This is a 60-position investment portfolio.
  You are analyzing one position at a time.
  The allocation percentage represents how much of the total
  portfolio value is invested in this position.
  ```

**PROB** — add entry:
- **path:** `PROB_{{pos.Symbol}}_{{pos.row_number_padded}}.txt`
- **content:**
  ```
  Symbol: {{pos.Symbol}}
  Name: {{pos.Name}}
  Portfolio Allocation: {{pos.Percentage}}
  ```

> **Important:** The `{{pos.Symbol}}` and `{{pos.row_number_padded}}` are
> template variables. They are expanded at runtime using the filter binding
> `pos` and the CSV column names. Type them exactly as shown.

#### 6. Configure task 2: portfolioSummary (ai_call, single)

Click the second ai_call node, then fill in:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Portfolio summary (6 stocks)` |
| **Mode**             | `single` |
| **working_directory**| `../queue/stockAnalyzerTop6/02_portfolioSummary` |
| **doc**              | `Aggregates all per-position reports into a summary.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

**Queue Binding:**

**STNG** — add entry:
- **path:** `STNG_style.txt`
- **content:**
  ```
  Be succinct and professional. Present a clear executive summary.
  Use tables where appropriate. Round percentages to two decimals.
  ```

**TASK** — add entry:
- **path:** `TASK_summary.txt`
- **content:**
  ```
  You are given dividend reports for 6 stock positions (CNTX files).

  Produce a summary with:
  1. Estimated combined dividend yield (sum of weighted contributions)
  2. Ranking of all 6 positions by weighted contribution (table)
  3. Dividend trend overview — how many Growing / Stable / Declining
  4. Key observations — 2-3 bullet points about the group
  ```

**PROB** — add entry:
- **path:** `PROB_summarize.txt`
- **content:**
  ```
  Analyze all 6 dividend reports provided as context files and produce
  a comprehensive summary as described in the TASK file.
  ```

**CNTX** — add entry (this is a **file glob** reference, not inline):
- **path:** `../01_analyzeStock/PROB_*.output.txt`
- **content:** *(leave empty — this is a path reference, not inline content)*

> **Note:** If the editor shows the CNTX entry as an inline entry with a
> path + content textarea, just leave the content textarea empty and enter
> the glob path. The backend resolves glob paths at runtime.

#### 7. Create edges

1. Draw an edge from the **filter node** → **analyzeStock** node.
2. Draw an edge from **analyzeStock** → **portfolioSummary** node.

#### 8. Layout, validate, save, run

1. **Auto Layout** → **Validate** → **Save** → **Run**.
2. Watch the canvas — the analyzeStock node should show fan-out activity
   (6 instances). After all 6 complete, portfolioSummary runs.

#### 9. Verify results

- Check `queue/stockAnalyzerTop6/01_analyzeStock/` — should contain 6
  `PROB_*.output.txt` files, one for each of: JPM, T, SPG, BAC, D, JNJ.
- Check `queue/stockAnalyzerTop6/02_portfolioSummary/PROB_summarize.output.txt`
  — should contain a summary table of the 6 stocks.
- **Both nodes green** in the editor.

---

## Test 3: techTermGlossary — 3-task AI chain

**Goal:** Create a 3-task linear chain of ai_call tasks. Task 1 generates a
list of 5 tech terms, task 2 expands each term into a beginner-friendly
explanation, task 3 combines everything into a formatted glossary document.

This workflow exercises:
- Linear `depends_on` chains (3 levels deep)
- Task output used as context input for the next task
- No external files needed — fully self-contained

**Expected outcome:**
- Task 1 produces a short list of 5 tech terms with one-line definitions.
- Task 2 produces expanded beginner-friendly paragraphs for each term.
- Task 3 produces a polished markdown glossary document.
- All 3 nodes turn green. Output files appear under `queue/techTermGlossary/`.

### Steps

#### 1. Create the workflow

1. **Workflows** tab → **+ New** → id: `techTermGlossary` → **Create**.

#### 2. Configure workflow metadata

| Field           | Value                        |
|-----------------|------------------------------|
| **label**       | `Tech Term Glossary`         |
| **doc**         | `3-task AI chain: generate terms, expand definitions, produce glossary.` |
| **base_directory** | `.`                       |
| **timeout_ms**  | `60000`                      |
| **ai.provider** | `openai`                     |
| **ai.model**    | `gpt-4.1-mini`               |

**Triggers:** ensure **manual_start** is checked.

#### 3. Add three AI Call nodes

1. Click **+ AI Call** three times. Three ai_call nodes appear on the canvas.

#### 4. Configure task 1: generateTerms

Click the first node:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Generate 5 tech terms` |
| **Mode**             | `single` |
| **working_directory**| `../queue/techTermGlossary/01_generateTerms` |
| **doc**              | `Asks AI to list 5 common tech terms with one-line definitions.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

**Queue Binding:**

**STNG** — add entry:
- **path:** `STNG_style.txt`
- **content:**
  ```
  Be concise. Use numbered lists. No filler text.
  ```

**TASK** — add entry:
- **path:** `TASK_generate.txt`
- **content:**
  ```
  Generate a list of exactly 5 important technology terms that a
  beginner software developer should know. For each term, provide
  a one-line definition (max 20 words).

  Format:
  1. <Term> — <definition>
  2. <Term> — <definition>
  ...
  ```

**PROB** — add entry:
- **path:** `PROB_terms.txt`
- **content:**
  ```
  Please generate the 5 tech terms as described.
  ```

#### 5. Configure task 2: expandDefinitions

Click the second node:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Expand definitions` |
| **Mode**             | `single` |
| **working_directory**| `../queue/techTermGlossary/02_expandDefinitions` |
| **doc**              | `Takes the 5 terms and expands each into a paragraph.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

**Queue Binding:**

**STNG** — add entry:
- **path:** `STNG_style.txt`
- **content:**
  ```
  Write for beginners. Use simple language. Avoid jargon.
  Each explanation should be 3-5 sentences.
  ```

**TASK** — add entry:
- **path:** `TASK_expand.txt`
- **content:**
  ```
  You are given a list of 5 tech terms (CNTX). For each term,
  write a beginner-friendly explanation paragraph (3-5 sentences).
  Include a real-world analogy if helpful.

  Format:
  ## <Term>
  <paragraph>
  ```

**CNTX** — add entry:
- **path:** `../01_generateTerms/PROB_terms.output.txt`
- **content:** *(leave empty — this is a file path reference)*

**PROB** — add entry:
- **path:** `PROB_expand.txt`
- **content:**
  ```
  Expand the 5 tech terms from the context into beginner-friendly paragraphs.
  ```

#### 6. Configure task 3: formatGlossary

Click the third node:

| Field                | Value |
|----------------------|-------|
| **Label**            | `Format glossary` |
| **Mode**             | `single` |
| **working_directory**| `../queue/techTermGlossary/03_formatGlossary` |
| **doc**              | `Combines expanded definitions into a polished markdown glossary.` |
| **params (JSON)**    | `{ "mode": "one_shot" }` |

**Queue Binding:**

**STNG** — add entry:
- **path:** `STNG_style.txt`
- **content:**
  ```
  Produce clean, well-formatted markdown. Include a title, introduction,
  and table of contents. Professional tone.
  ```

**TASK** — add entry:
- **path:** `TASK_glossary.txt`
- **content:**
  ```
  You are given expanded definitions for 5 tech terms (CNTX).
  Produce a polished markdown glossary document with:

  1. A title: "Beginner's Tech Glossary"
  2. A short introduction paragraph (2-3 sentences)
  3. A table of contents (linked list of terms)
  4. Each term as a section with the expanded definition
  5. A "Further Reading" section with 2-3 suggested resources
  ```

**CNTX** — add entry:
- **path:** `../02_expandDefinitions/PROB_expand.output.txt`
- **content:** *(leave empty — file path reference)*

**PROB** — add entry:
- **path:** `PROB_glossary.txt`
- **content:**
  ```
  Format the expanded definitions into a polished glossary document.
  ```

#### 7. Create dependency edges

1. Draw an edge from **generateTerms** → **expandDefinitions**.
2. Draw an edge from **expandDefinitions** → **formatGlossary**.

The canvas should show a clean left-to-right chain of 3 nodes.

#### 8. Layout, validate, save, run

1. **Auto Layout** → **Validate** → fix any errors → **Save** → **Run**.
2. Watch the nodes light up in sequence: generateTerms (blue → green),
   then expandDefinitions (blue → green), then formatGlossary (blue → green).

#### 9. Verify results

| File | Expected content |
|------|-----------------|
| `queue/techTermGlossary/01_generateTerms/PROB_terms.output.txt` | A numbered list of 5 tech terms with one-line definitions |
| `queue/techTermGlossary/02_expandDefinitions/PROB_expand.output.txt` | 5 sections, each with a term heading and a 3-5 sentence explanation |
| `queue/techTermGlossary/03_formatGlossary/PROB_glossary.output.txt` | A complete markdown glossary with title, TOC, definitions, and further reading |

- **All 3 nodes green** in the editor.
- The final glossary document should be a standalone, readable markdown file.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| "Save" shows an error | Workflow id contains invalid characters | Use only `a-z`, `A-Z`, `0-9`, and no spaces |
| Validate shows "working_directory missing" | Inspector field left empty | Fill in the working_directory for the flagged task |
| Task stays blue (running) for > 2 min | AI provider timeout or missing API key | Check AI Keys tab; verify the provider key is set |
| Task turns red immediately | Queue binding incomplete (missing STNG/TASK/PROB) | Open Inspector, check queue binding has all required entries |
| Filter node shows 0 items at runtime | CSV path or range incorrect | Verify `port62pos.csv` exists and range is `5-10` |
| Edge won't connect | Dragging from wrong handle direction | Drag from bottom handle of source to top handle of target |
| "Run" button is disabled | manual_start unchecked or no workflow loaded | Check Triggers card; ensure manual_start is checked |

---

## Summary Checklist

- [ ] **Test 1 (exampleMakefile):** Created, saved, validated, ran. Task 1 green. Task 2 green or red (depends on gcc).
- [ ] **Test 2 (stockAnalyzerTop6):** Created with filter (range 5-10), saved, validated, ran. 6 fan-out instances. Both tasks green.
- [ ] **Test 3 (techTermGlossary):** Created 3-task chain, saved, validated, ran. All 3 tasks green. Final glossary is readable markdown.
