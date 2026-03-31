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
| `max threads` | number | Worker-thread pool size. | Stored as `EngineConfig::m_MaxThreads`. `ConfigChecker` clamps via defaults if out of range. |
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
     - `m_SleepDuration`: if `<= 0ms` or `> 256ms` → set to `10ms`.
     - `m_MaxFileSizekB`: if `<= 0` or `> 256` → set to `20`.
   - Sets `engineConfig.m_ConfigValid = true`.

---

## 4. JsonHelper

**Header:** `json/jsonHelper.h`  
**Source:** `json/jsonHelper.cpp`  
**Namespace:** `AIAssistant`

`JsonHelper` provides small JSON utility helpers.

### 4.1 SanitizeForJson()

```cpp
class JsonHelper
{
public:
    std::string SanitizeForJson(std::string const& input);
};
```

**Behavior (as implemented):**

- Returns a copy of `input` where:
  - `"` becomes `\"`
  - `\` becomes `\\`
  - newline becomes `\n`
  - carriage return becomes `\r`
  - tab becomes `\t`
- One additional `switch` case in the current source appears as a non-printable / mangled character in some outputs; it is handled with a `break` (skipped). If you want, we can inspect the source file directly in a clean rendering and document that case precisely.

---

## 5. Summary

- **ConfigParser**: parses `config.json` using `simdjson::ondemand`, populates `EngineConfig`, logs some fields, and logs unknown top-level fields.
- **ConfigChecker**: validates directories and API selection and applies sensible defaults for thread count, sleep time, and max file size.
- **JsonHelper**: provides string sanitization for embedding arbitrary text safely in JSON strings.
