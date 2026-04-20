# AI Dispatch Refactor — Exploration

**Status:** exploration / options, not a dev plan yet.
**Audience:** JC + Claude, for discussion.
**Convert to dev plan:** after tomorrow's settle-up.

This document explores how to refactor j9t's AI call path so it becomes a **typed, validated, retry-aware dispatch layer** on top of the existing HTTP/2 + multithreaded transport. The transport layer itself (libcurl multi, HTTP/2 multiplexing, rate limits, 429 backoff) is **not** in scope — that is already good and we keep it untouched.

Terminology note: **we deliberately do not use the word "agent"** for the new abstraction. JarvisAgent *is* the agent. One call is one call.

---

## Executive summary

**What we're fixing.** An `ai_call` today is an opaque text-in / text-out operation with a brittle file-drop handoff between the JCWF runtime and the core AI dispatcher. Failure modes that JC has felt in practice:

- **Silent-fail on missing environment.** `sessionManager.cpp:742-747` aborts the `Environment::Assemble` step if any of STNG / CNTX / TASK is empty — no warning, no log, no error. The request never dispatches; `AiRequestPool` eventually times out; the operator has no clue what went wrong.
- **Prompt-hope output contract.** Downstream tasks consume `.output.txt` under the assumption that "the model probably wrote something parseable." A single malformed character breaks a 60-item aggregation at the wrong layer.
- **No determinism seam.** No standard place to pin `temperature=0`, `seed`, or log `system_fingerprint` drift per call.
- **No replay / audit.** After-the-fact, there's no typed record of what got sent or returned.

**What changes.**
1. **Typed `AiInvocation` envelope** replaces the opaque STNG+CNTX+TASK+PROB blob at the dispatch boundary. Missing-field errors surface at JCWF validation time, not as watchdog timeouts at runtime. Single loud warning/error point. The orchestrator calls `AiRequestPool::Submit(envelope)` directly — the file watcher is removed from the runtime-dispatch path (§4).
2. **Schema-enforced structured output** is the default, with bounded validation-retry. Downstream tasks receive a `.output.json` (not prayer-parsed text) when the task declares `output_schema`; free-text tasks (code, Makefiles, prose) opt in to the legacy `.output.txt`.
3. **`IRequestBuilder` + extended `ReplyParser`** make provider plumbing symmetric. API4 (Anthropic `/v1/messages`) is added to the existing API1/2/3 set.
4. **Determinism defaults** (`temperature`, `seed`, `system_fingerprint` logging) + **`.transcript.json`** per call give us replay, audit, and drift detection.
5. **`TestInterface`** makes JCWF integration tests free of network flakiness and token cost.

**What stays unchanged.**
- **HTTP/2 multiplexing and the libcurl multi transport.** No perf regression — the envelope serializes once at dispatch time.
- **Multithreaded dispatch** via the existing thread pool and `CurlMultiDispatcher` instances.
- **Async completion model.** The envelope is only the *input shape* of `AiRequestPool::Submit`; the completion-queue mechanism, event loop, watchdogs, and callback-based delivery are preserved bit-for-bit. No blocking waits are introduced anywhere.
- **Disk-first philosophy.** All inputs, outputs, transcripts, and intermediates continue to live on disk.
- **Queue-folder convention.** One TASK / one STNG / one CNTX / many PROB per `ai_call` task folder, with fan-out producing one dispatch per PROB.

**Stability wins, stated plainly.** The current "orchestrator ↔ core dispatcher" seam silently fails when any queue file is empty; the new seam is a typed struct that either validates or produces an explicit, logged error — before anything touches the network. Missing-context cases (CNTX absent, or ambiguously empty) get graded responses: strict failure for required fields, explicit warnings for recommended-but-absent context ("no CNTX provided — results may be less precise").

**Out of scope** (tracked elsewhere): native LLM tool-calling (`JarvisAgent TODO List.md §5e`), Claude Code / tool-of-tools PoC (`§5f`), sub-workflow task-type additions.

---

## 1. Goals

1. **Typed request envelope** above `AiRequestPool::Submit` — the JCWF `ai_call` task produces a structured object, not opaque JSON.
2. **Structured / schema-enforced output** as a first-class feature, not a prompt-hope convention. (We already half-use this — see `example/workflows/aiCarMaintenancePipeline.md` where the classify step is instructed to emit exactly one word from `{engine, tires, rephrase}`.)
3. **Bounded validation retry** — if the reply fails output-schema validation, feed the error back and retry N times before failing the task.
4. **Determinism defaults** — `temperature`, `seed`, provider fingerprint logged per call.
5. **Transcripts on disk** — typed message list per `ai_call` task, for replay and audit.
6. **Test interface** — a non-network reply source for JCWF integration tests.
7. **Keep the fastest, most efficient AI dispatch engine** — concretely:
   - HTTP/2 multiplexing stays (many concurrent requests over one connection per provider).
   - Multithreaded dispatch stays (thread pool → multiple `CurlMultiDispatcher` instances across hosts).
   - The new envelope is pure struct → serialized once at dispatch time. Zero hot-path cost beyond what we do today.
