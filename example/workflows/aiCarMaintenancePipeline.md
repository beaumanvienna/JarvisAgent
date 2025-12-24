# aiCarMaintenancePipeline Workflow – AI Classify → Manual → AI Answer → Zip → Python

This document explains what the **AI Car Maintenance Pipeline** (`aiCarMaintenancePipeline.jcwf`) does, **which steps happen in which order**, and **which files are produced**.

---

## 1. What this workflow is for

`aiCarMaintenancePipeline` is a small end‑to‑end demo pipeline that:

1. Takes a **question** from `../workflows/message.txt` (a symlink you can swap to change test questions).
2. Uses an **AI task** to **classify** the question (`engine | tires | rephrase`).
3. Uses an **internal C++ task** to generate a matching **manual/context** (`manual.txt`).
4. Uses a second **AI task** to produce the final **answer** using:
   - `manual.txt` as **CNTX**
   - `message.txt` as **PROB**
5. Uses a **shell task** to zip the answer.
6. Uses a **python task** to print file info for the zip.

---

## 2. Runtime folders (where things go)

JarvisAgent is configured with:

- **Workflows folder:** `../workflows`
- **Queue folder:** `../queue`

You can see these in the terminal output (`[Engine] [info] queue folder: ../queue`, `[Engine] [info] workflows folder: ../workflows`) from the run logs.

This workflow writes artifacts into two main places:

### 2.1 Queue artifacts (AI tasks)
- `../queue/aiCarMaintenancePipeline/01_classifyQuestion/`
- `../queue/aiCarMaintenancePipeline/03_answerWithManual/`

These contain the **queue files** (STNG/CNTX/TASK/PROB and outputs).

### 2.2 Workflow artifacts (manual + zip)
- `../workflows/aiCarMaintenancePipeline/02_buildManual/` (manual.txt)
- `../workflows/aiCarMaintenancePipeline/04_zipAnswer/` (answer.zip)
- `../workflows/aiCarMaintenancePipeline/05_printZipInfo/` (python task working dir; typically no output files)

---

## 3. Task graph and dependencies

The workflow is a straight pipeline:

```
classifyQuestion (ai_call)
    ↓
buildManual (internal)
    ↓
answerWithManual (ai_call)
    ↓
zipAnswer (shell)
    ↓
printZipInfo (python)
```

### What “depends_on” means (JCWF semantics)

Per the JCWF spec, `depends_on` means:

- A task MUST NOT run until all its dependency tasks have completed successfully.
- Freshness checks are applied per task using its `file_inputs` and `file_outputs` (Makefile‑style):
  - Missing outputs → task is considered **not up‑to‑date**
  - Any input newer than any output → **not up‑to‑date**
  - Outputs newer than inputs → **up‑to‑date** and can be skipped

(These behaviors are explicitly described in the JCWF spec’s dependency + file freshness sections.)

---

## 4. Task-by-task breakdown (what runs, what it reads, what it writes)

### Task 1 — `classifyQuestion` (type: `ai_call`)
**Goal:** classify the question into exactly one word: `engine`, `tires`, or `rephrase`.

**Working directory:**
- `../queue/aiCarMaintenancePipeline/01_classifyQuestion`

**Inputs (freshness):**
- `../../../workflows/message.txt`  
  (the question source)

**Outputs (freshness):**
- `classification.output.txt`

**Queue artifacts created (observed in the run logs and the directory listing):**
- `STNG_classifyOneWord.txt`
- `CNTX_classifyRules.txt`
- `TASK_classifyTopic.txt`
- `PROB_1_<timestamp>.txt`
- `PROB_1_<timestamp>.output.txt` (raw AI output)
- `classification.output.txt` (the output file used by the workflow)

> From the run log, you can see JarvisAgent resolving and reading the PROB source:
> - `debug ai_call: PROB ... resolved='../workflows/message.txt'`
> - `debug ai_call: PROB read bytes=...`

---

### Task 2 — `buildManual` (type: `internal`)
**Goal:** generate a matching `manual.txt` based on the classification result.

**Working directory:**
- `../workflows/aiCarMaintenancePipeline/02_buildManual`

**Depends on:**
- `classifyQuestion`

**Inputs (freshness):**
- `../../../queue/aiCarMaintenancePipeline/01_classifyQuestion/classification.output.txt`

**Outputs (freshness):**
- `manual.txt`

**What it does:**
- Reads `classification.output.txt`
- Writes `manual.txt` with one of:
  - the engine manual
  - the tire maintenance manual
  - a “rephrase request” context

---

### Task 3 — `answerWithManual` (type: `ai_call`)
**Goal:** produce the final answer using:
- `manual.txt` as CNTX
- `message.txt` as PROB

**Working directory:**
- `../queue/aiCarMaintenancePipeline/03_answerWithManual`

**Depends on:**
- `buildManual`

