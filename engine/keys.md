# Key Management System

---

## 1. Overview

The KeyManager is a multi-provider AI-credential store. It keeps an in-memory registry mapping
logical provider names to `{endpoint, api_key, default_model, api_type}`, populated at startup
from the AES-256-GCM-encrypted `keys.json.enc` and unlocked at runtime with the master password.
JCWF tasks select a backend via `"provider": "<name>"`; a `default_provider` covers tasks that
don't specify one. Every credential is encrypted at rest — there is no plaintext-keystore path
and no environment-variable credential path in any edition.

---

## 2. Goals

1. **Multiple providers** — support OpenAI, Anthropic, Google, local (Ollama/vLLM), and arbitrary
   OpenAI-compatible endpoints, each with its own key and defaults.
2. **Encrypted storage** — all keys in a single AES-256-GCM encrypted file (`keys.json.enc`) with
   a master password, unlocked at runtime. This is the **only** credential source in any edition:
   no plaintext-keystore path and no env-var credential path. Every credential is encrypted at rest.
3. **Provider registry** — an in-memory registry mapping logical provider names to
   `{endpoint, api_key, default_model, api_type}`, populated at startup from the decrypted keys.
4. **Per-task provider selection** — JCWF tasks can specify `"provider": "<name>"` to route
   requests to a specific AI backend.
5. **UI for key management** — a settings page in the React frontend to add/edit/remove providers
   and set the master password.
6. **Encrypted-only, no unsecured fallback** — `keys.json.enc` is the sole credential source in
   every edition; there is no plaintext-keystore path and no `OPENAI_API_KEY` env-var fallback
   (an env-var key is plaintext in the process environment — visible via `/proc/PID/environ` /
   `docker inspect` — which the "no unsecured keys" posture in `doc/cyber security.md` forbids).

---

## 3. File Layout

```
jarvisAgent/
├── keys.json.enc       # AES-256-GCM encrypted keystore (the only on-disk credential store)
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

## 4. Keystore JSON Format

This is the credential schema held inside the encrypted `keys.json.enc` (the bytes `ParseProvidersJson` parses after decryption); it is never written to disk unencrypted.

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
engine/keys/credential.h        — typed ICredential hierarchy (5 subtypes + factory)
```

`KeyManager` stores a registry of typed `ICredential` pointers.  The hierarchy lives in
`credential.h`: `ApiKeyCredential` (Bearer / x-api-key / api-key / x-goog-api-key),
`OAuthCredential` (refresh-token-rotated), `KeyPairCredential` (RSA PEM for JWT signing),
`BasicAuthCredential` (username + password), and `AwsCredential` (access_key_id +
secret_access_key + session_token + region).  Every secret-bearing field is a
`SecureString` so the plaintext value lives in mlock'd, zero-on-destruct memory rather
than a copy-prone `std::string`.

```cpp
class KeyManager
{
public:
    // Lifecycle: load/save the encrypted store with a master password.
    bool Load(std::filesystem::path const& keysFilePath, std::string_view masterPassword);
    bool Save(std::filesystem::path const& keysFilePath, std::string_view masterPassword);

    // Runtime unlock: decrypt the stored keys file path with a freshly-supplied password.
    bool Unlock(std::string_view masterPassword);

    // Read access via callback.  The callback runs while a shared_lock is held, so
    // the `ICredential const&` reference is guaranteed live for the call's duration.
    // Callers MUST NOT store the reference (or pointers to its fields) beyond the
    // callback — a subsequent RemoveProvider would invalidate it.  Callbacks MUST NOT
    // re-enter write-side methods on the same KeyManager (would deadlock against the
    // held shared_lock); read-side re-entry (other With/Has) is fine.
    template <typename F> bool WithCredential(std::string const& name, F&& fn) const;
    template <typename F> bool WithDefaultCredential(F&& fn) const;

    // Existence checks (cheap shared_lock + map lookup).
    bool HasCredential(std::string const& name) const;
    bool HasDefaultCredential() const;

    std::vector<std::string> GetProviderNames() const;
    bool HasProviders() const;

    // Write access (thread-safe, unique lock).  Take ownership of a pre-built credential.
    // REST handlers build via `CredentialFactory::CreateFromJson` (CREATE) or
    // `CredentialFactory::CloneAndPatch` (UPDATE).
    bool AddCredential(std::string const& name, std::unique_ptr<ICredential> cred);
    bool RemoveProvider(std::string const& name);
    [[nodiscard]] bool SetDefaultProvider(std::string const& name);  // rejects empty + unknown
    void ClearDefaultProvider();                                      // explicit clear

    // Atomic read-modify-write under unique_lock — mutator(existing) → new credential.
    // Returns false if `name` doesn't exist or mutator returned nullptr.  Closes the
    // race window between an old-style read-then-write pair where a concurrent
    // RemoveProvider would dangle.
    template <typename F> bool ModifyCredential(std::string const& name, F&& mutator);

    // Atomic add-or-update under unique_lock — builder(existing_or_null) → new credential.
    // Use when the flow may legitimately create or update in one operation (e.g. OAuth
    // callback persisting freshly-issued tokens whether or not the user pre-created a
    // same-named provider).
    template <typename F> void UpsertCredential(std::string const& name, F&& builder);

private:
    std::string m_DefaultProviderName;
    std::unordered_map<std::string, std::unique_ptr<ICredential>> m_Credentials;
    mutable std::shared_mutex m_Mutex;
};
```