8. **Direct dispatch** — orchestrator → `AiRequestPool::Submit` is a direct call. The file watcher is removed from the runtime-initiated dispatch seam. See §4.

Non-goals (for this refactor):

- Native LLM tool-calling (Assistant + post-1.0 JCWF `ai_call`) — tracked separately in `JarvisAgent TODO List.md` §5e.
- Adding new providers (covered in §5 — low priority).
- Streaming / event subscriptions (j9t already exposes run events at the workflow layer).

---

## 2. Current state (anchor)

Files that define the current dispatch stack:

| Layer | File | Role |
|---|---|---|
| Task executor | `application/workflow/aiCallTaskExecutor.{h,cpp}` | Resolves `{{template}}` vars, writes STNG/CNTX/TASK/PROB queue files. |
| Request pool | `application/workflow/aiRequestPool.{h,cpp}` | Tracks pending requests by handle + expected-output-path; watchdogs; completion queue. |
| Session assembly | `application/session/…` | Assembles STNG+CNTX+TASK+PROB into the HTTP body. |
| Reply parse | `application/json/replyParser.h` + `replyParserAPI1.h` + `replyParserAPI2.h` | Provider-specific JSON → text content + error info. |
| Transport | libcurl multi (vendored) | HTTP/2 multiplexing, retries, rate limit — **out of scope**. |

Shape of a call today (summarized from `aiRequestPool.h` and `aiCallTaskExecutor.h`):

```
JCWF ai_call task
  → AiCallTaskExecutor (template resolve, write STNG/CNTX/TASK/PROB files)
  → FileWatcher → FileAddedEvent → categorizer → SessionManager
  → AiRequestPool tracks the pending request
  → CurlMultiDispatcher (HTTP/2, fan-out, retries)
  → response body parsed by ReplyParserAPI1 or API2
  → written to .output.txt
  → downstream tasks consume opaque text
```

The contract between an AI step and its consumer today is *"the model probably wrote something parseable."* That is the gap.

What was labeled "Option E" in `JarvisAgent TODO List.md:282-287` (post-1.0) — replacing the file-watcher round trip for runtime-initiated calls with a direct `AiCallTaskExecutor → AiRequestPool::Submit` path — is pulled into this refactor. The envelope is the natural input type for that `Submit`; see §4 for the transport model.

---

## 3. Naming

The C++ type for "one AI call" is **`AiInvocation`** — no collision with `AiCallTaskExecutor` or JCWF `ai_call`, reads naturally as `IAiInvocation` if we later want a virtual. Shape:

```cpp
namespace AIAssistant
{
    struct AiInvocation          // typed envelope (data)
    {
        std::string m_InterfaceName;     // resolves to config.json ApiInterface
        std::optional<std::string> m_ModelOverride;
        AiSettings m_Settings;           // temperature, seed, max_tokens, ...
        std::vector<Message> m_Messages; // typed: System | User | Assistant
        std::optional<JsonSchema> m_OutputSchema;
        std::chrono::milliseconds m_Timeout;
        RetryPolicy m_Retry;

        // Transport — see §4
        std::filesystem::path m_QueueFolder;   // dispatcher reads STNG/CNTX/TASK/PROB here
        std::string m_ProbName;                // which PROB this invocation targets (fan-out)
    };

    struct AiReply               // typed result (data)
    {
        enum class Kind { Text, Structured, Error };
        Kind m_Kind;
        std::string m_Text;              // when Kind == Text
        simdjson::dom::element m_Json;   // when Kind == Structured (or store as string)
        AiError m_Error;                 // when Kind == Error
        AiUsage m_Usage;                 // tokens, cost estimate
        std::string m_SystemFingerprint; // drift detection
    };
}
```

Data-struct vs. `IAiInvocation` abstract base is still open for tomorrow (§9.A). The name itself is fixed.

---

## 4. Runtime transport — request + reply paths

Closes the orchestrator ↔ dispatcher seam in both directions. Today the request path depends on `FileWatcher` noticing queue files as they land on disk, and the reply path depends on it noticing the model's `.output.txt`. After the refactor, both are direct in-process signals; the event system is used only for observability (§4.6), never for correctness.

This section defines what each signal looks like, what guarantees it carries, how `output_schema` scope maps onto the queue-folder convention, and which failure modes collapse as a result.

