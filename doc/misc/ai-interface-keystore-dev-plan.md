# Development plan — move AI-routing + cloud-connection config into the encrypted keystore

Status: **all decisions settled — implementing all phases (0–4) in one pass, review at the end.** This is the build plan.

## 1. Goal & threat

`config.json` (plaintext, read before unlock) holds the AI-routing table — each interface's `url` + `key_name`, the default `"API index"`, and `"jcwf AI interface"`. At dispatch, the `key_name` resolves to the **decrypted** credential and is sent to that interface's `url`. So editing `config.json` (or, while running, the already-unlocked dashboard) lets an attacker **exfiltrate a live credential** (repoint `url`, keep `key_name`) or **reroute dispatch**. `connections.json` (cloud connectors) is the same bug class. Fix: move both into encrypted files unlocked by the master password, and require master-password re-entry for routing mutations.

Two sub-vectors of "admin left the computer open at lunch":
- **(a) on-disk edit** of `config.json` / `connections.json` → closed by moving the data into encrypted files.
- **(b) open authenticated dashboard tab** mutating over REST → closed by the re-auth gate (§3.3).

## 2. Decisions (settled)

- **Two new parallel encrypted files:** `API.json.enc` (AI interfaces + defaults) and `connections.json.enc` (cloud connections), each unlocked by the master password.
- **Selection by stable name** — interfaces + default + jcwf selectors hold names.
- **Start empty** on fresh installs; users add interfaces/connections via the dashboard after unlock.
- **Migration is manual, one-time** against the two dev configs (done out-of-band; nothing committed to the project).
- **Re-auth gate in scope** — every routing/connection mutation requires master-password re-entry (per-mutation, no grace window / no session-cached re-auth).
- **`jcwf batch size` stays in `config.json`** (operational throughput knob).
- **Pre-unlock UX is already handled** — both UIs gate the whole app behind `MasterPasswordDialog` (blurred background) while `status.keys_unlocked === false`; once unlocked, the settings views load their data like everything else.

## 3. Shared abstractions & helpers (DRY — build these first)

The three encrypted-store managers (`McpKeyManager` today + `ApiInterfaceManager` + `ConnectionStore` new) all repeat the same dance: read file → size-cap → `KeyEncryption::Decrypt` → simdjson parse; and serialize → `KeyEncryption::Encrypt` → `EngineCore::AtomicWriteFile`. That is the third+fourth copy of one pattern — CLAUDE.md's "extract a helper before the third site" rule applies. Pull the boilerplate up before adding the new managers.

### 3.1 `EncryptedJsonStore` base (`code/backend/engine/keys/encryptedJsonStore.{h,cpp}`)
Owns the crypto + IO + caps; derived classes own only the record model.

```cpp
class EncryptedJsonStore {                       // engine layer; no app deps
public:
    [[nodiscard]] std::expected<void, StoreError> Load(std::filesystem::path const&, SecureString const& masterPw);
    [[nodiscard]] std::expected<void, StoreError> Save(std::filesystem::path const&, SecureString const& masterPw);
    bool IsLoaded() const;
protected:
    virtual std::string SerializeToJson() const = 0;                       // derived builds plaintext JSON
    virtual std::expected<void, StoreError> ParseFromJson(std::string_view) = 0;  // derived simdjson parse
    virtual size_t MaxFileBytes() const { return 4 * 1024 * 1024; }        // overridable cap
    mutable std::shared_mutex m_Mutex;
};
```

`StoreError` follows the existing `std::expected<T, SubsystemError>` pattern (typed `Code` enum + `m_Details`, no `default:` arm). Base handles: size cap → `KeyEncryption::Decrypt` → `ParseFromJson` on load; `SerializeToJson` → `KeyEncryption::Encrypt` → `AtomicWriteFile` on save. **Retrofit `McpKeyManager` onto it in the same phase** (proves the base, removes the existing duplicate). `KeyManager` (richer credential-factory model) is a candidate 4th adopter for a later pass.

