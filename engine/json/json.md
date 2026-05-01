# JarvisAgent Engine JSON Utilities

This document describes the JSON-related engine components used to load and validate `config.json`:

- `ConfigParser`
- `ConfigChecker`
- `JsonHelper`

All code lives under `engine/json`.

---

## 1. config.json

JarvisAgent reads configuration from `config.json` at startup (example below). Fields not recognized by the parser are *best-effort stringified and logged* (top-level only), then ignored.

```jsonc
{
    "file format identifier": 1.2,
    "description": "jarvisAgent configuration file",
    "author": "Copyright (c) 2025 JC Technolabs",

    "queue folder": "../queue",
    "workflows folder": "../workflows",
    "max threads": 20,
    "engine sleep time in run loop in ms": 16,
    "verbose": false,

    "API interfaces": [
        {
            "name": "api.openai.com/gpt-4.1/API1",
            "description": "High-accuracy model with strong reasoning.",
            "url": "https://api.openai.com/v1/chat/completions",
            "model": "gpt-4.1",
            "API": "API1",
            "key_name": "openai"
        }
    ],

    "API index": 0,
    "max file size in kB": 24,
    "keys_file": "keys.json.enc",
    "use_bash": false
}
```

### 1.1 Top-level keys and meaning

| JSON key | Type | Meaning | Notes |
|---|---:|---|---|
| `file format identifier` | number | Format marker/version for the config file. | Parsed only for type checking; **not stored** in `EngineConfig`. |
| `description` | string | Human-readable description. | Logged; not stored. |
| `author` | string | Human-readable author/copyright string. | Logged; not stored. |
| `queue folder` | string | Path to the queue folder directory. | Stored as `EngineConfig::m_QueueFolderFilepath`. Must be an existing directory (checked by `ConfigChecker`). |
| `workflows folder` | string | Path to the workflows folder directory. | Stored as `EngineConfig::m_WorkflowsFolderFilepath`. Must be an existing directory (checked by `ConfigChecker`). |
| `port` | number | Web server listen port. `0` = auto (8080 for HTTP, 8443 for HTTPS). | Stored as `EngineConfig::m_Port`. Valid range `[1, 65535]`, defaults to `0` (auto). |
| `max threads` | number | Worker-thread pool size. | Stored as `EngineConfig::m_MaxThreads`. `ConfigChecker` clamps via defaults if out of range. |
| `max inflight ai calls` | number | Maximum concurrent AI requests dispatched via HTTP/2. Decoupled from thread pool size since requests are multiplexed on a single I/O thread. | Stored as `EngineConfig::m_MaxInflightAiCalls`. `ConfigChecker` clamps to `[1, 1000]`, defaults to `100`. |
| `python engines` | number | Number of Python sub-interpreters (each with its own GIL) for parallel Python task execution. Requires Python 3.12+. | Stored as `EngineConfig::m_PythonEngines`. `ConfigChecker` clamps to `[1, 16]`, defaults to `4`. |
| `engine sleep time in run loop in ms` | number | Sleep interval in the main run loop. | Stored as `EngineConfig::m_SleepDuration` (ms). Defaults applied if out of range. |
| `verbose` | boolean | Enables verbose logging. | Stored as `EngineConfig::m_Verbose`. |
| `API interfaces` | array | List of API endpoints/models. | Parsed by `ConfigParser::ParseInterfaces()`. |
| `API index` | number | Selects active interface in `API interfaces`. | Stored as `EngineConfig::m_ApiIndex`. Must point to an existing entry (see `ConfigChecker`). |
| `max file size in kB` | number | Maximum allowed file size for queue items. | Stored as `EngineConfig::m_MaxFileSizekB`. Defaults applied if out of range. |
| `keys_file` | string | Path to the encrypted keys file. | Stored as `EngineConfig::m_KeysFilePath`. Defaults to `"keys.json.enc"` if not specified. |
| `use_bash` | boolean | Windows only: prefer bash (MSYS2/Git Bash) over PowerShell for shell tasks. | Stored as `EngineConfig::m_UseBashOnWindows`. Defaults to `false` (PowerShell is the default on Windows). Parsed on all platforms but only meaningful on Windows; on Linux/macOS the startup log appends `(Windows-only, ignored on this platform)`. |

---

## 2. ConfigParser

**Header:** `json/configParser.h`  
**Source:** `json/configParser.cpp`  
**Namespace:** `AIAssistant`

`ConfigParser` loads and validates the raw JSON configuration file (e.g. `config.json`) and fills a `ConfigParser::EngineConfig` struct.

### 2.1 EngineConfig

```cpp
struct EngineConfig
{
    enum InterfaceType
    {
        API1 = 0,
        API2,
        API3,
        NumAPIs,
        InvalidAPI
    };

    struct ApiInterface
    {
        std::string m_Name;
        std::string m_Description;
        std::string m_Url;
        std::string m_Model;
        std::string m_KeyName;
        InterfaceType m_InterfaceType{InterfaceType::InvalidAPI};
    };

    // Generate a unique interface name from URL domain + model + API type
    static std::string GenerateInterfaceName(std::string const& url,
                                             std::string const& model,
                                             std::string const& apiType);

    size_t m_MaxThreads{0};
    size_t m_MaxInflightAiCalls{100};
    size_t m_PythonEngines{4};
    std::chrono::milliseconds m_SleepDuration{0};
    std::string m_QueueFolderFilepath;
    std::string m_WorkflowsFolderFilepath;
    bool m_Verbose{false};
    size_t m_ApiIndex{0};
    std::vector<ApiInterface> m_ApiInterfaces;
    size_t m_MaxFileSizekB{20};
    std::string m_KeysFilePath{"keys.json.enc"};
    bool m_UseBashOnWindows{false};
    bool m_ConfigValid{false};

    bool IsValid() const { return m_ConfigValid; }
};
```

