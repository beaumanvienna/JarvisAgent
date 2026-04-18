# Key Management System — Design & Change Plan

Last updated: Feb 2026

---

## 1. Problem Statement

Today JarvisAgent reads a single API key from the `OPENAI_API_KEY` environment variable
(`engine/curlWrapper/curlWrapper.cpp:46`). This key is stored as a process-wide static
(`CurlWrapper::m_ApiKey`) and used for every AI request regardless of provider, model, or task.

This means:

- Only one AI provider can be used at a time.
- Switching providers requires restarting with a different env var.
- No way for individual JCWF tasks to target different models/providers.
- The API key lives in the shell environment with no encryption at rest.

---

## 2. Goals

1. **Multiple providers** — support OpenAI, Anthropic, Google, local (Ollama/vLLM), and arbitrary
   OpenAI-compatible endpoints, each with its own key and defaults.
2. **Encrypted storage** — all keys in a single AES-256-GCM encrypted file (`keys.json.enc`) with
   a master password. Plaintext `keys.json` only exists during development (gitignored).
3. **Provider registry** — an in-memory registry mapping logical provider names to
   `{endpoint, api_key, default_model, api_type}`, populated at startup from the decrypted keys.
4. **Per-task provider selection** — JCWF tasks can specify `"provider": "<name>"` to route
   requests to a specific AI backend.
5. **UI for key management** — a settings page in the React frontend to add/edit/remove providers
   and set the master password.
6. **Backward compatibility** — if `keys.json.enc` does not exist, fall back to `OPENAI_API_KEY`
   env var so existing setups keep working.

---

## 3. File Layout

```
jarvisAgent/
├── keys.json           # plaintext (gitignored, dev only)
├── keys.json.enc       # AES-256-GCM encrypted (production)
├── config.json         # updated: new "keys_file" field
├── engine/
│   ├── keys/
│   │   ├── keyManager.h        # KeyManager class
│   │   ├── keyManager.cpp      # load/decrypt/encrypt/save, provider registry
│   │   ├── keyEncryption.h     # AES-256-GCM + PBKDF2 helpers
│   │   └── keyEncryption.cpp   # uses vendored OpenSSL
│   ├── curlWrapper/
│   │   ├── curlWrapper.h       # modified: Query() takes provider name
│   │   └── curlWrapper.cpp     # modified: looks up key from KeyManager
│   └── json/
│       ├── configParser.h      # modified: add m_KeysFilePath to EngineConfig
│       └── configParser.cpp    # modified: parse "keys_file" field
├── application/
│   ├── web/
│   │   └── webServer.cpp       # new routes: /api/settings/providers/*
│   └── workflow/
│       └── aiCallTaskExecutor.cpp  # modified: pass provider name to CurlWrapper
└── workflow-editor/ui/src/
    └── settings/
        ├── SettingsView.tsx     # settings page with provider management
        └── ProviderForm.tsx     # add/edit provider form component
```

---

## 4. keys.json Format

```jsonc
{
    "version": 1,
    "default_provider": "openai_gpt4",
    "providers": {
        "openai_gpt4": {
            "display_name": "OpenAI GPT-4",
            "endpoint": "https://api.openai.com/v1/chat/completions",
            "api_key": "sk-...",
            "default_model": "gpt-4.1",
            "api_type": "API1"
        },
        "anthropic_claude": {
            "display_name": "Anthropic Claude",
            "endpoint": "https://api.anthropic.com/v1/messages",
            "api_key": "sk-ant-...",
            "default_model": "claude-sonnet-4-20250514",
            "api_type": "API1"
        },
        "google_gemini": {
            "display_name": "Google Gemini (native)",
            "endpoint": "https://generativelanguage.googleapis.com/v1beta",
            "api_key": "AIza...",
            "default_model": "gemini-2.5-flash",
            "api_type": "API3"
        },
        "local_ollama": {
            "display_name": "Local Ollama",
            "endpoint": "http://localhost:11434/v1/chat/completions",
            "api_key": "",
            "default_model": "llama3",
            "api_type": "API1"
        }
    }
}
```

| Field            | Type   | Description                                              |
|------------------|--------|----------------------------------------------------------|
| `version`        | number | Schema version for future migration.                     |
| `default_provider` | string | Logical name used when a task does not specify one.    |
| `providers`      | object | Map of logical name → provider config.                   |
| `display_name`   | string | Human-readable label for the UI.                         |
| `endpoint`       | string | Full URL to the chat completions endpoint.               |
| `api_key`        | string | API key (empty string for local/keyless endpoints).      |
| `default_model`  | string | Model string sent in the request body.                   |
| `api_type`       | string | `"API1"`, `"API2"`, or `"API3"` — selects the request/reply parser. API1 = OpenAI-compatible, API2 = OpenAI Responses API (GPT-5+), API3 = Google Gemini native. |