### 3.2 Post-unlock hydration orchestration
Today `HandleKeysUnlockPost` calls `InitMcpKeyStore` + `OAuthTokenManager::HydrateFromKeyManager` ad hoc. Replace with one ordered entry point so the unlock handler doesn't grow a 4th/5th bespoke call:

```cpp
void WebServer::HydrateStoresOnUnlock(SecureString const& masterPw);
// loads + hydrates: mcp keys, api interfaces (→ EngineConfig::m_ApiInterfaces),
// connections (→ connector subsystem), oauth tokens. One place, defined order.
```

Each store's hydrate step follows the `OAuthTokenManager::HydrateFromKeyManager` template (read decrypted → populate in-memory cache → re-validate → log count). Interface/connection hydration re-runs the existing validators (`UrlPolicy::ValidateAiInterfaceUrl`, connector `ValidatePublicHttpEndpoint`/`ValidatePostgresParams`) and drops invalid rows with an ERROR log.

### 3.3 Re-auth gate helper (`WebServer::CheckMasterPasswordReauth`)
One funnel, mirroring `CheckAuth`'s shape, reused by every routing/connection/config mutation handler (matches the "one auth funnel per surface" discipline — no per-handler password checks):

```cpp
// "" on success; non-empty error key on failure (feeds MakeAuthErrorResponse)
std::string WebServer::CheckMasterPasswordReauth(crow::request const& req);
```

Extracts the supplied secret from the request, constant-time-compares to the held master password via `KeyManager::WithMasterPassword` + `CRYPTO_memcmp`, and reuses the existing `RecordAuthFailure` + unlock lockout path so brute force is rate-limited exactly like `/api/settings/keys/unlock`. Logs `[security]` on failure. Mutation handlers call `CheckAdminAuth(req)` **then** `CheckMasterPasswordReauth(req)`; the gate applies to mutation handlers only. Every mutation re-prompts — the proof is not cached across requests (no grace window), so the FE attaches the master password to each routing/connection mutation.

### 3.4 By-name selection helper
Selection moves from numeric index to name. A small shared resolver avoids repeating the lookup at hydrate + REST default-set + dispatch:

```cpp
std::optional<size_t> FindInterfaceIndexByName(std::vector<ApiInterface> const&, std::string_view name);
```

The persisted default/jcwf selectors store names; dispatch read-sites resolve once via this helper (replacing the raw `m_ApiIndex` / `m_JcwfAiInterfaceIndex` integer reads).

### 3.5 Frontend reuse
- Generalize `shared-ui/components/MasterPasswordDialog.tsx` with a purpose prop: `mode: "unlock" | "confirm"`. `"unlock"` keeps today's behavior (POST unlock, `onUnlocked`); `"confirm"` resolves `onConfirm(password)` to the caller without calling the unlock endpoint, so the routing-mutation flow attaches the secret to its request. One dialog, two purposes — no second password component.
- Mutation flows in the existing shared views (`AiManagerView`, `ConnectionsView`, `ProvidersSettingsView`, config-edit in `SettingsModal`) open the dialog in `"confirm"` mode on save; all already route through `@shared/api/auth::authFetch`.

### 3.6 Config path-field block
Add `api_file` + `connections_file` to `EngineConfig` + parser, mirroring the existing `keys_file` / `mcp_keys_file` (non-secret pointers, stay in `config.json`, needed at startup to locate the encrypted files).

## 4. Target architecture (condensed)

- **Storage:** `ApiInterfaceManager` owns `API.json.enc` (`{version, default_interface, jcwf_interface, interfaces[]}`); `ConnectionStore` owns `connections.json.enc`. Both derive `EncryptedJsonStore` (§3.1), use the same master password, unlocked together at `HandleKeysUnlockPost` via §3.2.
- **In-memory cache:** keep `EngineConfig::m_ApiInterfaces` (+ resolved default/jcwf) so dispatch read-sites stay lock-free and unchanged except for name→index resolution; the manager is the persistent owner and hydrates the cache post-unlock. Connections hydrate into the connector subsystem's existing in-memory state.
- **Persistence:** REST CRUD mutates in-memory then `manager.Save(...)` (re-encrypt + atomic write). Deletes the fragile `config.json` string-splice in `HandleAiInterfacesSavePost` and the scalar `replaceField` patching — net simpler.
- **Validation:** unchanged validators, called at REST write + at hydrate.

