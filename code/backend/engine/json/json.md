# JarvisAgent Engine JSON Utilities

This document describes the JSON-related engine components used to load and validate `config.json`:

- `ConfigParser`
- `ConfigChecker`
- `JsonHelper`

All code lives under `code/backend/engine/json`.

---

## 1. config.json

JarvisAgent reads configuration from `config.json` at startup.  Fields not recognised by the parser are best-effort stringified and logged (top-level only), then ignored.

For the full field-by-field reference (every key, default value, valid range, semantic) see **`doc/jarvisagent.md` "Configuration"** — that's the canonical home.  This document covers the C++ parser internals (state machine, type-check semantics, helpers, validation pipeline) only.

---

## 2. ConfigParser

**Header:** `json/configParser.h`  
**Source:** `json/configParser.cpp`  
**Namespace:** `AIAssistant`

`ConfigParser` loads and validates the raw JSON configuration file (e.g. `config.json`) and fills a `ConfigParser::EngineConfig` struct.

### 2.1 EngineConfig

The full struct + nested `ApiInterface` / `RateLimit` / `RequestBudget` definitions live in `code/backend/engine/json/configParser.h` and grow as new providers / knobs land — see the header for the canonical shape.  The closed-set enum that the parser dispatches on:

```cpp
enum InterfaceType
{
    API1 = 0,    // OpenAI-compatible chat completions
    API2,        // OpenAI Responses API
    API3,        // Google Gemini native
    API4,        // Anthropic Messages
    Test,        // No-network fixture backend (integration tests)
    API5,        // AWS Bedrock (SigV4)
    API6,        // Azure OpenAI
    NumAPIs,     // sentinel — count of valid variants
    InvalidAPI   // sentinel — set by the parser on unknown "API" string
};
```

The string ↔ enum mapping is centralised in the file-local `kInterfaceTypeMappings` table (Section 2.5).  Adding a new variant without extending the table OR the enum→string `InterfaceTypeName()` switch fails to compile (table size is asserted; switch is exhaustive).

**Logged-only (not stored):** `"description"`, `"author"`.

**Type-checked but not stored:** `"file format identifier"` (must be a JSON number; presence is recorded for the format-info summary, value is not retained).

For the field-by-field config.json reference (every `"queue folder"` / `"max threads"` / `"port"` / `"api_file"` key, with defaults and ranges) see `doc/jarvisagent.md` "Configuration".  The mapping from JSON key → `EngineConfig` member is mechanical 1:1 in `Parse()`.  (AI interfaces live in the encrypted `API.json.enc`, not `config.json` — see §2.5.)

### 2.2 Parser State

```cpp
enum State
{
    Undefined = 0,
    ConfigOk,
    ParseFailure,
    FileNotFound,
    FileFormatFailure
};
```

### 2.3 Public API

```cpp
ConfigParser(std::string const& filepathAndFilename);
~ConfigParser();

State GetState() const;
State Parse(EngineConfig& engineConfig);
bool ConfigParsed() const;
```

### 2.4 Parse() behavior (current implementation)

`Parse(EngineConfig&)` does:

1. Resets `m_State` and resets `engineConfig = {}`.
2. Checks that the config path exists and is not a directory.
   - On failure: `FileNotFound`.
3. Loads + parses JSON via `simdjson::ondemand` with explicit error checks at every step (file load, document iterate, top-level object access).  Each failure mode emits a distinct `LOG_CORE_ERROR` and returns the matching `State`: `FileNotFound` for load errors, `ParseFailure` for malformed JSON, `FileFormatFailure` if the top-level value is not an object.
4. Iterates over top-level fields.  Each field-parse site is a single-line call into one of the file-local helpers in the anonymous namespace (`ParseStringField`, `ParseStringFieldLogOnly`, `ParseInt64Field` with a `NumericPolicy` enum, `ParseUint64Field`, `ParseBoolField`, `ParseInt64FieldWithBounds`).  Each helper uses simdjson's `.get<T>().get(target)` extractor and on type mismatch emits `LOG_CORE_ERROR("ConfigParser: '{}' must be a {string|number|non-negative number|boolean}", key)` and **returns without storing or counting** — a single malformed field does not abort the whole config load, so partially-valid configs still populate as much as they can.  The `NumericPolicy` enum captures the per-site post-extract checks (`AcceptAny`, `RejectNegative`, `ClampNegativeToZero`, `StoreOnlyIfPositive`) so negative-value handling stays declarative at the call site rather than open-coded.  `ParseInt64FieldWithBounds` handles closed-range fields (`port`, `session_timeout_hours`) with WARN-on-out-of-range and sane-default substitution.  `use_bash` stays inline (platform-conditional INFO suffix).  AI interfaces are **no longer parsed here** — they moved to the encrypted `API.json.enc` (see §2.5).
   - `description`, `author`, and other informational fields are logged via `ParseStringFieldLogOnly` (no storage target).
   - Unknown top-level fields: best-effort stringified and logged as `"key: value"` (or `"[complex type]"` for arrays/objects).
