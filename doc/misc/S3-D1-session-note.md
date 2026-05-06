# S3 — D1 (Workflow orchestration) Hardening Session Note

**Domain:** D1 — `application/workflow/`, `application/python/`, `application/session/`, `application/file/`, `application/task/`, `application/content/`, `application/json/`.

**Source plans:** `doc/misc/cybersec-hardening-dev-plan.md` §6 and `doc/misc/cpp-safety-hardening-dev-plan.md` §6.

**Source audits (regenerated 2026-05-06 at session start, run IDs `jarvisCppCyberSecAudit_1778030037` + `jarvisCppSafetyAudit_1778030037`):**
- `workflows/jarvisCppCyberSecAudit/141_combineDocumentation/combinedCyberSecAudit.md` (311 KB; down from 842 KB in the pre-S1 baseline at `doc/combinedCyberSecAudit.md`)
- `workflows/jarvisCppSafetyAudit/141_combineDocumentation/combinedSafetyAudit.md` (689 KB; down from 1.37 MB in the pre-S1 baseline at `doc/combinedSafetyAudit.md`)

The pre-S1 baselines in `doc/` have not yet been refreshed from these run outputs — that's a separate publish step, deferred until the S3 sittings consume the findings.

## D1 finding density — opening picture

Combined HIGH-and-above counts across the regenerated audits (cyber-sec + safety), workflow-orchestration files only:

| File | Cyber HIGH | Safety HIGH | Total |
|---|---|---|---|
| `application/python/pythonEngine.h` | 3 | 3 | **6** |
| `application/workflow/workflowRuntimeManager.h` | 2 | 3 | 5 |
| `application/workflow/shellTaskExecutor.h` | 2 | 2 | 4 |
| `application/workflow/aiCallTaskExecutor.h` | 1 | 2 | 3 |
| `application/json/schemaValidator.h` | 0 | 3 | 3 |
| `application/workflow/triggerEngine.h` | 0 | 2 | 2 |
| `application/workflow/aiRequestPool.h` | 0 | 2 | 2 |
| `application/json/replyParserAPI1.h` | 0 | 2 | 2 |
| `application/workflow/jcwfContainer.h` | 1 | 0 | 1 (zip-slip) |
| `application/workflow/pythonTaskExecutor.h` | 1 | 0 | 1 |
| `application/session/fileWriter.h` | 1 | 0 | 1 |

Plus one CRITICAL outside D1 — `application/cloud/dbQueryCloudTaskExecutor.h` SQL-injection (sole remaining CRITICAL in the entire fresh cyber-sec audit). That's D2 territory; back-folded into S3 only if the cloud-executor surface needs follow-up, otherwise tracked separately.

**Densest D1 file:** `pythonEngine.h` (6 HIGHs across both axes).

**Total D1 surface (line counts):** ~14 600 lines across 27 .h/.cpp pairs; biggest is `workflowRuntimeManager.cpp` at 3 499 lines.

Plan estimate (per `cybersec-hardening-dev-plan.md` §3): **3–4 sittings**. Density "very high" per the safety plan's §6 — the most concurrency-heavy code in the project lives here. Likely runs hot.

---

## Sitting 1 — `application/python/pythonEngine.{h,cpp}` comprehensive

**Scope:** Python sandboxing (cyber-sec) + lifetime / concurrency (safety).  All 6 HIGHs from the regenerated audits closed in this sitting; 3 MEDIUMs bundled in.  Boundary held at `pythonEngine.{h,cpp}` whole-file plus the minimal call-site plumbing (`pythonEnginePool.{h,cpp}`, `pythonTaskExecutor.cpp`, `jarvisAgent.cpp`).

### [HIGH cyber] Untrusted module / function name into `PyImport_ImportModule` — pythonEngine.cpp:~601

**Finding:** `module_name` and `function_name` are read from `taskDefinition.m_ParamsJson` and passed unchecked to `PyImport_ImportModule` + `PyObject_GetAttrString`.  A workflow author (or AI assistant generating a JCWF) could set `{"module": "os", "function": "system"}` and execute arbitrary commands under j9t's UID.  The script registry already enumerates the legitimate modules (`scripts/<name>.py` files with `@jarvis-script` headers) but was not on the import path.

**Verification:** Holds up.  Pre-fix code calls `PyImport_ImportModule(moduleName.c_str())` directly after only an emptiness check on `moduleName`.