### 4.1 Decision: direct dispatch for runtime-initiated calls

`AiCallTaskExecutor` still writes STNG/CNTX/TASK/PROB files to the task's queue folder on disk (disk-first philosophy preserved — every input is reconstructible, every call is replayable). But instead of relying on `FileWatcher → FileAddedEvent → categorizer → SessionManager → AiRequestPool`, the executor now:

1. Writes the files to disk.
2. Constructs one `AiInvocation` per PROB (fan-out replicates the envelope with a different `m_ProbName`, sharing everything else).
3. Signals `AiRequestPool` directly — `AiRequestPool::Submit(envelope)` or an equivalent in-process event post; exact mechanism is a dev-plan detail.

The **queue-folder `FileWatcher` is retired entirely** — no runtime-dispatch use, no external-drop use. `jarvisAgent.cpp:161-162` (`m_FileWatcher = std::make_unique<FileWatcher>(absoluteQueuePath, …)`) goes away, along with the `FileAddedEvent` dispatch path in `sessionManager.cpp:227-228`. The `FileWatcher` class itself stays — `m_ScriptFileWatcher` (live script registry) and the file-watch triggers in `triggerEngine.h` are independent uses with different source folders and continue unchanged.

### 4.2 Dispatch contract: "if it's on disk and the orchestrator says send, send it"

The dispatcher's preconditions on `Submit`:

- **Input reading.** Dispatcher reads STNG + CNTX + TASK + the named PROB from `m_QueueFolder`, concatenates for the provider HTTP body via `IRequestBuilder` (§5.2), and fires.
- **No enforcement that all four categories are present.** If the combined body is non-empty and the orchestrator submitted the envelope, the call dispatches. The silent-abort path in `sessionManager.cpp:742-747` goes away.
- **Missing-but-expected categories** are checked at envelope *construction* time in `AiCallTaskExecutor` and surface as explicit warnings/errors — logged, with task name + folder path — not as watchdog timeouts 60 s later.
- **Unit of work.** One `ai_call` task = one queue folder = N PROBs = N envelopes = N dispatches. The envelope is the unit of work per PROB; fan-out produces multiple envelopes that share the prompt template, interface, schema, and retry policy.

### 4.3 Schema scope: declared once on the task, applied to every PROB

The queue-folder convention — one TASK / one STNG / one CNTX / many PROB — is preserved and made central to the structured-output feature:

- `output_schema` is declared **once on the `ai_call` task**, not per-PROB.
- Every PROB in the folder is dispatched with that same schema attached.
- Each PROB produces its own output file — `<prob>.output.json` when schema is present, `<prob>.output.txt` when absent — alongside its own `<prob>.transcript.json` (§7).
- `output_retries` is **per-PROB** — one malformed reply does not starve the retry budget of its siblings.

This matches the meaning of the `ai_call` task: *"run this prompt template with this expected output shape over these N inputs."* The shape is declared once; fan-out replicates it N times.

### 4.4 Failure modes closed

| Old symptom | Root cause | New behavior |
|---|---|---|
| Request never dispatched, watchdog timeout after 60 s | `Environment::Assemble` skipped at `sessionManager.cpp:742-747` when any queue file empty | Envelope construction raises a logged error before `Submit`; missing categories produce graded responses (required vs. recommended-but-absent) |
| Malformed response crashes downstream task | `.output.txt` prompt-hope | Schema validation + bounded retry (§6); downstream task reads typed `.output.json` |
| No way to tell why a specific call vanished | File-watcher indirection, no per-call correlation | Envelope carries `m_QueueFolder` + `m_ProbName`; logs, transcripts, and errors are addressable by that pair |

### 4.5 External-drop path — DECIDED: retired

No external "drop files into /queue" path is kept. Webhook triggers and HTTP ingress already cover external-producer use cases cleanly; maintaining a second ingestion route would be code for code's sake. The queue folder is an *internal* runtime artifact — only `AiCallTaskExecutor` writes it, only `AiRequestPool` reads it, both in-process.

Knock-on cleanups this unlocks:
- `sessionManager.cpp` loses the `FileAddedEvent` dispatcher and the `Environment::Assemble` silent-abort branch at `:742-747`.
- `jarvisAgent.cpp:161-162` (queue-folder `FileWatcher` construction) and the matching shutdown code at `:800-867` go away for the queue path.
- The file categorizer for STNG/CNTX/TASK/PROB becomes a pure in-memory helper called from `AiCallTaskExecutor`, not a file-event consumer.

### 4.6 Reply-side transport: dispatcher → runtime