**Stored fields populated from JSON:**

- `m_QueueFolderFilepath` from `"queue folder"`
- `m_WorkflowsFolderFilepath` from `"workflows folder"`
- `m_MaxThreads` from `"max threads"`
- `m_MaxInflightAiCalls` from `"max inflight ai calls"`
- `m_PythonEngines` from `"python engines"`
- `m_SleepDuration` from `"engine sleep time in run loop in ms"`
- `m_Verbose` from `"verbose"`
- `m_ApiInterfaces` from `"API interfaces"` (via `ParseInterfaces`)
- `m_ApiIndex` from `"API index"`
- `m_MaxFileSizekB` from `"max file size in kB"`
- `m_KeysFilePath` from `"keys_file"` (defaults to `"keys.json.enc"`)
- `m_UseBashOnWindows` from `"use_bash"` (defaults to `false`)

**Logged-only (not stored):**

- `"description"`
- `"author"`

**Type-checked but not stored:**

- `"file format identifier"` (must be a JSON number)

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
3. Parses JSON via `simdjson::ondemand`.
   - On parse error: `ParseFailure`.
4. Iterates over top-level fields:
   - Validates expected types with `CORE_ASSERT`.
   - Populates `EngineConfig` where applicable.
   - Logs `description`, `author`, and other informational fields.
   - Unknown top-level fields:
     - Attempts best-effort stringification and logs `"key: value"` for simple types.
     - Logs `"[complex type]"` for arrays/objects.
5. Sets state:
   - `ConfigOk` if **both**:
     - `"queue folder"` appeared at least once, **and**
     - at least one `"url"` field appeared within `"API interfaces"`.
   - Otherwise: `FileFormatFailure`.
6. Logs a “format info” summary of field occurrences.

**Important:** `ConfigOk` indicates successful parsing and minimal required presence checks. It does **not** guarantee semantic correctness (directory existence, valid API selection, etc.). Semantic validation is done by `ConfigChecker` (Section 3).

### 2.5 ParseInterfaces()

```cpp
void ParseInterfaces(simdjson::ondemand::array jsonArray,
                     EngineConfig& engineConfig,
                     FieldOccurances& fieldOccurances);
```

- Iterates the `"API interfaces"` array.
- For each element:
  - `"name"` (string) → `ApiInterface::m_Name`. If not provided, auto-generated from URL domain + model + API type via `GenerateInterfaceName()` (e.g. `api.openai.com/gpt-4.1/API1`).
  - `"description"` (string) → `ApiInterface::m_Description`. Optional human-readable hint.
  - `"url"` (string) → `ApiInterface::m_Url`
  - `"model"` (string) → `ApiInterface::m_Model`
  - `"API"` (string) → maps `"API1"` / `"API2"` / `"API3"` to `InterfaceType::API1` / `API2` / `API3`, otherwise `CORE_HARD_STOP`.
    - `API1` — OpenAI-compatible chat completions (`/v1/chat/completions`): OpenAI, Google Gemini via OpenAI-compat endpoint, Anthropic, Ollama, and any compatible provider.
    - `API2` — OpenAI Responses API (GPT-5 and later models).
    - `API3` — Google Gemini native API (`x-goog-api-key` header, `/models/{model}:generateContent` URL scheme).
  - `"key_name"` (string) → `ApiInterface::m_KeyName`. Optional reference to an API key by name (as stored in the encrypted keys file). If empty, the default (first available) key is used at runtime.
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
   - The selected index must be valid.
     - **Practical requirement:** `engineConfig.m_ApiIndex < engineConfig.m_ApiInterfaces.size()`.
   - For the selected interface:
     - URL must be non-empty and contain `"https://"`
     - Model must be non-empty
     - Interface type must not be `InvalidAPI`

3. If validation fails:
   - Logs error(s) for the failing component(s).
   - Sets `engineConfig.m_ConfigValid = false`.

4. If validation succeeds:
   - Applies defaults for out-of-range values:
     - `m_MaxThreads`: if `<= 0` or `> 256` → set to `16`.
     - `m_MaxInflightAiCalls`: if `== 0` or `> 1000` → set to `100`.
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

Every outbound AI request body and persisted transcript flows through this helper:

- `application/json/requestBuilder.cpp` builds the JSON request body for every AI provider via `JsonHelper().SanitizeForJson(ConcatMessages(envelope.m_Messages))`.
- `application/workflow/aiTranscript.cpp` writes per-call transcripts with `JsonHelper jsonHelper; jsonHelper.SanitizeForJson(...)` for each text/error/finish-reason field.
- `application/assistant/assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, `assistantController.cpp`, and `assistantTools.cpp` all use the static `JsonHelper::EscapeJsonString` for their JSON-string embeds (session JSONL, memory JSON, index JSONL, WS protocol messages, generated `global.json`).

These call sites benefit transparently from the fix — any provider response or session content containing control bytes (which previously generated malformed JSON or lost form-feed data) now produces valid output.

---

## 5. Summary

- **ConfigParser**: parses `config.json` using `simdjson::ondemand`, populates `EngineConfig`, logs some fields, and logs unknown top-level fields.
- **ConfigChecker**: validates directories and API selection and applies sensible defaults for thread count, sleep time, and max file size.
- **JsonHelper**: RFC 8259-compliant escape for embedding arbitrary text inside JSON string literals.  Use the static `JsonHelper::EscapeJsonString` for new code; the instance `SanitizeForJson` is retained for legacy callers and delegates to the static path.