---

## 5. Encryption Scheme

Uses vendored OpenSSL (already in `vendor/openssl`). No new dependencies.

### 5.1 Encrypt (keys.json → keys.json.enc)

```
1. Generate 16-byte random salt.
2. Derive 32-byte key from master password via PBKDF2-HMAC-SHA256 (100 000 iterations, salt).
3. Generate 12-byte random IV (nonce).
4. Encrypt plaintext JSON with AES-256-GCM → ciphertext + 16-byte auth tag.
5. Write file: [4-byte magic "JKEY"] [1-byte version] [16-byte salt] [12-byte IV]
               [ciphertext] [16-byte tag]
```

### 5.2 Decrypt (keys.json.enc → in-memory JSON)

```
1. Read and validate magic + version.
2. Extract salt, IV, ciphertext, tag.
3. Derive key from master password via PBKDF2-HMAC-SHA256 (same iterations, extracted salt).
4. Decrypt with AES-256-GCM, verify auth tag.
5. Parse resulting JSON into provider registry.
```

### 5.3 File header (33 bytes)

| Offset | Size | Content                          |
|--------|------|----------------------------------|
| 0      | 4    | Magic: `JKEY` (0x4A4B4559)       |
| 4      | 1    | Version: `0x01`                  |
| 5      | 16   | Salt (random)                    |
| 21     | 12   | IV / nonce (random)              |
| 33     | ...  | Ciphertext                       |
| EOF-16 | 16   | GCM authentication tag           |

---

## 6. Architecture — KeyManager

```
engine/keys/keyManager.h
```

```cpp
class KeyManager
{
public:
    struct ProviderConfig
    {
        std::string m_DisplayName;
        std::string m_Endpoint;
        std::string m_ApiKey;
        std::string m_DefaultModel;
        std::string m_ApiType;      // "API1" or "API2"
    };

    // Startup: load from encrypted file (or fall back to env var)
    bool Load(std::filesystem::path const& keysFilePath, std::string const& masterPassword);

    // Save current state to encrypted file
    bool Save(std::filesystem::path const& keysFilePath, std::string const& masterPassword);

    // Provider registry access
    ProviderConfig const* GetProvider(std::string const& name) const;
    ProviderConfig const* GetDefaultProvider() const;
    std::vector<std::string> GetProviderNames() const;

    // CRUD (used by API endpoints)
    bool AddProvider(std::string const& name, ProviderConfig config);
    bool UpdateProvider(std::string const& name, ProviderConfig config);
    bool RemoveProvider(std::string const& name);
    void SetDefaultProvider(std::string const& name);

    // Backward compatibility: populate from env var
    bool LoadFromEnvironment();

private:
    std::string m_DefaultProviderName;
    std::unordered_map<std::string, ProviderConfig> m_Providers;
    mutable std::shared_mutex m_Mutex;    // readers-writer lock
};
```

### 6.1 Startup sequence

```
1. ConfigParser parses config.json → reads "keys_file" (default: "keys.json.enc")
2. If keys_file exists:
   a. Mark KeyLoadStatus = NoPassword; the store stays sealed until the admin calls
      POST /api/settings/keys/unlock (or submits the dashboard login flow, which
      threads the password through the same unlock handler).
   b. On unlock: KeyManager::Load() decrypts, populates the provider registry, and
      also unlocks mcp_keys.json.enc with the same password (WebServer::InitMcpKeyStore).
3. Else if OPENAI_API_KEY env var is set:
   a. KeyManager::LoadFromEnvironment() creates a single "openai" provider entry
      using the existing config.json API interfaces/index for endpoint/model/api_type
4. Else:
   a. Log warning: no API keys configured
   b. AI tasks will fail at dispatch time with a clear error
```

---

## 7. config.json Changes

New optional field:

```jsonc
{
    "keys_file": "keys.json.enc"
}
```

If absent, defaults to `"keys.json.enc"` in the working directory.

---

## 8. CurlWrapper Changes

### Current

```cpp
// curlWrapper.h
static std::string m_ApiKey;         // single global key

// curlWrapper.cpp constructor
char* apiKeyEnv = std::getenv("OPENAI_API_KEY");

// Query()
headers.Append("Authorization: Bearer " + m_ApiKey);
```