Symmetric with the request side (§4.1). `CurlMultiDispatcher` completes an HTTP call on a worker thread and signals `AiRequestPool` directly — no event-queue round trip, no file-event trigger. The correctness path and the observability path are split deliberately.

**Correctness path — direct, in-process:**

```
CurlMultiDispatcher (worker thread, HTTP completes)
    → AiRequestPool::OnReply(handle, AiReply)       // direct call, thread-safe
        → schema validation + retry loop (§6) if m_OutputSchema set
        → writes <prob>.output.{json|txt} to m_QueueFolder
        → writes <prob>.transcript.json  to m_QueueFolder
        → enqueues AiRequestCompletion on m_Completed
Workflow runtime tick:
    AiRequestPool::TryPopCompletion()               // polls, as today
```

`AiRequestPool`'s existing non-blocking completion queue (`TryPopCompletion`, already consumed by the tick-based workflow runtime) is preserved bit-for-bit. The only thing that changes on the reply side is the *input edge*: replies enter via `OnReply(handle, AiReply)` instead of via `OnOutputFileCreated(path)` triggered by a file event. The legacy `OnProbFileEvent` / `OnOutputFileCreated` entry points in `aiRequestPool.h:112-116` go away along with the queue-folder `FileWatcher`.

**Observability path — event system, fire-and-forget:**

Lightweight lifecycle events posted onto the global `EventQueue` for consumers that want to know but are not load-bearing:

| Event | Posted when | Payload (minimum) |
|---|---|---|
| `AiCallStartedEvent` | Envelope accepted by `Submit` | task id, prob name, interface |
| `AiCallRetryingEvent` | Schema / HTTP retry fires | task id, attempt #, last error |
| `AiCallCompletedEvent` | Reply validated + written | task id, usage, system fingerprint |
| `AiCallFailedEvent` | Retries exhausted or hard error | task id, error kind, last message |

New event category `EventCategoryAi` (or fold under the existing `EventCategoryApp` — §9.H). Consumers:

- TUI status row — live "running / retrying / done" per task, replaces the debug-signals polling we fall back to today.
- Dashboard WebSocket stream — real-time task cards.
- Python scripts, via `BuildEventDict` (matches how `FileAddedEvent` is bridged today).
- Future: Tracy zones, audit-log exporters, the `/api/debug/signals` endpoint.

Events are **fire-and-forget**: delivery is best-effort, consumers can come and go, a slow consumer never stalls `AiRequestPool`. Correctness never depends on a subscriber being attached.

**Why split this way.** The event system is explicitly described in `engine/event/event_system.md` as a *notification* mechanism. Driving correctness through it would couple reply latency to the main-loop tick rate and re-create the roundabout dispatcher ↔ FS-event pattern this refactor removes. `PythonCrashedEvent` is the healthy precedent — a notification, not a state-machine signal. `FileAddedEvent` was the one case where an event drove business logic, and eliminating that coupling is the point of §4.

---

## 5. Provider surface (ReplyParser + its symmetric twin)

### 5.1 ReplyParser today

Already abstract — `application/json/replyParser.h:29`:

```cpp
class ReplyParser
{
public:
    virtual size_t HasContent() const = 0;
    virtual std::string GetContent(size_t index = 0) const = 0;
    static std::unique_ptr<ReplyParser> Create(InterfaceType const&, std::string const& json);
};
```

Three concrete implementations (dispatched from `replyParser.cpp:35-64`):
- `ReplyParserAPI1` — OpenAI chat.completions (GPT-4-family), `choices[].message.content`.
- `ReplyParserAPI2` — OpenAI Responses API (GPT-5-family), `output[].content[].text`, plus reasoning blocks.
- `ReplyParserAPI3` — Google Gemini native, `candidates[].content.parts[].text`, `finishReason`, `usageMetadata`.

**Answer to JC's question: we do not need to introduce an abstract base — we already have one.** But we probably need to **extend** it for the refactor. Concretely, additions would be:

| New virtual method | Why |
|---|---|
| `ErrorInfo const& GetError() const` | Base-class way to get error details (today only concrete classes expose `GetErrorInfo`/`GetErrorType`). |
| `AiUsage GetUsage() const` | Token accounting without knowing the provider. |
| `std::string GetFinishReason() const` | `"stop"`, `"length"`, etc. Needed for retry decisions and schema-enforcement. |
| `std::string GetSystemFingerprint() const` | Drift detection (OpenAI only; others return empty). |
| `std::optional<simdjson::dom::element> GetStructuredOutput() const` | When the reply used native json_schema mode or the forced-tool shim. |

These are **additive** — the existing `HasContent()`/`GetContent()` stays. Backwards-compatible.

### 5.2 Symmetric `IRequestBuilder` — new?