**Change:** New `ScriptRegistry const* m_ScriptRegistry` member on `PythonEngine`, wired through `PythonEnginePool::Initialize(scriptPath, engineCount, scriptRegistry)` (registry pointer required, `Initialize` returns false if null) and propagated to each engine via `engine->SetScriptRegistry(scriptRegistry)` before `SetupSubInterpreter`.  In `ExecuteWorkflowTaskOnWorker`, gate the module name through `m_ScriptRegistry->FindByModulePath(moduleName)` before any Python import.  Reject with an explicit fail message that names the module and explains why.

**Ramifications:**
- Callers touched: `pythonEnginePool.h` + `.cpp` (Initialize signature), `jarvisAgent.cpp` (passes `m_ScriptRegistry.get()` into `Initialize`), `pythonTaskExecutor.cpp` (also touched for run-id plumbing — see below).
- `ScriptRegistry::FindByModulePath` already accepts both flat names (`"parseLog"`) and dotted package paths (`"scripts.parseLog"`), so no change to the registry surface.
- Tests touched: smoke test of `cyber2.jcwf` (module `scripts.parseSshLog`) — succeeded post-rebuild, confirming the positive path.  Negative path (rejected module) is code-review verified; the rejection branch returns the documented error string before any Python state is mutated.
- Docs: deferred to sitting close-out doc sweep.

### [HIGH cyber] `m_ScriptDir` blindly appended to `sys.path` — pythonEngine.cpp:~85

**Finding:** `SetupSubInterpreter(scriptDir, ...)` stored `scriptDir` directly into `m_ScriptDir` and added it to `sys.path` without any path-traversal check.  Currently safe because the launcher hardcodes `"scripts/main.py"` in `jarvisAgent.cpp`, but defense-in-depth + future-call-site safety required.

**Verification:** Holds up.  Pre-fix code: `m_ScriptDir = scriptDir; ... PyList_Append(sysPathList, PyUnicode_FromString(m_ScriptDir.c_str()));`

**Change:** New anonymous-namespace helper `ConfineUnderProjectRoot(path)` that resolves a string against the project root via `fs::weakly_canonical`, validates containment via `lexically_relative` (rejecting `""`, `".."`, and any `"../"` prefix), and returns an empty path on rejection (fail-closed).  `SetupSubInterpreter` calls it on `scriptDir` before storing the canonical form into `m_ScriptDir`.  On rejection logs `LOG_APP_ERROR("PythonEngine[{}]: rejected scriptDir '{}' — does not resolve under project root", ...)` and returns false.  The parent-path appended to `sys.path` (for dotted imports like `scripts.foo`) inherits the canonical base — no separate gate needed.

**Ramifications:**
- Callers touched: none externally.  `m_ScriptDir` consumers all read the post-validation canonical string.
- Tests touched: same `cyber2.jcwf` smoke — engine started cleanly, scripts directory accepted.  Confirmed by absence of "rejected scriptDir" line in `log/log.txt` after restart.
- Docs: none.

### [HIGH cyber] `taskWorkingDirectory` inserted into Python context dict without confinement — pythonEngine.cpp:~540 / 769

