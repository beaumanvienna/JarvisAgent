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
- **Log-injection sanitization** on attacker-influenced log args (curl error messages, response body fragments, JSON field values reaching `LOG_APP_*`).  Apply `SanitizeUtf8` + `TruncateUtf8Safe(N)` at the format-arg boundary so control chars / multi-MB strings can't enter the log/dashboard pipeline.  Cyber MEDIUMs from sittings 7+9 (audit findings on aiRequestPool + polarionClient + connectors) all share this shape.
- **`[[nodiscard]]` sweep** on bool-returning public APIs in the D1 surface (registry, runtime, request pool, adhoc manager, executors).  Mechanical attribute-add; catches accidental result-discarding at compile time.  Deferrals from sittings 7+8.
- **Dashboard WS reconnect / lockout interaction** (defensive UX issue captured under sitting-4 verification).  Either (a) exclude WS-upgrade-without-credentials from the failure counter, (b) make the dashboard back off exponentially on `locked_out`, or both.
- **D1 close-out:** publish refreshed `combinedCyberSecAudit.md` + `combinedSafetyAudit.md` to `doc/`; verify the post-D1 audit shows zero remaining HIGHs (and the lone CRITICAL closed in sitting 9) in workflow orchestration; update `cybersec-hardening-dev-plan.md` + `cpp-safety-hardening-dev-plan.md` checkmarks.

---

## Sitting count update

**Original estimate:** 3-4 (plan); 6-9 likely (historical multiplier from S1=D2: 5-6→34, suggesting D1 lands at 8-12).

**Actual sittings done so far:** 5 (sittings 1-5).

**Remaining proposed:** 8 (sittings 6-13).

**Total projected:** 13 sittings.  At the upper bound of the historical multiplier — consistent with the audit's HIGH-and-above density (workflowRuntimeManager alone took 2 + 1 leftovers = 3).

The plan stays bundleable.  If a sitting comes in under-time, the next one's first task can be folded; if it comes in over-time, the file split into A/B is the natural escape hatch.  Sittings 11-12 are the most readily collapsible (small files; pattern recognition from sittings 1-4 should compress them).

---

## Sitting 6 — `application/workflow/triggerEngine.{h,cpp}`

**Theme:** External-input cluster.  Webhooks, file-watch, S3 / OneDrive / email / Azure Blob / GCS polling, cron, manual.  Highest-attacker-reach D1 file.

**Boundary held at:** `triggerEngine.{h,cpp}` whole-file.  No call-site plumbing required — all the changes are internal to the trigger engine; existing public API signatures unchanged.

### Closed in this sitting

**Safety HIGH 1 — `eventsToFire` / email-poll index identity drift across the inter-lock window in `Tick()`.**  Audit framing was misleading (says "two threads run Tick concurrently" — but `Tick()` is single-threaded, called only from the JarvisAgent main loop at `jarvisAgent.cpp:586`).  Real concern: between releasing `m_Mutex` after collecting `emailPollJobs` (~line 717) and re-acquiring it for the watermark update (~line 764), other threads can call `ClearWorkflowTriggers` / `AddEmailWatchTrigger` and shift `m_EmailWatchTriggers`.  The pre-fix bounds check (`if (job.m_Index < m_EmailWatchTriggers.size())`) caught OOB but **silently allowed identity drift** — the watermark could land on an unrelated trigger.

Fix: `EmailPollJob` now carries `(workflowId, triggerId)` instead of an index.  Post-network re-acquire uses a new `findEmailTriggerIndexLocked(job)` helper to look up by identity; if the trigger was removed during the network call, the watermark update is dropped (with INFO log noting the drop).  The fire-on-new-mail path also re-confirms the trigger still exists under the lock before queuing the fire event.

**Safety HIGH 2 — exhaustive switch discipline on `FileEventType`.**  The audit asked for `static_assert` on the variant count per `feedback_cpp_discipline`.  No actual `switch (FileEventType)` block existed in the file — the type is consumed via `std::find` (`ContainsEvent`) and `static_cast<int>` for log formatting only.  But the static_assert is the right shape to catch any future variant addition.

Fix: `static_assert(static_cast<int>(FileEventType::Deleted) == 2, ...)` in the header right after the enum, with the message naming the call sites that would need review (`ContainsEvent` / `NotifyFileEvent` / event-type logging).

**Safety MEDIUM 1 — TriggerCallback lifetime contract.**  Class member is a `std::function<void(TriggerFiredEvent const&)>` storing the caller-supplied callable.  No prior documentation of the capture-by-value contract; a caller passing a lambda capturing `&caller_local_data` would create a UAF.

Fix: header-level documentation block on the `TriggerCallback` typedef explaining the contract (capture-by-value or move; engine stores for its lifetime; invokes from arbitrary threads).  Cross-references `feedback_capture_by_value_async`.

**Safety MEDIUM 2 — `GetWebhookTrigger` OOB on stale index.**  The function returns `&m_WebhookTriggers[iterator->second]` after a map lookup.  Index/vector skew is currently impossible (every mutation site rebuilds the index under the same lock), but the function lacks a defensive bounds check.

Fix: explicit bounds check before deref; log `[error]` with workflowId on skew (the line is non-actionable in the current code but immediately spotlights any future bug that breaks the invariant) and return nullptr.  Same shape as sitting 4's `pythonEnginePool::GetTasksCompleted` OOB log fix.

**Safety MEDIUM 3 — `~TriggerEngine` Stop() throw safety.**  Pre-fix dtor called `m_TriggerFileWatcher->Stop()` directly with no exception handler; an exception escaping the dtor would call `std::terminate` (dtors are noexcept-by-default in C++11+).  `FileWatcher::Stop()` joins threads and tears down inotify/`ReadDirectoryChangesW` handles — any of those can throw under hostile conditions.

Fix: try/catch with two arms (`std::exception` + `...`), each logging at ERROR.  Mirrors sitting 2's `~WorkflowRuntimeManager` pattern.

**Safety MEDIUM 4 — `NotifyFileEvent` index access.**  Already covered by the existing bounds check in the `processIndex` lambda (line 807 pre-fix).  Sitting closes by adding a comment documenting the invariant + promoting the silent return to `LOG_APP_ERROR` on bounds-check failure (so a future skew is visible, not swallowed).

**Safety MEDIUM 5 — index-map access discipline.**  No prior class-level documentation of the lock invariant on `m_FileWatchIndex` / `m_WebhookIndex`.

Fix: class-level header comment block ("Threading & lifetime contract") explicitly documenting that the indices are valid only while `m_Mutex` is held, and that any caller dropping + re-acquiring the lock MUST look up by identity (workflowId+triggerId) rather than reusing a stored index.  Cross-references the email-poll loop as the canonical pattern.

**Safety MEDIUM 6 — WARN→ERROR on unrecoverable email/cloud failures + workflowId/triggerId context.**  Pre-fix log lines for connection-not-found, credentials-resolve-fail, IMAP-check-fail were all WARN, none with workflowId/triggerId context.  Per CLAUDE.md fail-path discipline (and `feedback_log_failures`), unrecoverable failures must be ERROR with identifiers as literal substrings so the dashboard run analyzer can attribute them.

Fix: connection-not-found → ERROR (operator config error), credentials-resolve-fail → ERROR (keystore/config error).  IMAP-check-fail stays WARN (transient network blip is a normal mode) but gains workflowId/triggerId context.  All three success-path log lines (seeded watermark / no new mail / new mail detected) also gain workflowId/triggerId context for cross-correlation.