Today, the provider-specific HTTP request *body* is not behind a polymorphic interface. Assembly happens in SessionManager (+ the STNG/CNTX/TASK file concatenation). For the refactor, we'd want an `IRequestBuilder` symmetric to `ReplyParser`:

```cpp
class IRequestBuilder
{
public:
    virtual std::string BuildBody(AiInvocation const&) const = 0;
    virtual std::string GetEndpointPath() const = 0;           // /v1/chat/completions vs /v1/responses
    virtual std::unordered_map<std::string,std::string> GetExtraHeaders() const = 0;
    static std::unique_ptr<IRequestBuilder> Create(InterfaceType const&);
};
```

With `RequestBuilderAPI1` (OpenAI chat.completions), `RequestBuilderAPI2` (OpenAI Responses), `RequestBuilderAPI3` (Gemini `generateContent`), and `RequestBuilderAPI4` (Anthropic `/v1/messages`). These mirror the four `ReplyParserAPI*` classes after this refactor.

Net shape post-refactor:

```
AiInvocation (envelope)
    ↓  IRequestBuilder (envelope → provider JSON body)
    ↓  CurlMultiDispatcher (HTTP/2, unchanged)
    ↑  ReplyParser (provider JSON → AiReply)
AiReply (typed result)
```

Pleasingly symmetric. `IRequestBuilder` + extended `ReplyParser` are the only provider-dependent code; everything else is generic.

### 5.3 Providers in scope for this refactor

Existing (three reply parsers, two vendors):

- **API1** — OpenAI chat.completions (GPT-4-family).
- **API2** — OpenAI Responses API (GPT-5-family).
- **API3** — Google Gemini native (`generateContent`).

Added by this refactor:

- **API4 — Anthropic `/v1/messages`** (Claude Sonnet / Opus / Haiku). Structured output via the forced-tool shim (Anthropic has no `response_format: json_schema`). No `seed` parameter. Usage fields differ (`input_tokens` / `output_tokens`). Requires `RequestBuilderAPI4` + `ReplyParserAPI4`.

Anything **OpenAI-compatible** (xAI, Groq, Cerebras, OpenRouter, Ollama, LM Studio, local llama.cpp) works through `RequestBuilderAPI1` with base-URL / auth overrides — no new classes needed.

---

## 6. Structured output — the biggest lever

This is where the refactor pays for itself. Three modes, all worth supporting:

| Mode | How | Providers |
|---|---|---|
| **Native JSON-schema** | `response_format: {type:"json_schema", schema}` in the request. | OpenAI (chat.completions + Responses), Gemini `responseSchema`, Groq. |
| **Forced-tool shim** | Define a single tool called `output` with the schema; force `tool_choice: {type:"tool", name:"output"}`. | Anthropic; older OpenAI; fallback for anything that supports tools but not json_schema. |
| **Prompted + validate** | Instruct the model in the system prompt to emit JSON matching the schema; validate locally. | Universal fallback (fragile). |

**Validation flow** (identical regardless of mode):

1. Response arrives.
2. If `output_schema` is set on the `AiInvocation`, validate the parsed content against it.
3. On validation failure, append a new `User` message: *"Your previous response failed schema validation: warning/error messages from validator <error>. Please correct it and try again."* and re-dispatch.
4. Counts toward a separate `output_retries` budget (default 3), not the HTTP retry budget.
5. After exhaustion, the task fails with a structured `AiError{kind=SchemaValidation, lastErrors=...}`.

**Validator — two-step, both on simdjson:**

1. **Parse** the reply body with **simdjson** — catches malformed JSON up front (truncated output, stray prose outside the structured block, unescaped quotes). Parse failure → retry with hint *"Your previous response was not valid JSON: <simdjson error>. Emit only JSON matching the requested schema."*
2. **Schema-validate** the parsed simdjson DOM with our own Draft-7 subset validator. Schema mismatch → retry with hint *"Your previous response failed schema validation: <validator errors>. Please correct it and try again."* (§6 step 3 above).

Schema validation is a *different* operation from parsing — simdjson parses, our validator walks the resulting DOM. **No new parser dependency is introduced; simdjson remains the only JSON parser in the project.** Target dialect: **JSON Schema Draft 2020-12** (matches the existing JCWF schema in the spec). The subset we need to support:

- `type` (string / number / integer / boolean / object / array / null)
- `properties` + `required` + `additionalProperties` (objects)
- `items` (arrays)
- `enum` (covers the single-word-classification case in `aiCarMaintenancePipeline`)
- `minimum` / `maximum` / `minLength` / `maxLength` / `pattern` (optional)
- `oneOf` / `anyOf` (for union outputs; JCWF schema uses `anyOf` for `doc` and `queue_file_ref`)
- **`$ref` + `$defs`** (the JCWF schema relies on this for `trigger`, `task`, `dataflow`, `queue_file_ref`, `control_node`)