## 5. Phased implementation

Each phase ends with: studio Release/Debug build clean, targeted tests green, `premake5 gmake` re-run if `.cpp` files were added.

**Phase 0 — shared foundation**
- Add `EncryptedJsonStore` base + `StoreError` (§3.1). Retrofit `McpKeyManager` onto it. Add `CheckMasterPasswordReauth` (§3.3) and `FindInterfaceIndexByName` (§3.4).
- Tests: store round-trip (serialize→encrypt→decrypt→parse equal); tamper byte → GCM auth reject; oversize → cap reject; re-auth helper accept/reject + lockout.

**Phase 1 — AI interfaces → `API.json.enc`**
- New `ApiInterfaceManager` (derives base; (de)serialize `ApiInterface`; by-name defaults; `UrlPolicy` on parse/hydrate).
- `ConfigParser`: remove `"API interfaces"` / `"API index"` / `"jcwf AI interface"` parse arms; add `api_file` path.
- Wire into `HydrateStoresOnUnlock`; hydrate `EngineConfig::m_ApiInterfaces` + resolved defaults.
- REST: rewrite `HandleAiInterface*` + the two settings handlers to persist via the manager and gate mutations with `CheckMasterPasswordReauth`; switch selection to by-name; delete the splice/`replaceField` paths. Drop `core.cpp:412` startup log.
- Read-sites (`aiRequestPool`, `aiCallTaskExecutor`, `aiJcwfService`): default resolution via §3.4.
- Tests: bootstrap → unlock → add interface via REST → lands in `API.json.enc` (not config) → restart → unlock → dispatch succeeds; edit `config.json` routing keys → zero effect; corrupt `API.json.enc` → rejected on unlock; mutation without re-auth → refused.

**Phase 2 — connections → `connections.json.enc`**
- New `ConnectionStore` (derives base). Connector load path (`engine.cpp:191–237`) reads from the store post-unlock instead of plaintext at startup; CRUD persists via the manager + re-auth gate; reuse connector validators.
- Tests: mirror Phase 1 for a connection (add → restart → unlock → connector test succeeds; tamper; re-auth).

**Phase 3 — frontend**
- Generalize `MasterPasswordDialog` (§3.5); wire `"confirm"` mode into the four mutation flows. Verify both dash + editor.

**Phase 4 — cleanup, docs, packaging, manual migration**
- `config.json.example` + packaging: remove the three moved keys + plaintext `connections.json`, add `api_file`/`connections_file`. Doc sweep: `doc/jarvisagent.md`, `doc/cyber security.md`, `doc/api-endpoints.md`. `premake5 gmake` regen.
- One-time manual migration of the two dev configs (move interfaces/connections into the encrypted files via the running dashboard, strip the keys from `config.json`).

## 6. Fields moved (summary)

Out of `config.json` → `API.json.enc`: `"API interfaces"` (whole array), `"API index"` (→ `default_interface` name), `"jcwf AI interface"` (→ `jcwf_interface` name). Separate file `connections.json` → `connections.json.enc`. Everything else stays in `config.json` (incl. `jcwf batch size` and the new `api_file`/`connections_file` paths).

## 7. Testing (rollup)
- Unit: `EncryptedJsonStore` round-trip / tamper / cap; by-name resolver; validators on hydrate + REST; re-auth accept/reject/lockout.
- Integration: per-phase add→restart→unlock→use; tamper-config no-op; tamper-enc reject; mutation requires re-auth.
- Re-run the heap-scan audit if the SecureString→curl path is touched; `test/hardening/test_keymanager_caps.py` as the cap-test model.