**Cyber LOW 1 — `NormalizePath` weakly_canonical + project-root containment.**  Pre-fix `NormalizePath` only collapsed `\` → `/` and dedup'd consecutive slashes.  `..` / symlink escapes survived.  An attacker-influenced JCWF could register a watch on `/tmp/foo`, but an event for `/tmp/foo/../secret.txt` would prefix-match (since the registered watch path is a literal prefix).

Fix: `NormalizePath` now routes through the shared `application/file/pathConfinement.h::ConfineUnderProjectRoot` helper (sitting 3's lifted helper).  Returns canonical absolute path string on success, empty string on rejection.  `AddFileWatchTrigger` rejects empty (operator-config error → ERROR log + refuse to register).  `NotifyFileEvent` rejects empty (event path doesn't resolve under project root → WARN log + drop the event, no trigger fires).  Per JC_Workflow_Specification §3.2.2, file_watch paths are project-root-relative — the spec is the cover for the canonical-form switch.  No existing test JCWFs use file_watch, so no regression risk on test paths.

**Cyber LOW 3 — secret-handling discipline doc.**  Pre-fix `m_WebhookTriggers[i].m_Secret` already runs through `SecretRedactor::Get().AddSecret(secret)` in `AddWebhookTrigger`, so log lines containing the value are filtered.  Audit's concern was future maintainers iterating-and-logging the field directly, which would build a log line *around* the redactor's matching pattern.

Fix: class-level header comment in the threading-contract block documenting the rule — shape-only logging (`<set>` / `<none>`) is the standard, never iterate `m_WebhookTriggers` and log the field.

### Skipped (deferred)

| Finding | Severity | Reason |
|---|---|---|
| Hardcoded poll-interval DoS / per-workflow trigger count cap | cyber LOW 2 | Config-policy decision (what's the right cap? 100? 1000?) bigger than this sitting.  Defer to sitting 13 horizontal sweep or treat as out-of-scope per the audit's LOW-and-cluster-mate stance.  Existing `std::max(pollIntervalSeconds, 60u)` already enforces a per-trigger minimum poll interval. |
| C++20 `std::optional` for CronExpression's bool+value field pairs | LOW (style) | Non-functional refactor.  Defer to a tail style-sweep sitting if/when one is scheduled. |
| `m_` prefix on local variables | LOW (style) | Project convention is `m_` for members only; locals don't take the prefix.  Audit's note doesn't apply. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (4 ai_call tasks) | succeeded in 13 s |
| `bookSummaryPipeline` (16 ai_call + 2 freshness-skipped Python) | succeeded in 9 s |
| `cyber2` after `DELETE /api/workflows/cyber2/clean` | succeeded in 37 s |
| **Live n8n → j9t hamburg-tourist-day-planner → cloudflared trycloudflare callback** | **HTTP 200**, JC user-verified |
| `[error]` / `[critical]` lines in log post-tests | 0 |

The hamburg run exercises `GetWebhookTrigger` (post-fix bounds check on the happy path), `Tick()` lock-scope, `FireTrigger` callback delivery, and sitting 3's `IsCallbackUrlAllowed` SSRF gate end-to-end against a real public HTTPS endpoint.  Round-trip verified by external observation in n8n's webhook-test panel + j9t-side log line `[callback] completion callback ... succeeded ... HTTP 200`.

What's not directly verified:
- **Path-traversal rejection branch in `AddFileWatchTrigger` / `NotifyFileEvent`.**  Constructing a JCWF with `path: "../../etc/passwd"` (and the cleanup afterward) was setup-heavy.  Rejection branch is structurally identical to sitting 1's `pythonEngine.cpp::SetupSubInterpreter` scriptDir rejection (verified live there) — same `ConfineUnderProjectRoot` helper, same empty-return semantics, same fail-closed posture.  Code review only.
- **Email-poll identity-drift fix** under an actual concurrent `ClearWorkflowTriggers` race.  Triggering this requires precise interleaving (the lock is dropped only during IMAP I/O, ~seconds long, and a concurrent registry reload is the realistic forcing function).  Code review only — the identity-lookup pattern is well-understood.
- **`GetWebhookTrigger` OOB ERROR branch.**  Currently unreachable under correct invariant maintenance; the guard catches a future bug, not a current one.

### Open boundary at sitting end

Sitting 7 picks up **`application/workflow/aiRequestPool.{h,cpp}`** per the proposed schedule — major HTTP dispatcher with the densest concurrency surface remaining (cancel-cascade lifetime, request-handle generations, deferred-completion queue).  Audit shows safety 2H+4M; cyber 0H+3M.

Carryover: dashboard WS reconnect / lockout interaction (still pre-existing, surfaced again in this sitting's startup log) — captured under sitting 13's horizontal-sweep agenda.

---

## Sitting 7 — `application/workflow/aiRequestPool.{h,cpp}`

**Theme:** Major HTTP dispatcher; densest remaining concurrency + the workflow-side ownership of in-flight AI requests.  Counterpart to sitting 2's `WorkflowRuntimeManager::Update()` lock-scope work — both are tick-driven mutators of shared state read by REST handlers.

**Boundary held at:** `aiRequestPool.{h,cpp}` whole-file.  No call-site plumbing changes — public API surface unchanged; behavioural fixes are localised.

### Closed in this sitting

**Safety HIGH 1 — `m_DirectDispatchInflight` counter race.**  Pre-fix decrement sites used `if (m_DirectDispatchInflight.load() > 0) { --m_DirectDispatchInflight; }` at both the schema-retry path (~line 1494) and the end-of-callback path (~line 1604).  That's a check-then-act race: two concurrent decrement attempts could both observe `count == 1` and both decrement, underflowing the unsigned `size_t` counter to `SIZE_MAX`.

Fix: `m_DirectDispatchInflight.fetch_add(1, std::memory_order_acq_rel)` at submit time (replaces the `++`), capture a per-submission `std::shared_ptr<std::atomic_flag> decrementOnce` into the `curlCallback` lambda, and gate both decrement sites with `if (!decrementOnce->test_and_set(std::memory_order_acq_rel)) { fetch_sub(1, std::memory_order_acq_rel); }`.  First decrement wins, subsequent attempts no-op cleanly — works regardless of whether the dispatcher invokes the callback synchronously (mock dispatcher / dispatcher error path) or asynchronously, and bounds the schema-retry decrement-then-recurse pattern symmetrically.

**Safety HIGH 2 — "PendingEntry shared_ptr UAF" in `CancelRequestsForRun`.**  Audit's framing is structurally incorrect: only `std::vector<std::string>` (path key snapshot) leaves the `m_OutputPathMutex`-locked region, no `shared_ptr<PendingEntry>` crosses the boundary.  The dispatcher matches by opaque cancel-key string set into `QueryData::m_CancelKey` at submit time — no entry deref is required for cancellation.  No code change needed.

Closure: in-line comment block in `CancelRequestsForRun` documenting "lifetime safety: only `std::string` path keys leave the lock — no `PendingEntry` pointer crosses the boundary".  Future readers see the design rationale immediately.

**Safety MEDIUM 1 — missing ERROR log in `OnOutputFileCreated` input-stream-open fail path.**  Pre-fix, the `inputStream.is_open() == false` branch silently set `m_IsFailed = true` and returned — every other fail path in the pool emits ERROR with run/workflow/task context, this one didn't.  Fix: snapshot the context fields under the entry lock (so they're readable after release), then emit `LOG_APP_ERROR("[AiRequestPool] OnOutputFileCreated: failed to open output file run='{}' workflow='{}' task='{}' path='{}'", ...)`.

**Safety MEDIUM 2 — possible double-removal from `m_PendingByOutputPath`.**  Both `OnOutputFileCreated` and `OnRequestFailed` erase the entry from the map after lookup.  Audit asks for an "only one path can erase" assertion.

Reality: structurally already safe — the second caller's `find` returns `end()` and short-circuits via `return false`/`return true` (the `if (already completed)` branch).  Closure: header-level "Erasure invariant for m_PendingByOutputPath" doc block in the threading & lifetime contract — names the two erasure sites, documents the second-lookup-misses-and-bails pattern, and warns future maintainers not to add a third erasure site without re-checking.

**Safety MEDIUM 3 — unchecked `dynamic_cast<JarvisAgent*>(App::g_App)`.**  Audit asks for stronger assertions / restructure.  Reality: both call sites (`Submit` line 1333, `CancelRequestsForRun` line 819) already check `nullptr` after the cast and emit ERROR + early-return without touching the inflight counter.  Defense in depth is in place.

Closure: header-level "App::g_App downcast" doc block documenting the contract — production `App::g_App` is always JarvisAgent, but the nullptr branch handles a hypothetical refactor that introduces a non-JarvisAgent global.  Future restructure must preserve the nullptr-recovery behaviour.

**Safety MEDIUM 4 — non-exhaustive `InterfaceType` handling.**  Audit cites the `if (api->m_InterfaceType == ConfigParser::EngineConfig::InterfaceType::Test)` branch (~line 1164) as the canonical "switch-equivalent that doesn't enforce variant exhaustiveness".  The current Test special-case is the only InterfaceType-specific branch in the file (every other variant dispatches through `IRequestBuilder::Create` + `IRateLimitStrategy::Get` helpers).

Fix: `static_assert(ConfigParser::EngineConfig::NumAPIs == 7, "InterfaceType variant count changed — review whether the new variant needs Test-style short-circuit handling in AiRequestPool::Submit, then bump this assertion")`.  Per `feedback_cpp_discipline`: lock the variant count instead of using `default:`.

**Cyber MEDIUM 1 — path traversal in `WriteTextFile`.**  Pre-fix `std::ofstream outputStream(filePath, ...)` opened any `filePath` value without validation.  A malicious workflow's `file_outputs: ["../../etc/passwd"]` could write outside the project tree.

Fix: `WriteTextFile` now routes `filePath` through `application/file/pathConfinement.h::ConfineUnderProjectRoot` (sitting 3's lifted helper).  Empty return → reject with ERROR log + `outErrorMessage`.  The actual `ofstream` opens via the canonical `confinedPath` so symlink targets resolve to their real on-tree location.

**Cyber MEDIUM 3 — path traversal in `OnOutputFileCreated` read.**  Pre-fix `std::ifstream inputStream(fullFilePath, ...)` read any path.  Same shape as M1 — could exfiltrate sensitive host files if `expectedOutputPath` is attacker-influenced.

Fix: `OnOutputFileCreated` now routes `fullFilePath` through `ConfineUnderProjectRoot` before the map lookup AND the file read.  The same canonical form is used for both.  Reading happens via `confinedPath` so symlink resolution is honoured.  Reject with ERROR + return false on containment failure.

**Cyber LOW 1 — absolute-path normalization without containment.**  Pre-fix used `fs::absolute(...).lexically_normal().generic_string()` at four sites: `RegisterPendingWorkflowTask` insert (~line 346), `OnRequestFailed` lookup (~line 492), `Submit` log-attribution lookup (~line 1112), `Submit` cancel-key build (~line 1354).

Fix: all four converged onto `ConfineUnderProjectRoot`, producing the same canonical form for both the insert side and every lookup/match side.  Symlink resolution is handled identically across the four sites — a future symlinked queue path won't cause binding-lookup misses (which would degrade fail-path attribution to empty `run=''` / cancel-key mismatch leaving in-flight requests un-cancellable on workflow termination).

**Cyber LOW 2 — uncontrolled file size in `OnOutputFileCreated` read.**  Pre-fix `tellg()` + `resize()` had no size cap — a multi-GB file at the registered `expectedOutputPath` would exhaust RAM.

Fix: new `kMaxOutputFileBytes = 10 MB` constant; `OnOutputFileCreated` rejects any file exceeding the cap with ERROR log (run/workflow/task/bytes/cap) and marks the request failed.  Cap value is order-of-magnitude above realistic AI text replies and well below RAM-budget concerns.

**Cyber LOW 3 — secrets leakage in error logs.**  Already filtered via `SecretRedactor` at the logger layer per CLAUDE.md.  Doc-only addition: header-level note that the redactor is the canonical filter, error-message logging assumes redactor is in place.

**Discipline doc additions (header).**  Class-level "Threading & lifetime contract" block now documents:
- Mutex layout (m_MapMutex / m_CompletedMutex / m_OutputPathMutex / m_IdMutex / per-entry mutex).
- Lock-acquire order for nested locks (pool-level → entry-level; pool-level mutexes never held simultaneously).
- Path containment invariant (every filesystem-touching path routed through `ConfineUnderProjectRoot`).
- Erasure invariant for `m_PendingByOutputPath` (single-erase per entry across `OnOutputFileCreated` / `OnRequestFailed`).
- `App::g_App` downcast contract.

### Skipped (deferred)

| Finding | Severity | Reason |
|---|---|---|
| `std::ofstream` exception-safety (no `exceptions(failbit\|badbit)` on writes) | LOW | Wrap in try/catch + RAII; tail-sweep style refactor.  Defer. |
| TOCTOU on `fs::exists` parent-dir check before `create_directories` | LOW | Audit notes benign — `create_directories` is itself race-safe; the `exists` check only feeds a "created" log line.  No fix needed. |
| Per-member locking-discipline doc on every shared mutable | LOW | Class-level threading-contract block now covers the discipline broadly.  Per-member granularity defers to clang thread-safety annotations adoption (sitting 13 horizontal sweep). |
| Log-context completeness across all error paths | LOW | Most paths now have run/workflow/task context.  Remaining gaps fold into sitting 13's logger fail-path horizontal sweep. |
| `[[nodiscard]]` on bool-returning APIs (`TryConsumeResult`, `WaitForCompletion`, `TryPopCompletion`, `OnOutputFileCreated`, `OnRequestFailed`) | LOW | Defer to a tail style-sweep sitting. |
| `std::optional<AiRequestHandle>` for error-vs-success on `RegisterPending*` | LOW | API-shape change with caller-side fanout; defer to a dedicated API-modernization sweep. |
| Log-injection sanitization on broad set of attacker-influenced fields (curl error messages, paths) | MEDIUM (cyber) | The most-attacker-influenced surfaces (parser boundaries) already run through `SanitizeUtf8` + `TruncateUtf8Safe` per the §19 boundary work.  Broader sweep across all log-format args is a sitting-13-class horizontal pattern.  Defer. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (4 ai_call) | succeeded in 11 s |
| `bookSummaryPipeline` (16 ai_call + 2 freshness-skipped Python) | succeeded in 11 s |
| `cyber2` after `DELETE /api/workflows/cyber2/clean` | first run failed (curl error 28, transient AI provider timeout — NOT a sitting-7 regression); retry succeeded in 45 s |
| Path-confinement gate live | exercised by 24+ ai_call output writes across the regression matrix; zero rejections (all paths resolved cleanly under project root) |
| `OnRequestFailed` code path live | exercised by cyber2's first-run timeout — canonical path `/home/beaumanvienna/dev/jarvisAgent/queue/OpenSSH_2k/02_analyze_threats/PROB_threat_report.output.txt` and full `run='cyber2_1778123531' workflow='cyber2' task='analyze_threats'` context logged correctly via the post-fix `ConfineUnderProjectRoot`-normalized lookup |

What's not directly verified:
- **Inflight-counter race** under an actual concurrent decrement / synchronous curlCallback.  The race only manifests with a mock or error-path dispatcher returning synchronously (production async path doesn't exercise it).  The atomic_flag pattern is structurally sound; same shape as sitting 1's `WorkflowTaskRequest::m_PromiseSatisfied` CAS-acquire.  Code review only.
- **Path-traversal rejection branches** in `WriteTextFile` / `OnOutputFileCreated` / `RegisterPendingWorkflowTask`.  Constructing a JCWF with a hostile `expected_output` requires real adversarial setup; rejection branches are structurally identical to sitting 1's `pythonEngine` and sitting 6's `triggerEngine` rejection patterns (verified live in those sittings).  Code review only.
- **`OnOutputFileCreated` size-cap negative path** at 10 MB.  Triggering requires a large AI response or an attacker-pointed path; the cap arm is straightforward and ERROR-logs on trip.  Code review only.

### Open boundary at sitting end

Sitting 8 picks up **`adhocWorkflowManager.{h,cpp}` + `workflowRegistry.{h,cpp}` (bundled)** per the proposed schedule — JCWF lifecycle: adhoc submission writes into the registry, the registry's reload path is file-watch-driven and races with adhoc submission.  Audit shows adhocWorkflowManager safety 2H+4M; workflowRegistry safety 2H+2M, cyber 1M.

Carryover: dashboard WS reconnect / lockout interaction (still pre-existing).  Inflight-counter race live verification (sitting 13 fixture work).

---

## Sitting 8 — `adhocWorkflowManager.{h,cpp}` + `workflowRegistry.{h,cpp}` (bundled)

**Theme:** JCWF lifecycle.  Adhoc submission writes into the registry; the registry's reload path is file-watch-driven and races with adhoc submission + REST-editor PUTs.

**Boundary held at:** both files end-to-end; minimal API change (one method signature flip + one method return type).

### Closed in this sitting

#### `workflowRegistry.{h,cpp}`

**Safety HIGH 1 — no synchronization on mutable state.**  Pre-fix `m_Workflows` / `m_BrokenWorkflows` / `m_LastContainerError` were freely accessed from multiple call sites: REST handlers (Get*, Validate, TryGet*), workflow runtime (GetWorkflow during dispatch), AdhocWorkflowManager::Stage (SaveOrUpdate), file-watcher reload (LoadDirectory), all without any lock.  Concurrent Save + Get could observe a half-built map; concurrent Load + Get could iterate during a `clear()`.

Fix: new `mutable std::mutex m_Mutex` member; every public method (Clear, LoadDirectory, GetWorkflowIds, GetWorkflow, GetBrokenWorkflows, ValidateAll, TryGetWorkflowFilePathAbsolute, TryGetWorkflowIdByFilePath, GetSubWorkflowDependencyGraph, SaveOrUpdateWorkflowFromJson, RemoveWorkflow) acquires the lock for the duration.  `LoadDirectory` inlines the body of `Clear()` to avoid a re-entry deadlock.  Private helpers (`LoadContainer`, `LoadContainerSubWorkflows`) document "caller holds m_Mutex" in the header.

Recursive-lock hazard caught + fixed: `GetSubWorkflowDependencyGraph` iterates the map while resolving each child workflow's id by file path.  Pre-fix that called `TryGetWorkflowIdByFilePath` which would now re-acquire the same non-recursive mutex → deadlock.  Fix: extracted `TryGetWorkflowIdByFilePathLocked` private helper; public TryGet* delegates after acquiring the lock; GetSubWorkflowDependencyGraph calls Locked directly.

**Safety MEDIUM 2 — `GetBrokenWorkflows` returns reference to mutable vector.**  Pre-fix returned `std::vector<BrokenWorkflow> const&` to the internal vector — caller could capture the reference and read it after another thread cleared the registry.  Fix: returns `std::vector<BrokenWorkflow>` by value (snapshot under the lock).  Vector is small (count of broken JCWFs in the workflows tree) so copy cost is negligible.

**Safety MEDIUM 1 — exception safety in `SaveOrUpdateWorkflowFromJson`.**  Pre-fix the multi-step file work (read existing global.json, write canvas JSON, JcwfContainer::Pack, set path metadata, insert into m_Workflows) had no exception guard.  An exception from `fs::weakly_canonical` (pathological path) or `JcwfContainer::Pack` (zip lib failure) would leak a half-written extracted directory + leave the registry untouched (which is correct) but with no diagnostic.

Fix: try/catch around the file-and-registry block.  Two arms (`std::exception&` + `...`); both ERROR-log with workflowId + path.  m_Workflows insert remains the LAST step inside the try, so a thrown exception cannot leave the registry holding a half-built definition.  Plus added explicit ERROR + return for the global.json open-failure case (was silent before).

**Cyber MEDIUM — path traversal in JCWF write/delete.**  Pre-fix `SaveOrUpdateWorkflowFromJson` accepted any `workflowFilePathAbsolute`; `RemoveWorkflow` deleted any stored path.  A malicious caller could pass `..`-laced paths or absolute paths outside the project root and overwrite arbitrary files.

Fix: both routes through `application/file/pathConfinement.h::ConfineUnderProjectRoot` (sitting 3's helper).  SaveOrUpdate rejects with ERROR + early return on containment failure.  RemoveWorkflow re-validates at delete time (defense in depth — the path was canonicalised at insert time, but a future bug that lets an unconfined path into the registry can't trigger an arbitrary-file delete).

**Safety HIGH 2 (LoadContainerSubWorkflows partial-parse cleanup).**  Audit asks for cleanup of partial extraction on parse error.  Reality on close inspection: `LoadContainerSubWorkflows` has try/catch around `ParseCanvasJson` already, logs WARN, and continues to the next sibling.  The "partial extraction" the audit flags is actually the JCWF zip's already-extracted folder structure on disk — that's `JcwfContainer::Extract`'s output, used by both legitimate sub-workflow folders AND any folders that fail to parse.  Removing failed-parse folders would corrupt the extracted tree and break the next legitimate `LoadContainer` (the freshness check would re-extract).  Closure: design rationale comment in the header documenting the "leave on disk for diagnostic + freshness re-extraction" behaviour.  Audit framing was structural; no code change appropriate.

#### `adhocWorkflowManager.{h,cpp}`

**Safety HIGH 1 — data race on `m_ReaperRunning` + `m_ReaperThread`.**  `m_ReaperRunning` was atomic, but `m_ReaperThread` (the std::thread member itself) wasn't — concurrent Start/Stop on the same instance could race the std::thread assignment.  Atomic alone doesn't make a non-atomic member safe.

Fix: new `mutable std::mutex m_ReaperLifecycleMutex`; both StartReaperThread and StopReaperThread acquire it for the duration of the (atomic flag flip + thread member assignment + join) sequence.  Atomic flag is still the externally-visible run state; the lock just serializes Start/Stop pairs.

**Safety HIGH 2 — Stage cleanup gaps on early-return paths.**  Pre-fix Stage had two early-return cases AFTER directory creation (`failed to create folder` + `failed to create queue folder`) that returned without removing the partially-created folder.  Plus `WriteMeta` was a void function that silently swallowed open failures — a write error after the registry save left the folder retained but un-attributable (artifact-retrieval ownership checks rely on meta.json).

Fix:
- All post-create_directories early returns now call `RemoveFolder(folder)` before returning.
- `WriteMeta` returns `bool` ([[nodiscard]]); Stage rolls back the folder via `RemoveFolder` when WriteMeta fails.
- WriteMeta itself logs ERROR on open OR write-stream failure with full context (user, folder).

**Safety MEDIUM 2 — slow shutdown via 1-second-poll reaper sleep.**  Pre-fix ReaperLoop did `for (60) { sleep(1) }`-style polling to keep StopReaperThread responsive, but a Stop call still waited up to 1 second for the loop to observe the flag.  Real wins are tiny (1 s shutdown latency) but the pattern is wrong.

Fix: `m_ReaperCv` (`std::condition_variable`) member; ReaperLoop now `wait_for(60s, predicate=stop-requested)` on `m_Mutex`.  StopReaperThread notifies under m_Mutex after flipping `m_ReaperRunning`.  Wake-on-stop is immediate; predicate guards spurious wakeups.

**Safety MEDIUM 3 — `OnRunCompleted` silent return on missing runId.**  Pre-fix early `return` on `m_RunIdToMeta.find` miss with no log.  Reaching the miss case means either a duplicate completion (already reaped) or an upstream logic bug — both worth surfacing.

Fix: LOG_APP_WARN with the runId before return.  WARN (not ERROR) because duplicate completion is a normal mode in cancel-cascade scenarios; the log line is for cross-correlation, not alarm.

**Safety MEDIUM 4 — silent open-failure in `WriteMeta` and `WriteManifest`.**  Both pre-fix `if (!os) return;` with no diagnostic.

Fix: LOG_APP_ERROR with path + run/user context.  WriteMeta now returns bool (covered above); WriteManifest stays void but logs.

**Cyber LOW — no size cap on submitted JCWF JSON in Stage.**  Pre-fix accepted any size; per-user disk quota is checked but only after the JSON is in memory.

Fix: new `kMaxJcwfBytes = 4 MB` constant; Stage rejects oversized submissions with ERROR + early return.  4 MB is well above realistic JCWF size (existing examples top out near 50 KB) and well below the Crow body cap configured at the server layer.  No folder-creation rollback needed at this gate (return is BEFORE any directory work).

### Skipped (deferred)

| Finding | Severity | Reason |
|---|---|---|
| SanitizeUserSlug collision risk (two distinct user names → same slug) | cyber LOW | Proper fix is appending a hash of the original user — needs a stable hash design + a migration path for existing meta.json files.  Defer to a dedicated user-slug refactor sitting. |
| Regex-based RewriteWorkflowId fragility | cyber LOW | Proper fix is simdjson-based parse-and-rebuild (per `feedback_simdjson_only`).  Bundle with sitting 12's parser cluster sweep — the same simdjson migration touches `ParseToolCallJson`, `ReadMeta`'s pluck-by-string-search, etc.  Defer. |
| Non-atomic `m_DiskUsageByUser` updates outside lock | safety MED | Audit's concern is "if any future code path accesses the map outside the lock"; current code respects the discipline.  Closure: header-level `m_Mutex` discipline doc covers this; per-member annotation defers to clang thread-safety annotations adoption. |
| Filesystem iterator exception safety on directory mutation during iteration | LOW | Existing `error_code` handling is sufficient for the documented use cases.  Defer. |
| `[[nodiscard]]` on Stage / TryGet* / etc. | LOW | API-shape sweep; defer to sitting 13. |
| C++20 idiom adoption (string_view, span) | LOW | Style/perf; defer. |
| Lifecycle interaction with `WebServer::SetWorkflowRegistry` (unjoined reaper from prior instance) | safety MED (cross-file) | Audit's concern is `WebServer::SetWorkflowRegistry` not resetting the prior `unique_ptr<AdhocWorkflowManager>`.  WebServer is in S1=D2 territory and was hardened in sittings 1-4; the documented pattern is "set once at startup".  Closure: cross-reference comment in adhocWorkflowManager header.  If WebServer's set-twice surface ever becomes legitimate, the destructor + StopReaperThread already join the thread cleanly — net effect would be a brief gap, not a leak. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (4 ai_call) | succeeded in 14 s |
| `bookSummaryPipeline` (16 ai_call + 2 freshness-skipped Python) | succeeded in 9 s |
| `cyber2` after `DELETE /api/workflows/cyber2/clean` | succeeded in 54 s |
| **Adhoc workflow submission happy path** | full lifecycle: Stage → SaveOrUpdateWorkflowFromJson (post-fix path-confinement gate) → WriteMeta (post-fix bool return) → workflow run → OnRunCompleted (post-fix locked block) → on_completion cleanup → user-slug parent dir cleaned (`_adhoc/` empty after run) |
| Registry init log | All 22+ JCWF containers loaded under the new lock; no warnings beyond pre-existing "no canvas JSON" sub-workflow folder messages |
| Adhoc Init log | `[adhoc] Init: base=... scanned 0 run(s) ... counter_floor=1` + `[adhoc] Manager ready` — clean |

What's not directly verified:
- **WorkflowRegistry mutex stress** under actual concurrent reload + REST-editor PUT.  The lock pattern is structurally sound (single mutex, always-acquire-first discipline; no nested locks across pool-level mutexes).  Code review only.
- **Adhoc Stage rollback paths** (failed create_directories / WriteMeta failure / SaveOrUpdate failure).  All hit standard filesystem error_code paths; the cleanup `RemoveFolder` calls were added at every early-return after directory creation.  Code review only.
- **Reaper CV wake-on-stop** under actual shutdown timing.  Structural; same shape as sitting 1's promise-CAS guard.  Code review only.
- **JCWF size cap negative path** at 4 MB.  Constructing a 4 MB JCWF JSON is feasible but not economical for a sitting verification.  The cap is a simple `if (size > N) return error` early exit.  Code review only.
- **Path-traversal rejection branches** in `SaveOrUpdateWorkflowFromJson` / `RemoveWorkflow`.  Same shape as sitting 1/3/6/7 rejection patterns.  Code review only.

### Open boundary at sitting end

Sitting 9 picks up **`polarionClient.{h,cpp}` + `dbQueryCloudTaskExecutor.{h,cpp}` (cloud-adjacent boundary cluster)** per the proposed schedule.  The latter holds the **lone CRITICAL in the entire fresh cyber-sec audit** (SQL injection); D2 territory back-folded into D1 only to close the only CRITICAL in one shot.  PolarionClient is the bigger work item (cyber 1H + 3M; safety 0H + 4M; XML parsing).

Carryover: dashboard WS reconnect / lockout interaction (still pre-existing).  Inflight-counter race live verification.  SanitizeUserSlug collision design.  RewriteWorkflowId simdjson rewrite (sitting 12).

---

## Sitting 9 — `dbQueryCloudTaskExecutor.{h,cpp}` + `polarionClient.{h,cpp}` (cloud-adjacent boundary)

**Theme:** Cloud-adjacent boundary cluster.  `dbQueryCloudTaskExecutor` carried the **lone CRITICAL in the entire fresh cyber-sec audit** (SQL injection); back-folded from D2 territory into D1 to close the only remaining CRITICAL in one shot.  PolarionClient is the bigger work item by line count and finding density.

**Boundary held at:** both files end-to-end; one .h trust-model doc block addition; minimal call-site impact.

### Closed in this sitting

#### `dbQueryCloudTaskExecutor.{h,cpp}`

**Cyber CRITICAL — SQL injection via unvalidated query parameter.**  Audit's framing called for "parameterized queries", but `query` IS the SQL the workflow author wrote — there's no template-and-substitution boundary to harden.  The structural fix is a layered trust model + bounded blast radius rather than blocking legitimate queries.

Closure (three layers, documented in the header trust-model block):
1. **Operator gate at submission** — db_query reaches the runtime only via JCWFs the operator authored or approved.  Adhoc submissions additionally require `adhoc_enabled` + `operator` role minimum (sitting 8).
2. **DB-side permissions** — operator MUST configure the connection's DB user with the minimum permissions for the workflow's queries.  This is the only durable defense against malicious or buggy SQL.
3. **Blast-radius caps** — new JCWF params with hard ceilings:
   - `max_rows` (default 100k, ceiling 1M) — bounds disk + RAM before write loop.
   - `max_output_bytes` (default 100MB, ceiling 1GB) — runs after each row write, aborts cleanly on cap.
   - `statement_timeout_ms` (default 60s, ceiling 600s) — server-side via `SET statement_timeout = N` before the user query, so a runaway query terminates DB-side regardless of client timeout.

The CRITICAL closes on this trust model — the audit's "parameterized queries" recommendation doesn't apply to this surface.

**Cyber HIGH — output_file path traversal.**  Pre-fix `output_file` was joined with `workDir` and used verbatim for `std::ofstream`.  A workflow with `"output_file": "../../etc/passwd"` would write outside the task working directory.

Fix: two layers.  (a) Reject path separators in `output_file` — bare filename only (no `/`, no `\`, no `..`/`.`).  (b) Pass the resolved `workDir / output_file` through `application/file/pathConfinement.h::ConfineUnderProjectRoot` for symmetry with sittings 1/3/6/7/8.  Either gate alone closes the traversal class; both together make a future bug in workflow-base-directory resolution can't reopen it.

**Safety HIGH — PG resource leak on exception.**  Pre-fix had 6 explicit `PQfinish` / `PQclear` sites and would leak `PGconn*` / `PGresult*` on any `std::bad_alloc` / `std::filesystem` exception between handle acquisition and the explicit free.  No RAII wrapper.

Fix: anon-namespace `PgConnDeleter` / `PgResultDeleter` deleters; `unique_ptr<PGconn, PgConnDeleter>` / `unique_ptr<PGresult, PgResultDeleter>`.  Every exit path (early return, exception, scope end) releases the handle automatically.

**Safety MEDIUM — `PQconnectdb` nullptr check.**  `PQconnectdb` returns nullptr only on libpq OOM — extremely rare but non-zero probability.  Pre-fix called `PQstatus(conn)` without a nullptr check → SIGSEGV.

Fix: explicit `if (!conn)` guard with distinct ERROR log so operators can tell "couldn't allocate" from "couldn't connect".

**Safety MEDIUM — libpq error message truncation.**  Pre-fix raw `PQerrorMessage` + `PQresultErrorMessage` flowed directly into `taskState.m_LastErrorMessage` and the log line.  libpq error text can include connection-string fragments, role names, or table layouts — could echo DB schema into the dashboard via the run analyzer's per-run filter.

Fix: new `TruncateLibpqError(char const*)` helper.  Strips trailing newline (libpq always appends `\n`), caps to 250 chars, appends `... (truncated)` if cut.  Applied at all 4 PQerror sites.

**Safety MEDIUM — fail-path logging completeness.**  Pre-fix several early returns (JSON parse fail, missing `query` param, format invalid) set `m_LastErrorMessage` but emitted no `LOG_APP_ERROR`.  Per CLAUDE.md fail-path discipline.

Fix: every early return now LOG_APP_ERRORs with `task='...' workflow='...' run='...'` literals so the dashboard run analyzer can attribute the failure.

#### `polarionClient.{h,cpp}`

**Cyber HIGH — `WriteItemFile` path traversal via `filter.m_Id`.**  `filter.m_Id` is JCWF-authored.  Pre-fix joined directly with `workflowBaseDir` and the resulting filename — a hostile JCWF with `filter.m_Id = "../../etc"` would write outside the workflow tree.

Fix: two layers.  (a) New anon-namespace `IsValidFilesystemId` allowlist — `[A-Za-z0-9._-]`, no leading dot, length 1-64 (mirrors `SanitizeUserSlug` from sitting 8 for cross-component consistency).  (b) `ConfineUnderProjectRoot` on the resolved file path.  Stream open and write happen via the canonical confined path.

Plus the post-write check: `if (!file.good()) return false` after the JSON output loop, with ERROR log.  Pre-fix only checked `is_open()` — disk-full or permission-loss-mid-write would silently produce a truncated JSON file.

**Cyber MEDIUM — URL component validation.**  `projectId`, `workItemId`, `attachmentId` flow through `UrlEncode` (the standard URL-injection defense) but had no shape validation.  A hostile JCWF could supply path-injection attempts that survive UrlEncode.

Fix: new anon-namespace `IsValidPolarionId` allowlist — `[A-Za-z0-9._-/]`, no `..` substring, length 1-256.  Allows `/` because Polarion work-item IDs can be project-qualified (`Proj/REQ-001`).  Applied at all 5 URL builders (`UpdateWorkItem`, `CreateWorkItem`, `DownloadAttachment`, `UploadAttachment`, `FetchLinkedWorkItems`).

**Cyber MEDIUM — HTTP error body in error message.**  Pre-fix appended response body to `errorMessage` if `body.size() < 500`.  An error response from Polarion can include tokens or identifiers we don't want in dashboard logs.

Fix: new `SanitizeErrorBody` helper — caps at 200 bytes, runs through `SanitizeUtf8` (workflow-types) so malformed bytes don't leak into dashboard / log.  Applied at all 4 HTTP method paths (`HttpGet`, `HttpRequest`, `HttpUploadFile`, `HttpDownloadFile`).

**Cyber MEDIUM — Download file size cap.**  Pre-fix `HttpDownloadFile` had no body-size limit — a hostile or buggy upstream could stream gigabytes onto the local disk.

Fix: `CURLOPT_MAXFILESIZE = 100 MB`.  libcurl aborts with `CURLE_FILESIZE_EXCEEDED` past the cap.  Plus `ConfineUnderProjectRoot` on the `outputPath` argument in `DownloadAttachment` — combined with the size cap, the worst case is a sized file inside the project tree.

**Safety MEDIUM — `WriteItemFile` post-write `file.good()` check.**  See Cyber HIGH above (folded in same edit).

**Cleanup LOW — `JsonHelper::EscapeJsonString` convergence in `WriteItemFile`.**  Pre-fix had a hand-written switch handling only 5 escape sequences — raw control bytes (< 0x20) leaked into JSON output.  Per the established pattern from S1=D2 sittings 9/12/19, replaced with `JsonHelper::EscapeJsonString`.  Cyber LOW "Weak/Minimal JSON Escaping" closes.

### Skipped (deferred)

| Finding | Severity | Reason |
|---|---|---|
| Bearer token in `std::string` (Polarion `FetchAll`) | cyber LOW | Defense in depth — would require a SecureString-only path through the HTTP layer, which is a bigger refactor.  Defer to a dedicated secure-string sweep. |
| simdjson error-discipline doc on `ParseJsonApiPage` | safety MEDIUM (audit framing) | Current code already checks `get(...).get(ec)` and bails on non-success at every reachable site.  No code change appropriate; closure is structural. |
| Atomic-write pattern for `WriteItemFile` (write-to-temp + rename) | safety MEDIUM | Race window is benign in practice (per-filter directory; concurrent writes from same filter would be a logic bug upstream).  Defer to a future filesystem-atomicity sweep that covers all hand-built JSON writers. |
| `std::optional<T>` / `std::expected<T,E>` API-shape sweep | LOW | API-shape change with caller-side fanout; defer to sitting 13. |
| Style / idiom items (noexcept, by-value-and-move, span vs index loops) | LOW | Defer. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (4 ai_call) | succeeded in 14 s |
| **`postgresDemo` (7 db_query executions across 6 tasks, including 3 fan-out children of `05_write_analysis`)** | **all clean — full live verification of the dbQuery rewrite**: RAII PGconn/PGresult handles, server-side `SET statement_timeout = 60000`, row + byte caps, `ConfineUnderProjectRoot` on output path, all exercised on a real local PostgreSQL connection (`local-pg` against `j9t_test` DB) |
| `bookSummaryPipeline` (16 ai_call + 2 freshness-skipped Python) | succeeded in 11 s |
| `cyber2` after clean | succeeded in 42 s |
| Zero `[error]` / `[critical]` lines since restart | confirmed |

What's not directly verified:
- **PolarionClient end-to-end** — no Polarion fixture available in this verification session.  Changes are structural (allowlist gates, error-body sanitization, MAXFILESIZE setopt, post-write check, JsonHelper convergence); same shape as sitting 8's allowlists and sittings 6/7/8's path-confinement work, all of which were verified live in those sittings.  Code review only.
- **db_query CRITICAL rejection branches** (max_rows / max_output_bytes / statement_timeout / output_file with path separator).  Constructing each negative case is feasible (e.g., a query returning 100k+1 rows, or `output_file: "../foo.csv"`) but not economical for a sitting-9 verification.  Each cap is a simple `if (... > ...) return error` early exit; the structural shape matches sitting 7's 10 MB output cap (verified by code review there).

### Open boundary at sitting end

Sitting 10 picks up **`shellTaskExecutor.{h,cpp}`** per the proposed schedule — dense safety surface (2H + 5M) on a file already extensively reworked for security per `feedback_argv_only_shell`.  Cyber axis empty (the argv-only refactor removed the shell-injection class); this sitting is purely about thread-safety / lifetime / RAII discipline around the spawned process + watchdog.

Carryover: PolarionClient live verification (folds into the next polarion-touching workflow run, no dedicated sitting needed).  Dashboard WS reconnect / lockout interaction.  Inflight-counter race live verification (sitting 13 fixture work).  SanitizeUserSlug collision design.  RewriteWorkflowId simdjson rewrite (sitting 12).

---

## Sitting 10 — `shellTaskExecutor.{h,cpp}`

**Theme:** Shell-task execution: spawned-process lifetime, watchdog discipline, command-construction safety.  The plan brief said "cyber axis empty" but a closer read found the audit's HIGH on `JoinArgumentsForSystem` was real — non-whitespace args were going through unquoted, so a literal `$(rm -rf /)`-style arg slipped past `IsSafeArgument`'s blocklist (which didn't reject `$ ( )`) and reached the shell as command substitution.  Bundled the cyber HIGHs alongside the safety work.

**Boundary held at:** `shellTaskExecutor.{h,cpp}` end-to-end.  No call-site plumbing changes; `s_WindowsShell` member-type change is internal (the `Atomic<WindowsShell>` doesn't escape the class).

### Closed in this sitting

#### Cyber axis (turned out to be non-empty)

**Cyber HIGH 1 — shell command construction allowed argument-level injection.**  Pre-fix `JoinArgumentsForSystem` only single-quoted args containing whitespace; non-whitespace args were emitted unquoted into the `sh -c "..."` string.  Combined with `IsSafeArgument`'s blocklist that didn't reject `$ ( ) \`, a literal arg of `"$(rm -rf /)"` would pass the safety check, get emitted unquoted, and execute as command substitution under the shell.

Fix: **always single-quote every arg** in `JoinArgumentsForSystem` regardless of whitespace.  `QuoteForPosixShell` wraps in `'…'` with embedded-quote escape (`'\''`), so the shell treats the entire arg as a literal — globbing, variable expansion, and command substitution all neutralised.  Plus `IsSafeArgument` extended to reject `$ ( ) \` (defense in depth on top of the always-quote).  Glob-expansion expectations belong in the `command` field (concatenated raw); `args[]` is for positional args and SHOULD be literal.

**Cyber HIGH 2 — `ValidateScriptPath` traversal.**  Pre-fix only checked `"scripts/"` prefix on raw + lexically-normalised path; a symlink `scripts/foo` pointing at `/etc/passwd` would pass.

Fix: routes through `application/file/pathConfinement.h::ConfineUnderProjectRoot` (sittings 3+ helper) which runs `fs::weakly_canonical` + lexically_relative containment + fail-closes on resolution error.  Then explicit `lexically_relative(<projectRoot>/scripts/)` check rejects anything that lands outside the scripts subtree.  Symlink-escape vector closed.

**Cyber MEDIUM — `.ja_stderr_tmp` filename race.**  Pre-fix used a fixed `.ja_stderr_tmp` filename per working directory; concurrent shell tasks in the same workDir would clobber each other's stderr, AND the predictable name was symlink-baitable in multi-tenant deployments.

Fix: per-task unique filename `.ja_stderr_<pid>_<counter>.tmp` using `getpid()` + a process-global atomic monotonic counter.  No filesystem call needed for collision avoidance.

**Cyber LOW — hardcoded `JARVIS_PORT=8080`.**  Wrong for HTTPS-default deployments (j9t lands on 8443) and discloses assumed topology.

Fix: read `Core::g_Core->GetConfig().m_Port`, fall back to `8443` if 0 (the `auto` sentinel).  Inside the forked child after dup2/close — the config read is bounded and `setenv` itself is already non-async-safe so the bar for "what's allowed in the child block" was already non-pristine; structurally consistent.

#### Safety axis

**Safety HIGH 1 — `s_WindowsShell` static thread safety.**  Pre-fix non-atomic `WindowsShell s_WindowsShell` was written by `ProbeWindowsShell` at startup and read by `Execute()` from worker-thread context; concurrent read/write would race on the enum's underlying integer.

Fix: `std::atomic<WindowsShell> s_WindowsShell{...}`.  `WindowsShell` is a POD enum so the atomic specialisation is trivial.  `ProbeWindowsShell` uses release-store; `GetWindowsShell` uses acquire-load.  External API (`static WindowsShell GetWindowsShell()`) returns the loaded value, so callers see no change.

**Safety HIGH 2 — simdjson iterator/move incorrectness.**  Code-review verified: pre-fix code already keeps `paddedJson` and `parser` in the same scope as all `params[...]` access; no string_view escapes.  Closure: structural — current code is correct; no change needed.

**Safety MEDIUM 1 — `g_ShellTaskExecutorCurrentPathMutex` purpose unclear.**  The mutex name suggested it was protecting `current_path` mutation, but the comment confirms that mutation was retired in favour of `cd` inside the spawned subshell.  What does it protect now?  Closure: comment block documenting it as a coarse "one-shell-task-at-a-time gate that bounds concurrent pressure on the system" — and noting that the per-task unique stderr filename means race-avoidance no longer depends on this lock; could be relaxed to per-working-directory if shell-task throughput becomes a bottleneck.

**Safety MEDIUM 2 — unchecked `fs::remove` return.**  Pre-fix dropped the error_code from `std::filesystem::remove` calls (stderr temp file cleanup, empty stdout/stderr.txt cleanup).  A failed remove silently leaks the file.

Fix: capture into a local `error_code` and `LOG_APP_WARN` on failure with task context.

**Safety MEDIUM 3 — exception safety on file/stream open.**  Pre-fix stdout.txt / stderr.txt writes used `is_open()` only; mid-write disk-full / quota-loss would silently truncate.

Fix: post-write `if (!file.good())` check with `LOG_APP_ERROR` + task context.  Pattern matches sitting 9's PolarionClient `WriteItemFile` fix.

**Safety MEDIUM 4 — popen `FILE*` leak on exception.**  Pre-fix `OpenPipe(...)` returned a raw `FILE*` and the explicit `ClosePipe` only ran on the happy path.  An exception in the read loop (logger throwing, std::string growth) leaked the FILE* + child process until process exit.

Fix: new `PipeCloser` deleter + `using PipePtr = std::unique_ptr<FILE, PipeCloser>`.  RAII close on every exit path.  The deleter writes the exit status into a captured `int*` so the caller can recover it after `pipe.reset()`.

**Safety MEDIUM 5 — TOCTOU on `fs::exists` checks.**  Pre-existing pattern; same audit framing as past sittings (benign in practice — the `exists` checks feed log lines, not security gates).  Closure: structural.  No fix.

#### Hygiene + doc

- **Trust-model block** added to the header — three layers (operator gate / `ValidateScriptPath` / always-quote+`IsSafeArgument`).  No audit-finding citations in code per `feedback_no_audit_traces_in_code`.
- **Audit-citation cleanup sweep** across sittings 6-9: scrubbed three audit-finding references in code that slipped past during this session — `dbQueryCloudTaskExecutor.h` trust-model block (sitting 9), `aiRequestPool.cpp` size-cap comment + CancelRequestsForRun snapshot pattern (sitting 7).  No behavioural change; comments now stand on their own without invoking external citations.

### Skipped (deferred)

| Finding | Severity | Reason |
|---|---|---|
| `setenv` with untrusted taskId (`JARVIS_TASK_ID`) | cyber LOW | Bundle into sitting 13's logger / log-injection horizontal sweep — same shape as the broader "sanitise attacker-influenced strings before serialisation" work.  Defer. |
| Default permissions on dirs/files (multi-tenant concern) | cyber LOW | Deployment-scope hardening (operator's umask + Docker-isolation responsibility), not per-file.  Defer. |
| Default in `IsSafeArgument` switch over closed set | safety LOW | The `default:` is correct here — it's `if (this byte is one of the forbidden set) reject; else allow`.  No closed enum to lock down.  No fix appropriate. |
| `[[nodiscard]]` on `ShellTaskExecutor::Execute` | safety LOW | Sitting 13 API-shape sweep. |
| `std::span<const std::string>` for argv params | safety LOW | C++20 idiom uplift; sitting 13. |
| Unstructured log message construction | safety LOW | Style/idiom; sitting 13 logger sweep. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug build | clean |
| Studio Release build | clean |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (includes `zip_responses` shell task) | succeeded in 14 s — **shell task path exercised post-fix**: PipeCloser RAII pclose, per-task unique stderr filename, always-quoted args, IsSafeArgument extended blocklist all live |
| `bookSummaryPipeline` | succeeded in 9 s |
| `cyber2` after clean (Python via fork/exec/poll watchdog path) | succeeded in 38 s — **watchdog path exercised post-fix**: JARVIS_PORT now reads from config (8443), JARVIS_TASK_ID set in child, fork+chdir+execl path unchanged |
| Zero `[error]` / `[critical]` lines since restart | confirmed |

What's not directly verified:
- **Cyber HIGH rejection branches** — `ValidateScriptPath` symlink-escape, `IsSafeArgument` `$ ( ) \` reject, `JoinArgumentsForSystem` always-quote vs malicious arg.  Constructing each negative case is feasible but verification deferred to "Negative-path verification fixtures for D1 hardening" (`todo.md` Loose follow-ups).
- **`s_WindowsShell` atomic race** under actual concurrent Probe+Get.  Windows-only path; structural fix matches sittings 1+2 atomic-flag patterns.  Code review only.
- **PipeCloser exception path** — would need an in-loop exception to verify the RAII close fires.  Structural; pattern matches sitting 9's PgConnDeleter.

### Open boundary at sitting end

Sitting 11 picks up the **task-executor + small-file bundle** per the proposed schedule — `subWorkflowTaskExecutor`, `taskExecutorRegistry`, `taskPathResolver`, `pythonTaskExecutor`, `workflowFileIndex`, `workflowTriggerBinder`, with `aiCallTaskExecutor` folded if depth allows.  Each file is small (sub-300 lines typical) but the count adds up; pattern recognition from prior sittings should compress.

Carryover: same as before plus PolarionClient live verification and inflight-counter race live verification (both bundled into the negative-path fixture entry in `todo.md`).

---

## Sitting 11 — task-executor + small-file bundle

**Theme:** Six files in one sitting.  Pattern across all six was the same: external strings (JCWF-authored) reaching filesystem APIs without containment, plus one webhook-secret default-permissive parse path.  `aiCallTaskExecutor` (1796 lines, comparable to workflowRuntimeManager) was deferred to its own slot — bundling would have busted the sitting's scope.

**Boundary held at:** six files; minimal cross-file plumbing (one `ParseWebhookParams` caller-side fix in the same file).  No header signature changes outside the trust-model doc on `taskPathResolver.h`.

### Closed in this sitting

#### `subWorkflowTaskExecutor.{h,cpp}`

**Cyber HIGH — `m_WorkflowFile` path traversal.**  Pre-fix joined caller-supplied `taskDefinition.m_WorkflowFile` with the parent workflow's directory and `weakly_canonical`'d the result, but did not verify the result stays under the project root.  A hostile JCWF with `m_WorkflowFile = "../../etc/passwd"` would resolve verbatim.

Fix: route through `application/file/pathConfinement.h::ConfineUnderProjectRoot`.  Rejection is fail-closed with ERROR + task-state failure.

Skipped (deferred): MEDIUM "log path leak" (sitting 13 logger sweep), LOW "uncontrolled child workflow launch depth" (handled at runtime layer; cross-ref comment).

#### `taskExecutorRegistry.{h,cpp}`

**Cyber HIGH — `MaterializeFiles` target path traversal.**  Pre-fix joined `taskWorkingDirectoryPath / targetFilename` without containment.  Hostile `targetFilename = "../../../etc/passwd"` would resolve to an absolute path outside the working dir and `copy_file` would happily overwrite it.

Fix: `lexically_relative(taskWorkingDirectoryPath)` containment check after the join.  Reject empty / `..` / `..`-prefix relatives with ERROR.  Defense in depth on top of the existing template-expansion sanity check.

Skipped (deferred): MEDIUM "log leakage" (sitting 13), LOW "directory creation error handling" (already returns false on error_code, code review confirms).

#### `taskPathResolver.{h,cpp}`

**Cyber HIGH — pass-through resolver doesn't enforce containment.**  Audit's framing wants `ResolvePath` / `ResolveTaskScopedPath` to gate internally.  But the helpers serve many call sites with different intended scopes (project root / workflow base / task working dir) — a one-size containment policy here would either reject legitimate cross-base resolution or fail open on hostile inputs.

Fix: header-level "Trust model" block documenting the resolver as **pass-through by design** and the caller's responsibility to gate via `ConfineUnderProjectRoot` before any filesystem-touching API.  Cross-references the established pattern (sittings 1, 3, 6, 7, 8, 9, 11) where each caller knows its scope.  No code change to the resolver itself; the HIGH closes via the per-call-site gates added in this and prior sittings.

Skipped (deferred): MEDIUM "excessive logging" (style; sitting 13), LOW "input sanitisation" (handled at JCWF parse layer + per-call-site containment).

#### `pythonTaskExecutor.{h,cpp}`

**Cyber HIGH — `file_inputs` path traversal.**  Pre-fix `ValidateFileInputsExist` resolved `taskDefinition.m_FileInputs[i]` via `TaskPathResolver::ResolvePath` (pass-through) and then `fs::exists` checked the result.  A hostile JCWF input of `"../../etc/passwd"` would resolve and pass the existence check, then flow to Python as a context value.

Fix: `ConfineUnderProjectRoot` on the resolved path before the existence check.  Fail-closed with ERROR + task-state failure.  Sitting 1 partially addressed `taskWorkingDirectory` (Python sandboxing); this closes the file-inputs side.

Skipped (deferred): MEDIUM "secrets/PII in logs" (sitting 13), LOW "unchecked deserialisation" (Python module gate is the structural fix from sitting 1), LOW "stdout/stderr unbounded" (sitting 7's pattern; defer).

#### `workflowFileIndex.{h,cpp}`

**Cyber HIGH — `FindByRelativePath` traversal.**  Pre-fix joined `m_RootDirectory / relativePath` and lexically-normalised, but did not verify containment under the root.  Hostile `relativePath = "../../../../etc/passwd"` would resolve outside the root and surface a path the existence check happily returned.

Fix: `lexically_relative(m_RootDirectory)` containment check; reject empty / `..` / `..`-prefix relatives with ERROR + return empty path.  Closes the file-enumeration vector.

Skipped (deferred): LOW "log path leakage" (sitting 13).

#### `workflowTriggerBinder.{h,cpp}`

**Cyber MEDIUM — webhook secret-missing default-open.**  This was the operationally-critical finding in the bundle.  Pre-fix `ParseWebhookParams` returned `true` (success) on every failure mode — empty params JSON, parse error, missing `secret` field, empty `secret` value — and the **caller IGNORED the return value entirely**, calling `AddWebhookTrigger(...)` with an empty secret.  An "open webhook" would slip past the binder layer; only the TriggerEngine validator's own empty-secret refusal stopped it.  Defense-in-depth gap — and the kind of bug where the upstream layer being correct masks the downstream layer being broken.

Fix: `ParseWebhookParams` now returns false on every failure mode with ERROR log naming the specific cause (empty params, parse error, non-object root, missing/empty `secret` field).  The caller (`RegisterAll`'s `WorkflowTriggerType::Webhook` branch) checks the return and breaks out of the case with an ERROR + workflow/trigger context if parse fails — webhook trigger is never registered with an empty secret.  TriggerEngine still has its own check; both gates fail-closed.

Live verification: hamburg-tourist-day-planner's webhook trigger still registers cleanly post-fix (its `global.json` has `params.secret = "demo-shared-secret-..."` — the fix was about REJECTING missing-secret, not requiring something new).

Skipped (deferred): cyber HIGH "file_watch path traversal" — already addressed in sitting 6 (`triggerEngine::NormalizePath` routes through `ConfineUnderProjectRoot`); the binder layer hands the raw watchedPath to `AddFileWatchTrigger` which canonicalises it.  Defense in depth would be to canonicalise at the binder too, but the trigger-engine gate is sufficient.  LOW items (cron/timezone/etc validation, secret logging, poll_interval bounds) → sitting 13.

### Skipped (deferred for the bundle)

| Finding | File | Severity | Reason |
|---|---|---|---|
| `aiCallTaskExecutor.{h,cpp}` | (this sitting was supposed to fold it in if shallow) | cyber 1H + safety 2H + various MEDs | At 1796 lines this file is bigger than `polarionClient` (sitting 9) and almost as big as `workflowRuntimeManager`.  Bundling would have busted the sitting's scope.  Promoted to its own dedicated slot — either sitting 12 (parser cluster gets folded around it) or a new sitting 11b before sitting 12.  See "Open boundary at sitting end" below for routing decision. |
| MEDIUM logging-context items across all six files | various | safety/cyber MEDIUM | Cross-cutting — bundle into sitting 13's logger horizontal sweep. |
| LOW path / cron / event-name validation | workflowTriggerBinder | cyber LOW | Sitting 13 input-validation sweep. |
| LOW unbounded child-workflow depth | subWorkflowTaskExecutor | cyber LOW | Cross-ref comment to runtime layer; defer to a dedicated depth/cycle-detection design. |

### What's verified

| Step | Result |
|---|---|
| Studio Debug + Release builds | clean |
| Hamburg webhook trigger still registers post-fix (`secret=<set>`) | confirmed via startup log |
| 28-test assistant non-AI suite | 28/28 in 2.1 s |
| `ai-zip-demo` (exercises shell `zip_responses` task; `subWorkflowTaskExecutor` and `MaterializeFiles` paths not directly hit by this workflow but compile-clean) | succeeded in 12 s |
| `cyber2` after clean (Python task — **exercises the new `ValidateFileInputsExist` `ConfineUnderProjectRoot` gate live** for `taskDefinition.m_FileInputs[0]` = OpenSSH_2k.log) | succeeded in 55 s |
| `postgresDemo` (regression on sitting 9 dbQuery) | succeeded in 2 s |
| Zero `[error]` / `[critical]` lines since restart | confirmed |

What's not directly verified:
- **Hostile-path rejection branches** in all 5 cyber-HIGH gates (subWorkflowTaskExecutor / taskExecutorRegistry / pythonTaskExecutor / workflowFileIndex).  Each is a simple `if (containment-fails) return error` early exit; pattern matches sittings 1/3/6/7/8/9 verified rejection branches.  Code review only; bundles into the "Negative-path verification fixtures" entry in `todo.md`.
- **Webhook secret-missing rejection** in workflowTriggerBinder.  Would need a JCWF with `webhook` trigger and `params: {}` (no secret) to verify the binder layer skips registration cleanly.  No existing test JCWF; could fold into the negative-path fixture work.

### Open boundary at sitting end

**Sitting 12 plan** — bumps from "parser cluster" to "**parser cluster + `aiCallTaskExecutor`**".  Folding aiCallTaskExecutor into sitting 12 keeps the sitting count at 13 total (no new slot).  The parser cluster originally proposed for sitting 12 was: `workflowJsonParser`, `aiTranscript`, `filterManifest`, `scriptCatalog`, `schemaValidator`, `replyParserAPI1` (6 files).  Adding aiCallTaskExecutor brings sitting 12 to 7 files but the parser-cluster files are typically small (sub-300 lines each except workflowJsonParser); aiCallTaskExecutor is the heavyweight at 1796 lines.  Sitting 12 will likely run hot; if too big, split into 12a (parser cluster) and 12b (aiCallTaskExecutor) — the natural escape hatch.

Plus the `RewriteWorkflowId` simdjson rewrite from sitting 8 deferral lands in sitting 12 alongside the parser work (same simdjson surface).

Carryover for sitting 13: dashboard WS reconnect / lockout interaction.  All horizontal sweeps (App::g_App null-deref, logger context, log-injection sanitisation, `[[nodiscard]]`, audit republish).  Open `todo.md` items unchanged.

---