Estimated ~300–500 lines of C++ against simdjson's DOM (higher than before because of `$ref`/`$defs` resolution). Lives under `application/json/` next to the reply parsers. Reusable for the editor-side JCWF schema validation (§6.6), too.

**JCWF surface change**: one new optional field on `ai_call` tasks, declared at the task level and applied to every PROB in the queue folder (see §4.3):

```json
"ai_call": {
  "interface": "gpt-5-nano",
  "output_schema": { "type": "string", "enum": ["engine", "tires", "rephrase"] },
  "output_retries": 3,
  ...
}
```

`example/workflows/aiCarMaintenancePipeline.md` (the classify step) becomes a one-line change to use this, and the downstream `buildManual` internal task can trust the classification instead of hoping.

### 6.4 Output file contract — `.output.json` vs `.output.txt`

Structured output becomes the **default** for new `ai_call` tasks; free-text is the explicit opt-out for code / Makefiles / prose. The file written to disk mirrors this:

| `ai_call` declares | Response validated against | Output file (per PROB — see §4.3) |
|---|---|---|
| `output_schema: {…}` | The schema, with retry on failure | `<prob>.output.json` |
| nothing (implicit free-text) | — | `<prob>.output.txt` (today's behavior, unchanged) |

JCWF dataflow wiring accordingly:
- `{{tasks.classify.output}}` → opaque handle; resolves to whichever sibling exists.
- `{{tasks.classify.output.category}}` → only valid when the upstream produced JSON; the workflow validator rejects this reference at load time if the upstream task has no `output_schema`.

Consequence: the **workflow validator** needs one new rule — structured-field references are only valid against tasks declaring an `output_schema`. Catches wiring mistakes at load time, before the run.

### 6.5 Default stance — "JSON unless told otherwise"

Structured output is an `AiInvocation` field, not a global flag, but we should pick a **policy for where the refactor sets it by default**. Proposed convention:

| Caller | Default output mode | Rationale |
|---|---|---|
| JCWF `ai_call` tasks | **Free text** (current behavior, backwards compatible) | Opt-in per task via `output_schema:` field. Workflows that want structure declare it; workflows that don't keep working. |
| `AiJcwfService::GenerateAsync` (editor AI → new JCWF) | **JSON with JCWF schema** | The output *is* JCWF JSON. Today we prompt-hope. Tomorrow we validate against the JCWF schema and retry. See §6.6. |
| `AiJcwfService::ExplainAsync` (editor AI → prose) | **Free text** | Human-readable explanation; structure would hurt. |
| `AiJcwfService::FixFailedScriptAsync` (editor AI → code) | **Free text** | Output is a Makefile / C++ / Python / shell script. Schema-enforcing JSON would fight the goal. |
| `AiJcwfService::TestAiInterface` | **Free text** | Smoke test; no structure needed. |
| Assistant chat | **Free text** | Conversation. Tool-calling surface is tracked separately (`JarvisAgent TODO List.md §5e`). |

So the rule of thumb is: **code/prose → free text; everything we parse programmatically → JSON + schema**. The envelope defaults to free text, and each call site declares a schema when parsing is downstream.

### 6.6 Editor AI generation — `AiJcwfService::GenerateAsync`

`application/web/aiJcwfService.h:66-67`:

```cpp
void GenerateAsync(std::string const& prompt, std::string const& currentJcwfJson);
```

Today: system prompt tells the model to emit JCWF JSON, we parse the response, and if the model strays (wrapped code fences, explanatory prose before/after, missing fields) we surface the error after the round trip. `doc/jcwf_generation_guide.md` is loaded as context to steer it.

With structured output:

1. **Single source of truth: `doc/jcwf.schema.json`.** A Draft 2020-12 schema already exists embedded inside `doc/JC_Workflow_Specification.md:2215-~2490` (§9 of the spec), labeled *"simplified … not exhaustive but suitable for validation and editor tooling"*. Three actions as part of this refactor:
   - **Extract** the embedded schema to `doc/jcwf.schema.json` as a standalone file. Replace §9 of the spec with a one-line reference to the extracted file. No duplication.
   - **Bring the schema up to speed.** The embedded draft is behind current JCWF reality — for example, its `task.type` enum is `["python", "shell", "ai_call", "internal"]`, missing `sub_workflow` (your next major feature), and it lacks the new `output_schema` / `output_retries` fields introduced by this refactor. Other gaps exist wherever `workflowJsonParser` / `workflowValidator` accept fields the schema does not describe. Dedicated pass required: diff the schema against the parser, close the gaps, and add a contract test that asserts every field the parser reads is declared in the schema (so the schema stays current).
   - **Compile the schema into the binary.** Premake gains a prebuild step that reads `doc/jcwf.schema.json` and writes `application/json/jcwfSchema.generated.h` with:
     ```cpp
     namespace AIAssistant
     {
         inline constexpr char const* kJcwfSchemaJson = R"JSON( …schema… )JSON";
     }
     ```
     The generated header is gitignored and regenerated automatically whenever the `.json` changes. C++ consumers `#include` the header; no runtime file lookup, no packaging risk, no silent fallback. `doc/jcwf.schema.json` stays the one authored source — humans read it there; the generator derives the header from it (same relationship as a `.cpp` to its `.o`). Works in C++20.
   - **Apply the same compile-time embed to `doc/jcwf_generation_guide.md`.** Today `AiJcwfService::LoadGenerationGuide` (`application/web/aiJcwfService.cpp:1114`) loads the generation guide from disk with a multi-path search and falls back to a placeholder string + `LOG_APP_WARN` if not found — the same silent-degradation pattern that affects the schema. Same fix: extend the Premake prebuild step to also generate `application/json/jcwfGenerationGuide.generated.h` with `inline constexpr char const* kJcwfGenerationGuide = R"MD( …guide… )MD";`. `AiJcwfService` reads the constant, removes the file-search + fallback. One loaded artifact, no packaging risk.
2. `GenerateAsync` sets `AiInvocation.m_OutputSchema = kJcwfSchemaJson` directly from the compiled-in constant.
3. Dispatcher requests schema-enforced output from the provider (OpenAI `response_format: {type:"json_schema"}`, Gemini `responseSchema`; forced-tool shim for Anthropic).
4. On validation failure, the existing retry path (§6) kicks in: send the validator errors back as a follow-up user message. This is strictly better than what we do today, which is "re-prompt blind."
5. Downstream `WorkflowValidator` still runs (schema says "these fields exist with these types"; validator says "these references resolve, the DAG has no cycles, freshness is coherent"). Both layers useful.

**Ripple on `AiJcwfService::ExplainAsync` and `FixFailedScriptAsync`:** unchanged. Both stay free-text. The *mechanism* they use (queue files → `AiRequestPool`) gets the envelope refactor but doesn't use the `output_schema` field.

**Ripple on the "multi-stage pipeline" (decompose → generate → review):** each stage becomes its own `AiInvocation` with its own schema. Decompose → `{tasks: [...]}` schema. Generate → full JCWF schema. Review → `{issues: [...], severity: ...}` schema. The pipeline becomes a chain of typed transforms instead of a chain of string parsers.

---

## 7. Determinism + transcript

### 7.1 Determinism defaults in `EngineConfig`

```cpp
struct DeterminismDefaults
{
    double m_Temperature = 0.0;
    std::optional<int64_t> m_Seed;            // where provider supports it
    bool m_RecordSystemFingerprint = true;    // OpenAI only; others log empty
};
```

Per-task overrides in JCWF stay supported. The defaults nudge the whole engine toward reproducibility.

### 7.2 `.transcript.json` next to `.output.txt`

Same task folder that holds `PROB_*_*.txt`, `STNG_*.txt`, `answer.output.txt` also gets `answer.transcript.json`:

```json
[
  {"kind":"request","parts":[{"role":"system","content":"..."},{"role":"user","content":"..."}],"settings":{"temperature":0,"seed":42},"timestamp":"..."},
  {"kind":"response","parts":[{"role":"assistant","content":"engine"}],"usage":{"in":55,"out":1,"total":56},"system_fingerprint":"fp_abc123","timestamp":"..."}
]
```

Two wins: replay (re-run any task with the exact prior context) and audit (every AI decision reconstructible from disk). Consistent with our disk-first philosophy.

---

## 8. Test interface

`TestInterface` as a new `InterfaceType` alongside `API1`/`API2`. Backed by a simple map or callback:

- Canned mode: fixture maps `(interface, model, hash(messages)) → AiReply`. Deterministic, no network.
- Schema-driven mode: if `AiInvocation.m_OutputSchema` is set, auto-generate a minimal valid reply against the schema (for tests that only care about *shape*).

Enables every JCWF integration test to run with zero API tokens and zero flakiness. Opt-in per test via config override.

---

## 9. Options & open questions — bring to tomorrow's settle-up

### A. Polymorphism shape
1. Pure data struct `AiInvocation` + interfaces at `IRequestBuilder`/`ReplyParser` only (simpler).
2. `IAiInvocation` abstract + concrete subclasses (opens room for `MultiTurnAiInvocation` later).

### B. `IRequestBuilder` — introduce now or later?
1. Now: symmetric with `ReplyParser`, cleaner layering, forces us to test the shape against Anthropic mentally.
2. Later: unblock structured output first, factor out request building as a follow-up.

### C. Extend `ReplyParser` base?
1. Add the virtuals listed in §5.1 to the base class now.
2. Keep the base minimal, add getters on concrete parsers, upcast at the call site.

### D. Structured-output mode priority
1. OpenAI `json_schema` native (easiest, covers both API1 and API2).
2. Forced-tool shim (universal fallback, required for API4 Anthropic).
3. Ship both at once, mode selected per-interface.

### E. Scope for 1.0
Claude's proposal (from yesterday's assessment):
1. (A) typed `AiInvocation` envelope
2. (C) output-schema enforcement
3. (D) determinism defaults + transcript log
4. (F) test interface
   — *ship as the 1.0 refactor*

