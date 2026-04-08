# inputResolutionTest Workflow – Context Resolution & Input Defaults

This document explains what the **Input Resolution Test** (`inputResolutionTest.jcwf`) does, **which resolution paths it exercises**, and **how to run it**.

---

## 1. What this workflow is for

`inputResolutionTest` is a minimal test harness that verifies the runtime's **3-step input resolution chain** (see JC Workflow Specification §8.1):

1. **Dataflow edges** — explicit `from_task.from_output → to_task.to_input` wiring.
2. **Run context** — lookup by input name in the shared `ContextMap` (seeded via REST or auto-published by upstream tasks).
3. **Input default** — the `"default"` field on the input declaration.

It also verifies that task outputs are **auto-published to the run context** as `taskId.outputName` keys (§8.2).

---

## 2. Task graph and dependencies

```
echoDefault      (shell, no deps)     ← tests input default fallback
echoContext      (shell, no deps)     ← tests REST-seeded context
produceValue     (shell, no deps)     ← produces output → auto-publishes to context
    ↓
consumeContext   (shell, depends_on: produceValue) ← reads auto-published context
```

`echoDefault`, `echoContext`, and `produceValue` have no dependencies and run in parallel. `consumeContext` waits for `produceValue` to complete.

---

## 3. Task-by-task breakdown

### Task 1 — `echoDefault` (type: `shell`)
**Resolution path tested:** Input default (step 3).

**Input:** `greeting` — declared with `"required": true, "default": "Hello World"`. No dataflow edge or context provides this value, so the default is used.

**Command:**
```
scripts/echoInput.sh greeting "Hello World"
```

**Expected log output:**
```
[shell:echoDefault] [echoInput.sh] greeting = Hello World
```

---

### Task 2 — `echoContext` (type: `shell`)
**Resolution path tested:** Run context lookup (step 2).

**Input:** `user_name` — declared with `"required": true`, no default. Must be resolved from the run context seeded via the REST API body.

**Command:**
```
scripts/echoInput.sh user_name <value-from-REST-body>
```

**Expected log output (when run with `{"context": {"user_name": "JarvisTester"}}`):**
```
[shell:echoContext] [echoInput.sh] user_name = JarvisTester
```

**Note:** If no context is provided, this task will **fail** with: `required input 'user_name' for task 'echoContext' could not be resolved`.

---

### Task 3 — `produceValue` (type: `shell`)
**Purpose:** Produces an output to test auto-publication to context.

**Output:** `result` — mapped to `file_outputs[0]` (`result.txt`). After successful completion, the runtime publishes `produceValue.result` → `<absolute path to result.txt>` into the run context.

**Command:**
```
scripts/echoInput.sh producing ContextPropagationWorks
```

---

### Task 4 — `consumeContext` (type: `shell`)
**Resolution path tested:** Auto-published context (step 2 via upstream output).

**Input:** `produceValue.result` — declared with `"required": true`, no default, no dataflow edge. Resolved from the context key `produceValue.result` that was auto-published when task 3 succeeded.

**Depends on:** `produceValue`

**Command:**
```
scripts/echoInput.sh upstream_result <absolute-path-to-result.txt>
```

**Expected log output:**
```
[shell:consumeContext] [echoInput.sh] upstream_result = /path/to/workflows/inputResolutionTest/03_produceValue/result.txt
```

---

## 4. How to run

**Prerequisites:**
- JarvisAgent running with the new binary (context resolution support).
- `scripts/echoInput.sh` present and executable.

**Steps:**

```bash
# 1. Copy the JCWF to the workflows folder
cp example/workflows/inputResolutionTest.jcwf workflows/

# 2. Reload workflow registry
curl -s -X POST http://localhost:8080/api/workflows/reload

# 3. Run with REST-seeded context
curl -s -X POST http://localhost:8080/api/workflows/inputResolutionTest/run \
  -H "Content-Type: application/json" \
  -d '{"context": {"user_name": "JarvisTester"}}'

# Or with TLS enabled (default port 8443):
curl -sk -X POST https://localhost:8443/api/workflows/inputResolutionTest/run \
  -H 'Content-Type: application/json' \
  -d '{"context": {"user_name": "JarvisTester"}}' | jq .

# 4. Check the log for resolution results
grep "echoInput.sh" log/log.txt
```

**Expected log output (all 4 tasks succeed):**
```
[shell:echoDefault]    [echoInput.sh] greeting = Hello World
[shell:echoContext]    [echoInput.sh] user_name = JarvisTester
[shell:produceValue]   [echoInput.sh] producing = ContextPropagationWorks
[shell:consumeContext] [echoInput.sh] upstream_result = /.../inputResolutionTest/03_produceValue/result.txt
```

---

## 5. Runtime folders

**Workflow artifacts:**
```
workflows/inputResolutionTest/01_echoDefault/     (empty — echoInput.sh only prints)
workflows/inputResolutionTest/02_echoContext/      (empty)
workflows/inputResolutionTest/03_produceValue/     (result.txt — output file)
workflows/inputResolutionTest/04_consumeContext/   (empty)
```

---

## 6. What this tests in the codebase

| Component | What is exercised |
|-----------|-------------------|
| `DataflowResolver::ResolveInputsForTask` | Full 3-step resolution chain |
| `TaskIOField.m_Default` | Parsed from JCWF `"default"` field, used as step 3 fallback |
| `WorkflowRuntimeManager` (harvest) | Auto-publishes `m_OutputValues` to `m_Context` as `taskId.outputName` |
| `WebServer::HandleWorkflowRunPost` | Parses `{"context": {...}}` from request body |
| `EnqueueWorkflowRunWithContextAndGetRunId` | Seeds `ContextMap` at run creation |
| `TemplateEngine` | `{{inputs.slot_name}}` expansion in `params.args` |

---