5. Sets state:
   - `ConfigOk` if `"queue folder"` **and** `"workflows folder"` both appeared at least once.  (Before AI interfaces moved out of `config.json`, this also required a `"url"` inside `"API interfaces"` — that gate is gone, since the routing table is no longer in `config.json`.)
   - Otherwise: `FileFormatFailure`.
6. Logs a "format info" summary of field occurrences.

**Important:** `ConfigOk` indicates successful parsing and minimal required presence checks. It does **not** guarantee semantic correctness (directory existence, valid API selection, etc.). Semantic validation is done by `ConfigChecker` (Section 3).

### 2.5 AI interfaces — moved out of config.json

AI interfaces, the default interface, and the JCWF interface are **no longer parsed by `ConfigParser`** — they moved into the master-password-encrypted `API.json.enc`, owned by `ApiInterfaceManager` (`code/backend/application/web/apiInterfaceManager.{h,cpp}`).  That manager holds the single interface (de)serialization + validation that the old `ParseInterfaces` ran: auto-name via `EngineConfig::GenerateInterfaceName`, `max_context_tokens` resolution, `is_mock`/`fixture_path` confinement via `ConfineUnderProjectRoot`, the legacy-`"Test"` rejection, and the `UrlPolicy` plaintext/SSRF gate.  The `InterfaceType` ↔ `"API1".."API6"` mapping now lives in `EngineConfig::InterfaceType{From,To}String` (single source of truth, `static_assert`-guarded on `NumAPIs == 6`).

At unlock, `WebServer::HydrateAiInterfaces` copies a snapshot into `engineConfig.m_ApiInterfaces` and resolves the stored default/jcwf **names** into `m_ApiIndex` / `m_JcwfAiInterfaceIndex`, so the dispatch read-sites are unchanged.  `config.json` keeps only the non-secret `api_file` path pointing at the store.  Per-interface field reference: `doc/jarvisagent.md` "AI interfaces".

---

## 3. ConfigChecker

**Header:** `json/configChecker.h`  
**Source:** `json/configChecker.cpp`  
**Namespace:** `AIAssistant`

`ConfigChecker` performs semantic validation and applies defaults on a previously parsed `EngineConfig`.

### 3.1 Public API

```cpp
class ConfigChecker
{
public:
    ConfigChecker() = default;
    ~ConfigChecker() = default;

    bool Check(ConfigParser::EngineConfig& engineConfig);
    bool ConfigIsOk() const;

private:
    bool m_ConfigIsOk{false};
};
```

### 3.2 Check() behavior (current implementation)

`Check(EngineConfig&)`:

1. Validates directories:
   - `EngineCore::IsDirectory(engineConfig.m_QueueFolderFilepath)` must be `true`.
   - `EngineCore::IsDirectory(engineConfig.m_WorkflowsFolderFilepath)` must be `true`.

2. Validates API selection:
   - `m_ApiInterfaces` must not be empty.
   - `engineConfig.m_ApiIndex` must be a valid in-bounds index — strictly `m_ApiIndex < m_ApiInterfaces.size()`.
   - For the selected interface:
     - URL must `starts_with("https://")` or `starts_with("http://")` (scheme prefix, not substring — a URL like `http://x/?next=https://y` is rejected).  Policy decisions on `http://` (loopback-only, never with `key_name`) live upstream in `UrlPolicy::ValidateAiInterfaceUrl`; by the time ConfigChecker runs, the entry has already been validated at parse time, so any `http://` URL still present is loopback-only.
     - Model must be non-empty.
     - Interface type must not be `InvalidAPI`.

3. If validation fails:
   - Logs `LOG_CORE_ERROR` for the failing component(s) (queue / workflows folder path). Operator-visible details land in `log/log.txt`.  (AI-interface validation moved to `ApiInterfaceManager`; ConfigChecker no longer checks the interface table — it is empty at config-load by design, hydrated only at unlock.)
   - Sets `engineConfig.m_ConfigValid = false`.

4. If validation succeeds:
   - Applies defaults for out-of-range values:
     - `m_MaxThreads`: if `<= 0` or `> 256` → set to `16`.
     - `m_MaxInflightAiCalls`: if `== 0` or `> 10000` → set to `1000`.
     - `m_PythonEngines`: if `== 0` or `> 16` → set to `4`.
     - `m_SleepDuration`: if `<= 0ms` or `> 256ms` → set to `10ms`.
     - `m_MaxFileSizekB`: if `<= 0` or `> 256` → set to `20`.
   - Sets `engineConfig.m_ConfigValid = true`.

---

## 4. JsonHelper

**Header:** `json/jsonHelper.h`
**Source:** `json/jsonHelper.cpp`
**Namespace:** `AIAssistant`

`JsonHelper` is the single canonical helper for escaping a `std::string` so its contents can be embedded inside a JSON string literal.  Output is RFC 8259 §7-compliant.

### 4.1 EscapeJsonString() — the canonical static API