**Finding:** `request->m_TaskWorkingDirectory` is plumbed straight into the Python context dict under key `_task_working_directory`.  Workflow tasks routinely use it as a base for file I/O.  An attacker who can influence the task working directory (e.g. via a malicious workflow's `working_directory` field) could pass an `..`-escaping path that user Python code subsequently uses for reads or writes.

**Verification:** Holds up.  Pre-fix code passes `taskWorkingDirectory.c_str()` to `PyUnicode_FromString` and then `PyDict_SetItemString(contextDict, "_task_working_directory", ...)` with no validation.

**Change:** Reuse the `ConfineUnderProjectRoot` helper from HIGH cyber #2.  In `ExecuteWorkflowTaskOnWorker`, compute `taskWorkingDirectoryConfined` once after the module-allowlist check; reject the task with `fail(...)` if confinement fails; pass the confined path to the context dict (replacing the raw value).  All other consumers of `taskWorkingDirectory` inside this method continue to use the raw value (logging-only, no security impact).

**Ramifications:**
- Callers touched: none — `_task_working_directory` is read by Python user code that already treats the value as a base path.  Canonical form is what they want anyway.
- Tests touched: same `cyber2.jcwf` smoke — `parse_ssh_log` task working directory `workflows/cyber2/OpenSSH_2k/01_parse` resolves under project root and produced its `attack_stats.json` correctly.
- Docs: none.

### [HIGH safety] `m_InterpreterState` raw-pointer lifetime invariant undocumented — pythonEngine.cpp:~138

**Finding:** `m_InterpreterState` is a raw `PyInterpreterState*` set via `SetInterpreterState()` and used in `WorkerLoop` for `PyThreadState_New`.  No documented invariant that the owner outlives the worker thread; the audit flagged it as a UAF risk.

**Verification:** Read `pythonEnginePool.cpp:Initialize()`.  The pointer is `subTS->interp` from `Py_NewInterpreterFromConfig` — sub-interpreters are torn down only by `Py_Finalize` at process exit (the pool intentionally leaks the main-thread state per its own comment).  So the lifetime is process-scoped and the worker thread is always covered.  Audit concern is valid as a code-review concern, not a real UAF.

**Change:** Class-level documentation block on `PythonEngine` enumerating both `m_InterpreterState` and `m_ScriptRegistry` lifetime invariants.  `assert(m_InterpreterState != nullptr && "...")` immediately before `PyThreadState_New` so a misordered call site (e.g. forgetting `SetInterpreterState` before `StartWorkerThread`) trips a clear assertion in Debug instead of producing a silent null pointer dereference inside CPython.

**Ramifications:**
- Callers touched: none.  Existing call sites already do `SetInterpreterState` before `StartWorkerThread`.
- Tests touched: covered by every Python task run (`cyber2`, `bookSummaryPipeline`, prior `jarvisCppCyberSecAudit` / `jarvisCppSafetyAudit`).
- Docs: class comment in `pythonEngine.h`.

### [HIGH safety] Cross-thread access to `m_Running` / `m_StopRequested` / `m_TasksCompleted` without synchronization — pythonEngine.h

**Finding:** `m_Running` (read in public `IsRunning`, `OnStart`, `OnUpdate`, `OnEvent`, `SignalStop`, `ExecuteWorkflowTask` from main thread; written in `StartWorkerThread` / `WaitStop` from main thread) is non-atomic and unprotected.  `m_StopRequested` is read+written under `m_QueueMutex` on every site so already synchronized, but the contract is not visible.  `m_TasksCompleted` is written by the worker thread and read by `GetTasksCompleted` from any thread that owns the engine pointer.

**Verification:** Holds up.  Pre-fix `bool m_Running{false};` and `++m_TasksCompleted;` in WorkerLoop are textbook data races on TSan.

**Change:** Promote all three to `std::atomic<bool>` / `std::atomic<size_t>` with explicit memory ordering.  `m_Running` reads use `acquire`, writes use `release` — the natural happens-before edge between the StartWorkerThread store and the worker's first observation.  `m_StopRequested` keeps mutex protection on every existing site (zero behaviour change), but the atomic gives readers an option to drop the lock if a future call site needs it.  `m_TasksCompleted` uses `relaxed` order — the counter has no synchronization role, just monitoring.

**Ramifications:**
- Callers touched: none externally.  The change is semantically a no-op against current single-mutator-per-flag patterns; it removes the data-race UB.
- Tests touched: `cyber2` + `bookSummaryPipeline` exercise the queue path; no regression.
- Docs: in-class comments on each member explaining the synchronization contract.

### [HIGH safety] `WorkflowTaskRequest::m_InputValues` / `m_ContextValues` raw `const*` to caller-owned maps — pythonEngine.h

**Finding:** Pre-fix struct stored `unordered_map<...> const*` pointing at the caller's stack-frame variables (`pythonTaskExecutor.cpp:151-181` constructs them as locals).  The worker thread reads these after the request is enqueued; current behaviour is safe only because `ExecuteWorkflowTask` blocks on `resultFuture.get()`.  Switch to fire-and-forget would produce immediate UAF.  Per memory `feedback_capture_by_value_async`: capture by value into async work sites; no exceptions for "fast paths".

**Verification:** Holds up.  Pre-fix code: `request->m_InputValues = &inputValues;` followed by `*request->m_InputValues` deref on the worker thread.

**Change:** `m_InputValues` and `m_ContextValues` are now value-owned `unordered_map<std::string, std::string>` on the request.  `ExecuteWorkflowTask` copies the caller's maps into the request at enqueue time.  `ExecuteWorkflowTaskOnWorker` reads the owned copies via reference.  Cost: one map copy per task; not on a hot path.

**Ramifications:**
- Callers touched: `ExecuteWorkflowTask` signature gains `workflowId` + `runId` parameters (also needed for fail-path logging — see MEDIUM bundle below).  `pythonEnginePool::ExecuteWorkflowTask` and `pythonTaskExecutor.cpp` updated accordingly.
- Tests touched: `cyber2` + `bookSummaryPipeline` succeeded post-rebuild — Python sees identical kwargs and context.
- Docs: struct comment explaining the ownership contract.

### [MEDIUM bundle] Fail-path logging + drain race + Python-exception sanitization

**(a) Fail-path logging completeness.**  Pre-fix `fail()` lambda in `ExecuteWorkflowTaskOnWorker` set `m_ErrorMessage` but emitted no `LOG_APP_ERROR`.  Public-API early returns in `ExecuteWorkflowTask` (`!m_Running`, missing params) also returned silently.  Per CLAUDE.md fail-path discipline: every fail emits ERROR with `runId` / `workflowId` / `taskId` literals so the dashboard's Run Analyzer can attribute the failure.

Change: `fail()` now LOG_APP_ERRORs after setting `m_ErrorMessage`; both early-return cases in `ExecuteWorkflowTask` also log.  Required threading `workflowId` and `runId` through to the request; `pythonTaskExecutor.cpp` reads them from `WorkflowRun` and passes them in.

**(b) Double-set-promise race in shutdown drain.**  Pre-fix `WorkerLoop::drain` block at `~line 339` and the main-path `request->m_Promise.set_value(false)` in `fail()` could both fire on the same request if shutdown raced with task completion.  Double `set_value` on `std::promise` throws `std::future_error: broken_promise`, propagating UB up the worker thread.

Change: `WorkflowTaskRequest::m_PromiseSatisfied` (atomic bool, default false).  Both `fail()` and the new `succeed()` lambda CAS-acquire it before `set_value`; the drain block does the same.  First writer wins; subsequent attempts no-op cleanly.

**(c) Python exception messages forwarded verbatim into log + error message.**  `consumePythonException()` returned the raw `pyObjectToUtf8(value)` string — Python tracebacks may contain non-UTF-8 bytes (mojibake from misconfigured locales) and run unbounded length.

Change: New helper `SanitizePythonErrorMessage(raw)` = `TruncateUtf8Safe(SanitizeUtf8(raw), 4096)`.  Applied at the `consumePythonException()` return.  Downstream consumers (dashboard JSON, ncurses TUI, log/log.txt) all see well-formed UTF-8 ≤ 4 KB.  Pattern parallels the S1=D2 §19 SanitizeUtf8 boundary work.

**Ramifications:**
- Callers touched: ExecuteWorkflowTask signature gain (workflowId, runId) — propagated through pool + executor.
- Tests touched: `cyber2` + `bookSummaryPipeline` post-rebuild — zero new error lines, no regressions.
- Docs: in-line comments on the helpers + the atomic guard.

### Skipped findings (sitting 1)

| Finding | Severity | Reason |
|---|---|---|
| "No Python sandboxing" (no seccomp / cgroup / PyPy-sandbox / network restriction) | MEDIUM | Out of scope per CLAUDE.md threat model.  JCWFs are authored under operator approval; defense-in-depth via OS-level isolation is a platform decision, not a hardening fix to be made inside `PythonEngine`. |
| `PyObject*` ref-count discipline audit | LOW | Existing code is correct (verified during the read); LOWs cluster into a tail sweep at session close. |
| `PyRun_SimpleString` exception-safety RAII wrapper | LOW | Tail sweep. |
| `PythonTask::Type` switch missing `default:` | LOW | Memory `feedback_cpp_discipline` argues *against* `default:` over closed enums we own; the absent default is correct.  `static_assert(NumVariants == N)` is the right shape — bundle into the workflowTypes safety pass. |
| Style / column-limit / east-const | style | Out of scope. |

### Sitting 1 wrap

**What landed:** 6 HIGH (3 cyber-sec + 3 safety) closed; 3 MEDIUMs bundled.  Files modified:
- `application/python/pythonEngine.{h,cpp}` — primary surface.
- `application/python/pythonEnginePool.{h,cpp}` — Initialize signature, registry plumbing.
- `application/workflow/pythonTaskExecutor.cpp` — pass workflowId + runId.
- `application/jarvisAgent.cpp` — pass `m_ScriptRegistry.get()` into `Initialize`.

**What's verified:**

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| j9t fresh start with new binary | OK; keystore unlocked, 18 providers loaded |
| `ai-zip-demo` (4 ai_call tasks, 1 shell task) | succeeded in 19 s — adjacent path unbroken |
| `bookSummaryPipeline` (16 ai_call + 2 python tasks; python tasks freshness-skipped) | succeeded in 16 s — adjacent path unbroken |
| `cyber2` after `DELETE /api/workflows/cyber2/clean` | succeeded in 17 s — `parse_ssh_log` (Python task, module `scripts.parseSshLog`) executed end-to-end and produced `attack_stats.json` with valid `ip_profiles` data |
| `[error]` lines in `log/log.txt` post-tests | 0 |

What's not directly verified:
- The negative path of the module-allowlist gate (rejecting an unregistered module) — verified by code review only.  Negative-path test would require either editing a JCWF in-flight or constructing a malicious one; deferred to a future hardening test.
- The double-set-promise guard under actual shutdown-during-task race — verified by code review only.  Triggering this requires precise interleaving; the CAS pattern is well-understood.

**Open boundary at sitting end:** `pythonEnginePool.{h,cpp}` (1 MEDIUM finding from the audit — bundled into sitting 2 if trivial), then sitting 2 picks up `workflowRuntimeManager.{h,cpp}` Cluster A (DAG state + concurrency + lambda-by-ref sweep) per the proposed schedule.

---

## Sittings 2-3 — `application/workflow/workflowRuntimeManager.{h,cpp}` Cluster A + B

Split into two adjacent sittings.  Cluster A = concurrency / lambda captures / DAG state.  Cluster B = path traversal + SSRF + per-item resource cap.  Detailed retrospectives in `doc/misc/hand-off.md` (2026-05-05 entry).

**Closed in sitting 2 (Cluster A):** safety HIGH 1 (`m_WorkflowRegistry` raw-ptr races — class-level lifetime contract documented), safety HIGH 2 (`m_ActiveRuns`/`m_LastRuns`/`m_SubWorkflowLinks` cross-thread access — `Update()` now holds `m_Mutex` for the entire tick body, external calls deferred to a `postTickActions` vector and fired after lock release), safety HIGH 3 + cyber LOW 3 (lambda-by-ref capture in `TickActiveRun`'s `pool.SubmitTask` → captured by value), safety MEDIUM 2 (`m_RunTerminalObserver` race — copied under lock before invocation), safety LOW 1 (destructor `noexcept` + try/catch), safety LOW old-style enum (`WorkflowRunStateToString` + `TaskInstanceStateKind` switch in `FireCompletionCallback` got `static_assert` + no `default:` arm).

**Closed in sitting 3 (Cluster B):** cyber HIGH 1 (path traversal in `CleanWorkflow` — new `deleteIfConfined` lambda gates all 5 `fs::remove*` sites through `ConfineUnderProjectRoot`), cyber HIGH 2 + LOW 1 (SSRF + TLS hardening in `FireCompletionCallback` — new `IsCallbackUrlAllowed` enforces https-only + DNS-resolved internal-IP rejection; TLS verify + no-redirect + protocol-allowlist), cyber MEDIUM 3 (per-item fan-out cap via new `EngineConfig::m_MaxPerItemFanOut`, default 10000), cyber LOW 4 (symlink safety — free byproduct of `weakly_canonical` in `ConfineUnderProjectRoot`).  New shared helper `application/file/pathConfinement.{h,cpp}` lifted from `pythonEngine.cpp`'s anonymous namespace per `feedback_cpp_discipline` — `pythonEngine.cpp` switched to use it.

**Verified:** Studio Debug + Release clean; standard 3-workflow suite (ai-zip-demo, bookSummaryPipeline, cyber2) passes; cancel-cascade verified flips run to `cancelled` cleanly with in-flight task allowed to finish; SSRF gate verified by running bookSummaryPipeline with `callbackUrl=https://127.0.0.1:9999/test` (refused with `resolves to internal IPv4 127.0.0.1`); two and three concurrent workflows succeed (also stresses sitting 2's lock-scope expansion); 28/28 assistant tests; graceful REST shutdown.

**Open boundary at sitting 3 end:** `pythonEnginePool.h` HIGH (m_Engines/m_Running mutex) — the audit shows it's a HIGH not the "1 MEDIUM" the prior hand-off claimed; tail-deferred to sitting 4.

---

## Sitting 4 — `application/python/pythonEnginePool.{h,cpp}` synchronization + cleanup bundle

**Closed:** safety HIGH (data races on `m_Engines` + `m_Running` — atomic `m_Running` with explicit acquire/release ordering; `mutable std::mutex m_Mutex` guards `m_Engines` mutation; class-level threading contract documented post-`Initialize` true / pre-`SignalStop` false → `m_Engines` stable + lock-free readable), safety MEDIUM (`SetInterpreterState` ordering — moved AFTER `setupOk` check), safety MEDIUM (`GetTasksCompleted` OOB log), cyber MEDIUM (scriptPath confinement at the pool boundary via `ConfineUnderProjectRoot` — defense in depth on top of the per-engine gate), several LOWs (catch(...), `[[nodiscard]]` on `Initialize`/`ExecuteWorkflowTask`, `=delete` on copy/move ctor + assignment).

**Verified:** Studio Debug + Release clean; **three concurrent workflows** ran simultaneously (cyber2 + ai-zip-demo + bookSummaryPipeline) with overlapping start/end windows — stresses pool synchronization + sitting 2's lock-scope expansion at once.  Pool init clean (4 engines).  28/28 assistant tests.

**Lockout incident worth flagging:** the dashboard's WS auto-reconnect generates failed-auth attempts that hit the per-IP lockout (10 failures / 5 min → 15 min ban).  Once the count crosses the threshold, all REST + MCP + assistant-WS from the same IP are rejected — including `/api/shutdown`.  JC manually killed the process to break the cycle.  Captured under "Open items" for a future small sitting.

---

## Sitting 5 — Cluster B leftovers cleanup

Bundle of four small post-sitting-3 cleanups.

**Closed:** cyber LOW 2 (input validation for `runId`/`workflowId` at the runtime layer — new `IsValidRunOrWorkflowId(id)` allowlist `[A-Za-z0-9._-]{1,256}`, no `..`, no leading dot; applied at every public-API entrypoint as defense in depth on top of REST validation), cyber MEDIUM 2 (glob hardening — new `GlobMatchesFilename` iterative two-pointer fnmatch-style matcher with backtracking, supports `*` and `?` anywhere), safety LOW 3 (TOCTOU — `deleteIfConfined` silent-skips on non-existent paths via `fs::remove*` error_code; bare `fs::exists` pre-checks dropped), cyber MEDIUM 1 (callback opt-out — new `callback_include_outputs` context flag, default `true` for backwards compat).

**Verified:** Studio Debug + Release clean; bogus workflowId rejected by REST validation (`..%2F..%2Fetc%2Fpasswd` → `invalid_workflow_id`); 3 concurrent workflows succeed; SSRF gate fires for the loopback callback test; clean cyber2 ran twice in the same session — second pass exercised the new TOCTOU silent-skip path on the queue directory cleanly; 28/28 assistant tests.

---

## D1 status snapshot at end of session

**Sittings done (5):** pythonEngine, workflowRuntimeManager Cluster A, workflowRuntimeManager Cluster B, pythonEnginePool, leftovers.

**HIGH-and-above findings closed in D1 so far:**

| File | Cyber HIGH | Safety HIGH | Status |
|---|---|---|---|
| `application/python/pythonEngine.h` | 3 | 3 | ✓ closed (sitting 1) |
| `application/python/pythonEnginePool.h` | 0 | 1 | ✓ closed (sitting 4) |
| `application/workflow/workflowRuntimeManager.h` | 2 | 3 | ✓ closed (sittings 2-3 + leftovers) |

Plus 1 cyber CRITICAL outside D1 (`application/cloud/dbQueryCloudTaskExecutor.h` SQL injection) still open.

**Remaining D1 files with HIGH-and-above findings (audit re-scan at session end):**

| File | Cyber HIGH | Cyber MED | Safety HIGH | Safety MED | Notes |
|---|---|---|---|---|---|
| `application/workflow/triggerEngine.h` | 0 | 0 | **2** | **6** | Webhooks + S3/OneDrive/email polling — biggest external trigger surface |
| `application/workflow/aiRequestPool.h` | 0 | **3** | **2** | **4** | Major HTTP dispatcher; concurrency-heavy (cancel-cascade lifetime, request-handle generations, deferred-completion queue) |
| `application/workflow/adhocWorkflowManager.h` | 0 | 0 | **2** | **4** | Adhoc-submission gate; per-tenant disk quota; reaper |
| `application/workflow/workflowRegistry.h` | 0 | 1 | **2** | 2 | JCWF loading + reload, file-watch driven |
| `application/workflow/shellTaskExecutor.h` | 0 | 0 | **2** | **5** | Already extensively reworked per `feedback_argv_only_shell`; safety axis still dense |
| `application/workflow/aiCallTaskExecutor.h` | **1** | ? | **2** | ? | Dispatch-side of the AI pipeline |
| `application/workflow/aiTranscript.h` | 0 | 1 | **1** | 3 | Transcript JSON serialisation |
| `application/workflow/polarionClient.h` | **1** | 3 | 0 | 4 | External SOAP/REST client; XML parsing |
| `application/workflow/jcwfContainer.h` | **1** | 0 | 0 | 0 | Zip-slip in extract |
| `application/workflow/workflowJsonParser.h` | **1** | 2 | 0 | 0 | JCWF JSON parsing — attacker-controlled input |
| `application/workflow/workflowFileIndex.h` | **1** | 0 | 0 | 2 | Freshness manifests |
| `application/workflow/workflowTriggerBinder.h` | **1** | 1 | 0 | 4 | Bridge between parsed triggers and TriggerEngine |
| `application/workflow/subWorkflowTaskExecutor.h` | **1** | 1 | 0 | 0 | Already touched (sitting 2's `RegisterSubWorkflowLink` lock fix) |
| `application/workflow/taskExecutorRegistry.h` | **1** | 1 | 0 | 0 | Type→executor dispatch |
| `application/workflow/taskPathResolver.h` | **1** | 1 | 0 | 0 | Working-directory + freshness-path resolution |
| `application/workflow/pythonTaskExecutor.h` | **1** | 1 | 0 | 4 | Already partially touched (sitting 1's owned-by-value `WorkflowTaskRequest` maps) |
| `application/session/fileWriter.h` | **1** | 0 | ? | ? | Disk-write boundary; in D1 per the domain definition |
| `application/json/schemaValidator.h` | 0 | 0 | **3** | ? | Output-schema validation; in D1 per the domain definition |
| `application/json/replyParserAPI1.h` | 0 | 0 | **2** | ? | OpenAI reply parser |

---

## Proposed remaining sittings

Ordered by **attacker reach** (external surface first), then **density**, then **adjacency** (related files bundled).

### Sitting 6 — `application/workflow/triggerEngine.{h,cpp}`

**Theme:** External-input cluster.  Webhooks (HMAC-signed external HTTP), file-watch triggers, S3 / OneDrive / email polling, cron.  Highest-attacker-reach D1 file.

**Scope estimate:** likely splits A/B.  Cluster A = webhook side (HMAC verification, payload parsing, per-trigger rate limit).  Cluster B = polling side (S3 / OneDrive / email scheduling, retry + dedup, cancellation on shutdown).

**Audit:** safety 2H + 6M; cyber 0H + 0M (the safety axis is what hurts here — the cyber surface is mostly hardened in `webServer.cpp` upstream of TriggerEngine).

**Likely depth:** medium-high.  The file is concurrency-heavy (`m_Mutex` already exists; multiple worker threads feed into trigger evaluation); we likely converge it onto the same lock-scope-expansion + capture-by-value pattern from sitting 2.

### Sitting 7 — `application/workflow/aiRequestPool.{h,cpp}`

**Theme:** Major HTTP dispatcher with the densest concurrency surface remaining.  Cancel-cascade lifetime, request-handle generations, deferred-completion queue, batched submission.

**Audit:** safety 2H + 4M; cyber 0H + 3M.

**Likely depth:** high.  The pool already received heavy attention in S2=D3 (curl wrapper hardening) but the runtime-side ownership (`m_PendingRequests`, `m_DeferredCompletions`, in-flight handle book-keeping) is still on the table.  This is the natural counterpart to sitting 2's `WorkflowRuntimeManager::Update()` lock-scope work — both are tick-driven mutators of shared state read by REST handlers.

### Sitting 8 — `adhocWorkflowManager.{h,cpp}` + `workflowRegistry.{h,cpp}` (bundled)

**Theme:** JCWF lifecycle — adhoc submission + the registry that serves it.  Both are Safety-heavy (2H+4M and 2H+2M).  Adjacent — adhoc submission writes into the registry, the registry's reload path is file-watch driven and races with adhoc submission.

**Likely depth:** medium.  Bundle is reasonable because the surfaces are tightly coupled and the audit findings overlap (race between adhoc-submission write and registry-reload read; per-tenant cleanup + reaper concurrency).

### Sitting 9 — Cloud-adjacent boundary cluster: `polarionClient.{h,cpp}` + `dbQueryCloudTaskExecutor.{h,cpp}`

**Theme:** External-data clients.  PolarionClient (cyber 1H + 3M; safety 0H + 4M) handles external SOAP/REST + XML.  `dbQueryCloudTaskExecutor` is the **lone CRITICAL in the entire fresh cyber-sec audit** (SQL injection); D2 territory but workflow-runtime-adjacent — folding here closes the only CRITICAL in one shot.

**Likely depth:** medium-high.  The CRITICAL is small in surface (one query-builder method) but symbolically important.  PolarionClient is the bigger work item (XML parsing is always nasty).

### Sitting 10 — `application/workflow/shellTaskExecutor.{h,cpp}`

**Theme:** Dense safety surface (2H + 5M) on a file already extensively reworked for security per `feedback_argv_only_shell`.  Cyber axis empty (the argv-only refactor removed the shell-injection attack class), so this is purely about thread-safety / lifetime / RAII discipline around the spawned process + watchdog.

**Likely depth:** medium.

### Sitting 11 — Task-executor + small-file bundle

Aggregate of small files with single HIGHs:
- `subWorkflowTaskExecutor.h` (1H+1M cyber; partially touched in sitting 2)
- `taskExecutorRegistry.h` (1H+1M cyber)
- `taskPathResolver.h` (1H+1M cyber)
- `pythonTaskExecutor.h` (1H+1M cyber, 0H+4M safety; partially touched in sitting 1)
- `workflowFileIndex.h` (1H+0M cyber, 0H+2M safety)
- `workflowTriggerBinder.h` (1H+1M cyber, 0H+4M safety)
- `aiCallTaskExecutor.h` (cyber 1H + safety 2H — folded here if depth is shallow; otherwise promote to its own sitting)

**Likely depth:** medium.  Each file is small (sub-300 lines typically) but the count adds up.  Pattern recognition from sittings 1-4 should make these go faster.

### Sitting 12 — Parser + schema cluster

- `application/workflow/workflowJsonParser.h` (cyber 1H+2M)
- `application/workflow/aiTranscript.h` (cyber 0H+1M, safety 1H+3M)
- `application/workflow/filter/filterManifest.h` (cyber 0H+0M, safety 0H+3M)
- `application/file/scriptCatalog.h` (cyber 0H+0M, safety 0H+3M)
- `application/json/schemaValidator.h` (safety 3H — D1 per the domain definition)
- `application/json/replyParserAPI1.h` (safety 2H — D1 per the domain definition)

**Theme:** All touch JSON parsing surfaces with attacker-controlled or AI-generated input.  Bundle natural — same coding patterns recur (simdjson ondemand iterator discipline, error-shape consistency, fail-path logging).

**Likely depth:** medium.

### Sitting 13 — Horizontal sweeps + close

- **`App::g_App` / `Core::g_Core` null-deref defense** across D1 (safety MEDIUM 5).  `feedback_horizontal_sweeps` pattern: 1 fix × N files.  Small per-site, big aggregate.
- **Logger fail-path context** across D1 surfaces still missing runId/workflowId in early returns (safety MEDIUM 1).  Same horizontal-sweep shape.
- **Dashboard WS reconnect / lockout interaction** (defensive UX issue captured under sitting-4 verification).  Either (a) exclude WS-upgrade-without-credentials from the failure counter, (b) make the dashboard back off exponentially on `locked_out`, or both.
- **D1 close-out:** publish refreshed `combinedCyberSecAudit.md` + `combinedSafetyAudit.md` to `doc/`; verify the post-D1 audit shows zero remaining HIGHs in workflow orchestration; update `cybersec-hardening-dev-plan.md` + `cpp-safety-hardening-dev-plan.md` checkmarks.

---

## Sitting count update

**Original estimate:** 3-4 (plan); 6-9 likely (historical multiplier from S1=D2: 5-6→34, suggesting D1 lands at 8-12).

**Actual sittings done so far:** 5 (sittings 1-5).

**Remaining proposed:** 8 (sittings 6-13).

**Total projected:** 13 sittings.  At the upper bound of the historical multiplier — consistent with the audit's HIGH-and-above density (workflowRuntimeManager alone took 2 + 1 leftovers = 3).

The plan stays bundleable.  If a sitting comes in under-time, the next one's first task can be folded; if it comes in over-time, the file split into A/B is the natural escape hatch.  Sittings 11-12 are the most readily collapsible (small files; pattern recognition from sittings 1-4 should compress them).