Consumer pattern (every cloud connector + every credential consumer):

```cpp
SecureString bearerToken;  // request-scoped mlock'd buffer; SecureString::Set copies bytes
                           // into a fresh mlock'd region without going through std::string.
bool const found = Core::g_Core->GetKeyManager().WithCredential(connection.m_KeyName,
    [&](ICredential const& cred)
    {
        auto const* api = dynamic_cast<ApiKeyCredential const*>(&cred);
        if (!api) { /* wrong type — fail closed, don't fall through to other types */ return; }
        bearerToken.Set(api->m_ApiKey.Get());
    });
if (!found) { /* not found */ }
// Pass bearerToken to AppendSecretHeader(headers, "Authorization: Bearer ", bearerToken, scratch)
// or hand SecureString::CStr() to libcurl directly (CURLOPT_PASSWORD etc.) — never concatenate
// into a std::string. See doc/cyber security.md "SecureString-only HTTP path".
```

The `dynamic_cast` cascade is allowlist-style: connectors that legitimately accept multiple
shapes (e.g. Jira Cloud's BasicAuth vs Jira DC's PAT, S3's three conventions) explicitly
check each valid type and fail closed if none match.  No "default-to-ApiKey" fallback.

### Snapshot pattern for deferred dispatch

`WithCredential`'s lock-scoped contract works for any consumer that uses the credential
immediately and completes synchronously.  Async-dispatch paths (`AiRequestPool::Submit`
queues a request that may sit in the dispatcher's inbox / retry queue for minutes
before the signer fires) need a different shape: the credential pointer must outlive
the callback.  The pattern is a **deep-copy snapshot taken inside the `WithCredential`
callback**, wrapped in `std::shared_ptr<T const>`:

```cpp
// aiRequestPool.cpp::ResolveAwsCredentialSnapshot
std::shared_ptr<AwsCredential const> ResolveAwsCredentialSnapshot(api)
{
    std::shared_ptr<AwsCredential> snap;
    keyManager.WithCredential(api.m_KeyName,
        [&](ICredential const& cred)
        {
            auto const* aws = dynamic_cast<AwsCredential const*>(&cred);
            if (!aws) return;                              // wrong type — fail closed
            snap = std::make_shared<AwsCredential>();
            // metadata + non-secret fields: plain assignment
            snap->m_AccessKeyId = aws->m_AccessKeyId;
            snap->m_Region      = aws->m_Region;
            // SecureString fields: copy via Set(Get()) — non-copyable type
            snap->m_SecretAccessKey.Set(aws->m_SecretAccessKey.Get());
            snap->m_SessionToken.Set(aws->m_SessionToken.Get());
        });
    return snap;
}
```

The snapshot is stored in `CurlWrapper::QueryData::m_AwsCredential` (`shared_ptr<AwsCredential const>`)
and consumed by `SigV4Signer::Apply()` at signing time — the dispatcher may have queued
the request long after `WithCredential` returned, but the snapshot still carries the
correct credential.  A concurrent `RemoveProvider` / `SetDefaultProvider` mutates the
KeyManager registry without touching the snapshot — the in-flight request signs with
the credential that existed at submit time, which is the correct semantic.

Use this pattern for ANY caller that needs the credential to outlive `WithCredential`'s
synchronous scope.  Don't smuggle the raw `ICredential const&` out via lambda capture —
the reference dangles after the callback returns.  Don't copy secret material into a
plain `std::string` field — keep `SecureString` end-to-end so the mlock'd buffer + zero-
on-destruct invariant survives.

### Hardening guards

- **Encrypted-only keystore.** Credentials load exclusively from the AES-256-GCM `keys.json.enc` store (`Load`), unlocked at runtime with the master password.  There is no plaintext-keystore path and no `OPENAI_API_KEY` env-var path in any edition — every credential is encrypted at rest.  Matches the "same cyber-sec measures across editions" posture in `doc/cyber security.md`.
- **Keystore size cap.** `Load` rejects files larger than `kMaxKeysFileBytes = 4 MB` before parsing.  Bounds OOM-via-hostile-keystore at the boundary; realistic keystores are < 100 KB.
- **Provider count cap.** `ParseProvidersJson` aborts after `kMaxProviders = 1024` entries with a structured ERROR.  Bounds per-provider unique_ptr allocations.
- **`SetDefaultProvider` rejects empty.** `[[nodiscard]]` return; empty / unknown names log a WARN and return false.  Use `ClearDefaultProvider()` for explicit clears so a hand-edit accident can't silently wipe the default by passing an empty string.
- **`Unlock` TOCTOU closed.** The keys file path is captured under `m_KeysFilePathMutex` at function entry, so a concurrent `SetKeysFilePath` cannot swap the path between the existence check and the `Load` call.

### 6.1 Startup sequence

```
1. ConfigParser parses config.json → reads "keys_file" (default: "keys.json.enc")
2. If keys_file exists:
   a. Mark KeyLoadStatus = NoPassword; the store stays sealed until the admin calls
      POST /api/settings/keys/unlock (or submits the dashboard login flow, which
      threads the password through the same unlock handler).
   b. On unlock: KeyManager::Load() decrypts, populates the provider registry, and
      also unlocks mcp_keys.json.enc with the same password (WebServer::InitMcpKeyStore).
3. Else (no keys_file present):
   a. Log warning: no API keys configured
   b. AI tasks will fail at dispatch time with a clear error until a keystore is created + unlocked
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

## 8. CurlWrapper Integration

`CurlWrapper` no longer owns the credential.  The credential travels in the request through `CurlWrapper::QueryData::m_ApiKey` (a `SecureString`, mlock'd + zero-on-destruct, non-copyable).  The caller (`AiRequestPool` / `AiCallTaskExecutor` / `TestApiInterface`) resolves the provider via `KeyManager` (callback-scoped `WithCredential` / `WithDefaultCredential`), populates `QueryData::m_ApiKey.Set(view)`, and invokes `Query()`.

Auth-header production is delegated to `IAuthSigner` (`engine/curlWrapper/authSigner.{h,cpp}`).  `Apply()` writes non-secret headers into a `std::vector<std::string>& publicHeaders` and the secret-bearing header (Bearer / x-api-key / x-goog-api-key / api-key, plus SigV4 X-Amz-Security-Token when an STS session token is present) into a caller-owned `SecureString& secretHeader` via `SecureString::Format(prefix, secret.Get())` — single mlock'd allocation, no `std::string` intermediate.  The transport layer (`CurlWrapper::Query` / `LiveTransport::SetupEasyHandle` / `MockTransport`) hands `secretHeader.CStr()` to `curl_slist_append` (or to `CurlSlist::AppendCStr`); libcurl's own `strdup` is the irreducible residue floor and is documented as outside the threat-model boundary.

For cloud connectors / workflow filters that build secret-bearing headers themselves (rather than going through `IAuthSigner`), the canonical helper is `AppendSecretHeader(curl_slist*&, prefix, SecureString const&, SecureString& scratch)` in `engine/curlWrapper/curlSlistHelper.h`.  See `doc/cyber security.md` "SecureString-only HTTP path" for the full discipline.

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
4. ✅ `KeyManager::Load()` called at runtime unlock; no keystore → WARN (no env-var/plaintext fallback).
5. ✅ `keys.json.enc` added to `.gitignore`.
6. ✅ `premake5.lua` updated.

### Phase 2: Wire KeyManager into request pipeline — **DONE**

8. ✅ `CurlWrapper::QueryData` carries the resolved credential via `SecureString m_ApiKey` (non-copyable).
9. ✅ `AiCallTaskExecutor` + `AiRequestPool::Submit` resolve per-task `api_interface` and populate `QueryData::m_ApiKey.Set(view)` from the KeyManager callback.
10. ✅ Provider's key/endpoint resolved per interface and threaded through `QueryData`.
11. ✅ Legacy static `m_ApiKey` removed from CurlWrapper.

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

## 13. Startup Behavior

| Scenario                                    | Behavior                                           |
|---------------------------------------------|----------------------------------------------------|
| No `keys.json.enc`                          | Warning logged; AI tasks fail with a clear error until a keystore is created + unlocked. |
| `keys.json.enc` exists, not yet unlocked    | Store stays sealed (`NoPassword`); awaits `POST /api/settings/keys/unlock`. |
| `keys.json.enc` exists, password provided   | Decrypts; full multi-provider support.              |
| `keys.json.enc` exists, wrong password      | Decryption fails; store stays sealed (no fallback). |
| JCWF task has no `"provider"` field         | Uses `default_provider` from the keystore.          |

---

## 14. Security Notes

- **Encrypted-only keystore.** Every edition uses `keys.json.enc` (AES-256-GCM) exclusively — there is no plaintext-keystore path and no `OPENAI_API_KEY` env-var path. Every credential is encrypted at rest; an env-var key (plaintext in the process environment, readable via `/proc/PID/environ` or `docker inspect`) is exactly the unsecured exposure the keystore exists to prevent.
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