### New

```cpp
// curlWrapper.h
// Remove: static std::string m_ApiKey;

// Query() signature change
bool Query(QueryData const& queryData, std::string const& apiKey);

// Query() implementation
headers.Append("Authorization: Bearer " + apiKey);
```

The caller (`AiRequestPool` / `AiCallTaskExecutor`) resolves the provider via `KeyManager`,
extracts the key, and passes it to `Query()`.

---

## 9. Per-Task AI Interface Selection

Tasks of type `ai_call` can optionally specify an AI interface via the `api_interface` field:

```jsonc
{
    "id": "summarize",
    "type": "ai_call",
    "api_interface": "api.openai.com/gpt-4.1/API1"
}
```

The value references an AI interface `name` from `config.json` `"API interfaces"`. Each AI interface can in turn reference an API key via `key_name`.

Resolution order:
1. Task-level `"api_interface"` → look up in config's `m_ApiInterfaces` by name
2. Interface's `key_name` → look up API key in KeyManager
3. If no `api_interface` specified → use the global `"API index"` from config.json
4. If no `key_name` on the interface → use the default (first) provider from KeyManager

The workflow editor Inspector shows an **AI Interface** dropdown for `ai_call` nodes, populated from the live list of configured interfaces.

---

## 10. Backend API Endpoints

### 10.1 Key Management (`/api/settings/keys/*`)

| Method   | Path                              | Description                |
|----------|-----------------------------------|----------------------------|
| `GET`    | `/api/settings/keys/status`       | Key load status (ok, no_password, wrong_password, no_keys_file) |
| `POST`   | `/api/settings/keys/unlock`       | Submit master password to decrypt keys at runtime |

### 10.2 Provider / Key CRUD (`/api/settings/providers/*`)

| Method   | Path                              | Description                |
|----------|-----------------------------------|----------------------------|
| `GET`    | `/api/settings/providers`         | List all providers (keys shown as has_key boolean) |
| `POST`   | `/api/settings/providers`         | Add a new provider (name + api_key) |
| `PUT`    | `/api/settings/providers/<name>`  | Update a provider          |
| `DELETE` | `/api/settings/providers/<name>`  | Remove a provider          |
| `POST`   | `/api/settings/providers/save`    | Encrypt and save to disk (keys.json.enc) |

### 10.3 AI Interfaces (`/api/settings/ai-interfaces/*`)

| Method   | Path                                      | Description                |
|----------|-------------------------------------------|----------------------------|
| `GET`    | `/api/settings/ai-interfaces`             | List all interfaces (from config.json) |
| `POST`   | `/api/settings/ai-interfaces`             | Add a new interface        |
| `PUT`    | `/api/settings/ai-interfaces/<name>`      | Update an interface        |
| `DELETE` | `/api/settings/ai-interfaces/<name>`      | Delete an interface        |
| `POST`   | `/api/settings/ai-interfaces/save`        | Save interfaces to config.json |

**Note:** URL path parameters are URL-decoded server-side to support names with spaces and special characters.

Key masking: `GET /api/settings/providers` responses include a `has_key` boolean per provider.
Full keys are never sent to the frontend after initial entry.

---

## 11. Frontend

Three views in the workflow editor at `/editor`:

### 11.1 AI Keys Page (nav button: "AI Keys")

**Component:** `ProvidersSettingsView.tsx`

- Lists API keys by name with "key set" / "no key" status.
- **+ Add Key** button — form with Name (dropdown from AI interfaces) + API Key.
- **Edit** — update name or API key (leave blank to keep current).
- **Delete** — remove a key.
- **Save Encrypted** — encrypts and writes to `keys.json.enc`.

### 11.2 AI Manager Page (nav button: "AI Manager")

**Component:** `AiManagerView.tsx`

- Lists all AI interfaces from `config.json` with name, description, URL, model, API type.
- Active interface (by `API index`) highlighted in blue.
- **Key dropdown** per interface — selects which API key to use (`key_name` field). Defaults to first available key if none selected. Shows "no key configured" if no keys exist.
- **+ Add Interface** / **Edit** / **Delete** for CRUD.
- **Save to config.json** — persists changes including `key_name`.
- **Reload** — re-reads from the backend.

### 11.3 Editor Inspector (AI Interface dropdown)

When an `ai_call` node is selected in the workflow editor, the Inspector panel shows an **AI Interface** dropdown between Type and working_directory. Options include:
- "default (global API index)" — uses the global setting.
- All configured AI interface names.