```cpp
class JsonHelper
{
public:
    // RFC 8259 §7-compliant escape.  Static, no instance required.
    static std::string EscapeJsonString(std::string_view input);

    // Backwards-compatible instance alias — delegates to EscapeJsonString.
    std::string SanitizeForJson(std::string const& input) const;
};
```

**Behavior:**

- The four shorthand cases are emitted as their two-character escapes:
  - `"`  → `\"`
  - `\`  → `\\`
  - `\n` → `\n`
  - `\r` → `\r`
  - `\t` → `\t`
- Every other byte in `0x00–0x1F` is emitted as `\u00XX` (the form RFC 8259 §7 mandates for non-shorthand control characters).  This includes:
  - `\b` (0x08), `\f` (0x0C), and the full `0x01–0x07`, `0x0B`, `0x0E–0x1F` range.
  - Bytes that previously slipped through unescaped — see "Why this changed" below.
- Bytes `>= 0x20`, including UTF-8 continuation bytes, are passed through unchanged, so valid UTF-8 input remains valid UTF-8 output.

### 4.2 SanitizeForJson() — backwards-compatible instance method

The instance method is retained as a thin delegator for legacy call sites that construct the helper as `JsonHelper jh; jh.SanitizeForJson(x)` or `JsonHelper().SanitizeForJson(x)`.  Both are still legal and now produce RFC-compliant output for free.  New code should call `JsonHelper::EscapeJsonString(x)` directly.

### 4.3 Why this changed (2026-04-29)

The previous implementation only handled the five shorthand cases and passed every other control byte through unchanged.  The case-statement label for form-feed (`0x0C`) was a literal control character in the source file, which compiled as a `case '\f':` arm whose body simply `break`'d — silently *dropping* form-feed bytes.

That divergence broke RFC 8259 in two distinct ways:

1. Bytes `0x01–0x08`, `0x0B`, `0x0E–0x1F` produced output that downstream JSON parsers (including the project's own `simdjson` consumers) reject as malformed.
2. Form-feed bytes were silently elided, so a round-trip through `SanitizeForJson` lost data.

The rewrite consolidates four other broken `JsonEscape`-style copies that had spread across the project (`assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, plus the original `SanitizeForJson` itself).  All sites now route through `JsonHelper::EscapeJsonString` — including the last two that remained in the assistant subsystem post-2026-04-29 (`assistantTools.cpp`, `assistantController.cpp`), which were converged in the 2026-04-30 sweep.  No anon-namespace `JsonEscape` copies remain in the codebase.

### 4.4 Use sites

Every outbound AI request body, persisted transcript, and cloud-surface JSON splice flows through this helper:

- `code/backend/application/json/requestBuilder.cpp` builds the JSON request body for every AI provider via `JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages))`.
- `code/backend/application/workflow/aiTranscript.cpp` writes per-call transcripts with `JsonHelper jsonHelper; jsonHelper.SanitizeForJson(...)` for each text/error/finish-reason field.
- `code/backend/application/assistant/assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, `assistantController.cpp`, and `assistantTools.cpp` all use the static `JsonHelper::EscapeJsonString` for their JSON-string embeds (session JSONL, memory JSON, index JSONL, WS protocol messages, generated `global.json`).
- **Cloud surface** — every executor and connector that splices user-controlled values into a JSON request or response body uses `JsonHelper::EscapeJsonString`.  This covers both directions:
  - **Request bodies** (data going OUT to the cloud API): slack `chat.postMessage` channel/text/thread_ts; jira `create` projectKey/summary/issueType/description/priority/labels and `transition` transitionId and `comment` body; gitHub `issue` title/body/labels and `comment` body; snowflake `statement.query` (the SQL string itself) and warehouse/database/schema; polarionWrite work-item field-value bodies; dbQuery column-name and value embedding; sheets `values` cells; redmine `issue.notes` / `assigned_to_id` (string form).
  - **Response bodies the executor synthesises** (when the upstream returned `204 No Content`): jira `update` and `transition` success-response objects, polarionWrite download success-response, every cloud-storage executor's synthesised `{"ok":true,...}` summary.  Every value spliced into these is escaped at the splice site.
  - **Result-write JSON** (the on-disk artefact written by query operations): snowflake `result.json` row data with both column names AND values escaped; dbQuery column names and PG values escaped; sheets read-output column names and values escaped.

These call sites benefit transparently from the fix — any provider response, session content, or cloud-API value containing control bytes (which previously generated malformed JSON or lost form-feed data) now produces valid output.  No raw `+` / `<<` JSON splice with a user-controlled value remains in the cloud surface; future cloud-executor edits must keep this discipline (a raw splice is a code-review reject).

---

## 5. Summary

- **ConfigParser**: parses `config.json` using `simdjson::ondemand`, populates `EngineConfig`, logs some fields, and logs unknown top-level fields.
- **ConfigChecker**: validates directories and API selection and applies sensible defaults for thread count, sleep time, and max file size.
- **JsonHelper**: RFC 8259-compliant escape for embedding arbitrary text inside JSON string literals.  Use the static `JsonHelper::EscapeJsonString` for new code; the instance `SanitizeForJson` is retained for legacy callers and delegates to the static path.
