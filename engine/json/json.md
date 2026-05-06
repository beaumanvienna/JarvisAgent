# JarvisAgent Engine JSON Utilities

This document describes the JSON-related engine components used to load and validate `config.json`:

- `ConfigParser`
- `ConfigChecker`
- `JsonHelper`

All code lives under `engine/json`.

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

The full struct + nested `ApiInterface` / `RateLimit` / `RequestBudget` definitions live in `engine/json/configParser.h` and grow as new providers / knobs land — see the header for the canonical shape.  The closed-set enum that the parser dispatches on:

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

For the field-by-field config.json reference (every `"queue folder"` / `"max threads"` / `"port"` / `"API interfaces"[]` key, with defaults and ranges) see `doc/jarvisagent.md` "Configuration".  The mapping from JSON key → `EngineConfig` member is mechanical 1:1 in `Parse()`.

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
4. Iterates over top-level fields.  Each field uses simdjson's `.get<T>().get(target)` extractor; on type mismatch the parser emits `LOG_CORE_ERROR("ConfigParser: '{}' must be a {string|number|boolean|array}", fieldName)` and **continues** to the next field — a single malformed field does not abort the whole config load, so partially-valid configs still populate as much as they can.  Numeric fields cast to `size_t` / `uint32_t` defensively reject negative values with the same log + continue pattern, since negatives would otherwise wrap to huge unsigned values that `ConfigChecker` would only detect symptomatically via its out-of-range clamps.  Logging behaviour:
   - `description`, `author`, and other informational fields are logged.
   - Unknown top-level fields: best-effort stringified and logged as `"key: value"` (or `"[complex type]"` for arrays/objects).
5. Sets state:
   - `ConfigOk` if **both**:
     - `"queue folder"` appeared at least once, **and**
     - at least one `"url"` field appeared within `"API interfaces"`.
   - Otherwise: `FileFormatFailure`.
6. Logs a "format info" summary of field occurrences.

**Important:** `ConfigOk` indicates successful parsing and minimal required presence checks. It does **not** guarantee semantic correctness (directory existence, valid API selection, etc.). Semantic validation is done by `ConfigChecker` (Section 3).

### 2.5 ParseInterfaces()

```cpp
void ParseInterfaces(simdjson::ondemand::array jsonArray,
                     EngineConfig& engineConfig,
                     FieldOccurances& fieldOccurances);
```

- Iterates the `"API interfaces"` array.
- For each element (full per-field semantics + provider list — including all seven valid `"API"` values, `rate_limit` schema, `max_context_tokens`, `default_output_tokens`, `key_name` — live in `doc/jarvisagent.md` "API interfaces").  Highlights specific to the parser:
  - `"name"` is auto-generated from URL domain + model + API type via `GenerateInterfaceName()` (e.g. `api.openai.com/gpt-4.1/API1`) when omitted.  The enum→string side of the mapping uses the `InterfaceTypeName()` helper, which exhaustively switches every `InterfaceType` variant + `static_assert`s on `NumAPIs == 7` so a future provider addition fails to compile until the helper is extended.
  - `"API"` parsing routes through the `ParseInterfaceType()` helper backed by the file-local `kInterfaceTypeMappings` table — single source of truth for both directions of the mapping (`"API1"` ↔ `InterfaceType::API1`, `"Test"` ↔ `InterfaceType::Test`, …).  An unknown value (e.g. `"API7"`) produces a `LOG_CORE_ERROR` and assigns `InterfaceType::InvalidAPI` to the interface; `ConfigChecker` then rejects it from the active-index slot but other interfaces still load.  No hard-stop.
- Appends each `ApiInterface` to `engineConfig.m_ApiInterfaces`.

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
     - URL must `starts_with("https://")` (prefix, not substring — a URL like `http://x/?next=https://y` is rejected).
     - Model must be non-empty.
     - Interface type must not be `InvalidAPI`.

3. If validation fails:
   - Logs `LOG_CORE_ERROR` for the failing component(s) (folder path / API index + count). Operator-visible details land in `log/log.txt`.
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

- `application/json/requestBuilder.cpp` builds the JSON request body for every AI provider via `JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages))`.
- `application/workflow/aiTranscript.cpp` writes per-call transcripts with `JsonHelper jsonHelper; jsonHelper.SanitizeForJson(...)` for each text/error/finish-reason field.
- `application/assistant/assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, `assistantController.cpp`, and `assistantTools.cpp` all use the static `JsonHelper::EscapeJsonString` for their JSON-string embeds (session JSONL, memory JSON, index JSONL, WS protocol messages, generated `global.json`).
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