Alternative: slice thinner. Just ship (A) + (C) + transcript; defer determinism defaults and test interface to 1.x. Open.

### F. Editor AI generation (§6.6) — schema-update scope
The schema draft already exists (§9 of the spec). This refactor will extract it to `doc/jcwf.schema.json` and wire `GenerateAsync` to schema-enforced output + retry. Open sub-question:

1. **Bring the schema fully up to speed in this refactor** (close all gaps vs. `workflowJsonParser`, add `sub_workflow`, add `output_schema` / `output_retries`, add contract test that schema ⊇ parser). One-time pass, bigger scope.
2. **Extract + minimal updates now, full gap-closing as a follow-up.** Ship the envelope with whatever the schema covers today + a TODO to expand. The editor AI will over-reject things the schema forgot, which is a soft failure (validator errors fed back → model fixes it).

Option 1 is cleaner but bigger; option 2 ships earlier. The `sub_workflow` addition probably pairs better with the sub-workflows feature work itself than with this refactor.

### G. Interaction with Option E — DECIDED

Option E (`JarvisAgent TODO List.md:282-287`) is pulled into this refactor. The queue-folder `FileWatcher` is retired entirely — no runtime-dispatch use, no external-drop use. `AiCallTaskExecutor → AiRequestPool::Submit(envelope)` is a direct in-process call. See §4 for the transport contract, `m_QueueFolder` / `m_ProbName` envelope fields, retired code paths, and failure-mode reductions.