**Inputs (freshness):**
- `../../../workflows/aiCarMaintenancePipeline/02_buildManual/manual.txt`
- `../../../workflows/message.txt`

**Outputs (freshness):**
- `answer.output.txt`

**Queue artifacts created (observed in the directory listing):**
- `STNG_answerStyle.txt`
- `CNTX_manual.txt` *(the copied/normalized context file in the queue dir)*
- `TASK_answerUserQuestion.txt`
- `PROB_2_<timestamp>.txt`
- `PROB_2_<timestamp>.output.txt`
- `answer.output.txt`

---

### Task 4 — `zipAnswer` (type: `shell`)
**Goal:** create a zip archive that contains the AI answer.

**Working directory:**
- `../workflows/aiCarMaintenancePipeline/04_zipAnswer`

**Depends on:**
- `answerWithManual`

**Inputs (freshness):**
- `../../../queue/aiCarMaintenancePipeline/03_answerWithManual/answer.output.txt`

**Outputs (freshness):**
- `answer.zip`

**Command:**
- `scripts/zipTool.sh`

**Arguments (from JCWF):**
- `${output[0]}` → `answer.zip`
- `${input[0]}` → `.../answer.output.txt`

So the effective call is conceptually:

```
scripts/zipTool.sh  answer.zip  <resolved-path-to-answer.output.txt>
```

---

### Task 5 — `printZipInfo` (type: `python`)
**Goal:** print file information (size + timestamp) for `answer.zip`.

**Working directory:**
- `../workflows/aiCarMaintenancePipeline/05_printZipInfo`

**Depends on:**
- `zipAnswer`

**Inputs (freshness):**
- `../04_zipAnswer/answer.zip`

**Python entrypoint:**
- module: `printFileInfo`
- function: `get_file_info`

**Dataflow wiring:**
The JCWF `dataflow` connects:
- `zipAnswer.archive_path` → `printZipInfo.filename`

So `printZipInfo` receives the zip file path as the `filename` argument.

> Note: `scripts/printFileInfo.py` is written so the interactive `input()` is only used under `if __name__ == "__main__":`.  
> The workflow calls `get_file_info()` directly.

---

## 5. Concrete artifact example from a run

From your run output, after the workflow finishes you typically have:

```
../queue/aiCarMaintenancePipeline/01_classifyQuestion/
  STNG_classifyOneWord.txt
  CNTX_classifyRules.txt
  TASK_classifyTopic.txt
  PROB_1_<timestamp>.txt
  PROB_1_<timestamp>.output.txt
  classification.output.txt

../workflows/aiCarMaintenancePipeline/02_buildManual/
  manual.txt

../queue/aiCarMaintenancePipeline/03_answerWithManual/
  STNG_answerStyle.txt
  CNTX_manual.txt
  TASK_answerUserQuestion.txt
  PROB_2_<timestamp>.txt
  PROB_2_<timestamp>.output.txt
  answer.output.txt

../workflows/aiCarMaintenancePipeline/04_zipAnswer/
  answer.zip

../workflows/aiCarMaintenancePipeline/05_printZipInfo/
  (usually no output files; prints to console)
```

---

## 6. What should happen on a second run?

With JCWF freshness semantics, a second run **should skip tasks** when:

- the task’s `file_outputs` exist **and**
- all outputs are newer than all inputs

That means (expected behavior):

- If `message.txt` did not change, **Task 1** should be up‑to‑date.
- If `classification.output.txt` did not change, **Task 2** should be up‑to‑date.
- If `manual.txt` did not change, **Task 3** should be up‑to‑date.
- If `answer.output.txt` did not change, **Task 4** should be up‑to‑date.
- If `answer.zip` did not change, **Task 5** should be up‑to‑date.

In other words: “everything up‑to‑date” should result in a fast run with tasks skipped, consistent with the spec’s Makefile‑like intent.

---

## 7. How to switch test questions

This pipeline intentionally uses a stable input path:

- `../workflows/message.txt`

You switch test cases by changing the symlink target, e.g.:

- `message_tire_question.txt`
- `message_engine_question.txt`
- `message_unclear_question.txt`

As soon as `message.txt` changes (timestamp or content via a new symlink target), Task 1 becomes not‑up‑to‑date and the pipeline reruns.

---

## 8. Key C++ components involved (conceptual map)

This workflow uses the same runtime “stack” as the other JCWF demos:

- **WorkflowRuntimeManager**: owns workflow runs and advances them over time
- **WorkflowOrchestrator**: schedules ready tasks based on dependencies
- **TaskFreshnessChecker**: Makefile‑style freshness decision using file timestamps
- **AiCallTaskExecutor**: materializes STNG/CNTX/TASK/PROB and dispatches AI calls
- **ShellTaskExecutor**: runs `scripts/zipTool.sh`
- **PythonTaskExecutor / PythonEngine**: calls `printFileInfo.get_file_info()`
- **FileWatcher**: observes new files in the queue (useful for debugging)

---