The selection is stored as `api_interface` on the task and preserved in `.jcwf` files.

### 11.4 Master Password Dialog

**Component:** `MasterPasswordDialog.tsx`

- Shown automatically on page load when `keys.json.enc` exists but no master password is set.
- Password input with eye icon toggle (show/hide).
- Wrong password shows red error, modal stays open.
- On success, modal closes and keys are loaded.

### 11.5 Key entry UX

- API keys are entered once and sent to the backend.
- After saving, keys are shown as "key set" (boolean) in the UI.
- To change a key, the user types a new one (empty = keep existing).

---

## 12. Change Plan — Existing → New

### Phase 1: KeyManager + encryption (backend only) — **DONE**

1. ✅ Created `engine/keys/keyManager.h` and `keyManager.cpp` — provider registry with CRUD.
2. ✅ Created `engine/keys/keyEncryption.h` and `keyEncryption.cpp` — AES-256-GCM encrypt/decrypt
   using vendored OpenSSL.
3. ✅ Added `"keys_file"` field to `ConfigParser` / `EngineConfig`.
4. ✅ `KeyManager::Load()` called in startup sequence.
5. ✅ Falls back to `KeyManager::LoadFromEnvironment()` if no encrypted file.
6. ✅ `keys.json` and `keys.json.enc` added to `.gitignore`.
7. ✅ `premake5.lua` updated.

### Phase 2: Wire KeyManager into request pipeline — *partial*

8. ✅ `CurlWrapper::Query()` modified to accept an API key parameter.
9. ⚠️ `AiCallTaskExecutor` — per-task `api_interface` stored in `.jcwf` but not yet resolved at dispatch time.
10. ⚠️ Provider's key/endpoint not yet passed to `CurlWrapper::Query()` per interface.
11. ⚠️ Static `m_ApiKey` still exists in CurlWrapper as fallback.

### Phase 3: Backend API — **DONE**

12. ✅ Provider CRUD routes: `/api/settings/providers/*`
13. ✅ AI interface CRUD routes: `/api/settings/ai-interfaces/*` with save to config.json.
14. ✅ Key status and unlock routes: `/api/settings/keys/status`, `/api/settings/keys/unlock`.
15. ✅ URL decoding for path parameters with special characters.

### Phase 4: Frontend — **DONE**

16. ✅ AI Keys page (`ProvidersSettingsView.tsx`) — simplified name + API key UI.
17. ✅ AI Manager page (`AiManagerView.tsx`) — interface CRUD with key dropdown.
18. ✅ Nav buttons: "AI Manager", "AI Keys" in workflow editor.
19. ✅ Master password dialog with eye icon toggle.
20. ✅ Editor Inspector: AI Interface dropdown for `ai_call` nodes.

### Phase 5: Per-task AI interface selection — *frontend done, backend pending*

21. ✅ `api_interface` field stored on `ai_call` tasks in `.jcwf` files (frontend).
22. ✅ `key_name` field on AI interfaces in `config.json` linking interfaces to keys.
23. ⚠️ Backend dispatch: resolve `api_interface` → interface config → `key_name` → API key at runtime.

---

## 13. Backward Compatibility

| Scenario                                    | Behavior                                           |
|---------------------------------------------|----------------------------------------------------|
| No `keys.json.enc`, `OPENAI_API_KEY` set    | Works exactly as today (single provider from env).  |
| No `keys.json.enc`, no env var              | Warning logged; AI tasks fail with clear error.     |
| `keys.json.enc` exists, password provided   | Full multi-provider support.                        |
| `keys.json.enc` exists, wrong password      | Decryption fails; falls back to env var if present. |
| JCWF task has no `"provider"` field         | Uses `default_provider` from keys.json.             |

---

## 14. Security Notes

- **Plaintext `keys.json`** is gitignored and only used for development convenience.
  Production uses `keys.json.enc` exclusively.
- **Master password** is never stored on disk and never read from an environment variable.
  It is supplied at runtime via `POST /api/settings/keys/unlock` (or the dashboard login
  flow, which posts to the same endpoint) and held in an `mlock()`-protected
  `SecureString` buffer that zeroes on destruction.
- **In-memory keys** live in `KeyManager` for the lifetime of the process. No disk caching
  of decrypted material.
- **API responses** always mask keys (first 3 + last 4 characters). Full keys are write-only
  from the frontend's perspective.
- **PBKDF2 iterations** (100 000) follow OWASP 2023 recommendations for SHA-256.
- **AES-256-GCM** provides authenticated encryption — detects tampering and wrong passwords.