The `FileWatcher` class itself survives for `m_ScriptFileWatcher` (live script registry) and file-watch triggers in `triggerEngine.h` — those are independent uses on different folders.

### H. AI lifecycle event category

Observability events (§4.6) need a category flag. Options:

1. Add a new `EventCategoryAi` to the bitfield in `engine/event/event.h:49-58`. Cleanest semantically; gives consumers a single-flag filter.
2. Fold `AiCall*Event` under the existing `EventCategoryApp`. No new category, matches the "app-level notification" precedent of `AppErrorEvent`; consumers filter by `EventType` instead.

Either works; option 1 is a one-line addition and opens the door to richer filtering if Python hooks grow.

---

## 10. Reading list (for tomorrow)

- `application/json/replyParser.h` / `replyParserAPI1.h` / `replyParserAPI2.h` / `replyParserAPI3.h` — existing abstraction to preserve / extend (three providers: OpenAI chat.completions, OpenAI Responses, Gemini native). API4 (Anthropic `/v1/messages`) is added by this refactor.
- `application/workflow/aiRequestPool.h` — the thing our envelope feeds into.
- `application/web/aiJcwfService.h` — editor-side AI operations (explain / generate / fix-script); see §6.6.
- `doc/jcwf_generation_guide.md` — currently steers the editor generator via prompt; becomes a complement to the JCWF JSON Schema once §6.6 lands.
- `doc/JC_Workflow_Specification.md:2215` (§9) — existing embedded JCWF JSON Schema draft (Draft 2020-12). To be extracted into `doc/jcwf.schema.json` as the single source of truth; spec §9 becomes a one-line reference to the extracted file.
- `JarvisAgent TODO List.md:282-287` — Option E context (pulled into scope by this refactor; see §4 and §9.G).
- `example/workflows/aiCarMaintenancePipeline.md` — real workflow that already wants structured output; good test target.

---

## 11. Next step

Tomorrow:
1. Settle items A–F in §9 (G is decided).
2. Convert this doc into a dev plan:
   - one section per chosen item, with file-level diffs sketched
   - ordered work breakdown
   - contract tests (Studio + Engine) to add
3. Open tracking entry under `application/workflow/doc/todo.md` "future refactors" + bump `JarvisAgent TODO List.md §5c` status.
