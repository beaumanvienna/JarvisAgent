# JarvisAgent — Threat Model & Vulnerability Analysis (STRIDE)

> **Complete** — the full STRIDE walk (boundaries 1–11 + data stores) is done in §3; findings + accepted risks are in §4. **No open findings.** F2 (cookie `Secure`) was a non-finding — already mitigated; F3 (stale comment) is fixed; F1 (webhook replay) is accepted for 1.0 as AR-6. No CRITICAL or HIGH surfaced.
> This is the system-level security analysis, complementary to [`doc/cyber security.md`](cyber%20security.md) (which documents the per-subsystem controls). Where this document says "mitigated," the cited control lives in cyber security.md or the named source file.

JarvisAgent ("j9t") is a C++ workflow engine that coordinates concurrent AI requests and external-service integrations, with a React dashboard + workflow editor, an MCP tool surface, and embedded Python/shell task execution. This document models the system as a data-flow diagram with trust boundaries, inventories every datum at rest, and walks the six STRIDE threat categories against each model element.

---

## 1. Methodology

STRIDE (Microsoft; Kohnfelder & Garg, 1999) is a mnemonic for six threat categories — each the violation of one security property:

|   | Threat | Violates |
|---|---|---|
| **S** | Spoofing — pretending to be someone/something else | Authentication |
| **T** | Tampering — unauthorized modification of data/code | Integrity |
| **R** | Repudiation — denying an action with no way to prove it happened | Non-repudiation |
| **I** | Information disclosure — exposing data to those not authorized | Confidentiality |
| **D** | Denial of service — degrading/blocking availability | Availability |
| **E** | Elevation of privilege — gaining capabilities you shouldn't have | Authorization |

The analysis is **model-driven**: build the system model (§2), then for each element walk the applicable STRIDE categories and record the mitigation or the gap (§3). The element→category mapping is the standard one — external entities: S, R; processes: all six; data flows: T, I, D; data stores: T, R, I, D.

---

## 2. System model

### 2.1 Deployment assumption (the primary trust boundary)

j9t's default deployment is **single-host**: the operator runs it on their own machine or server, and **everything on `127.0.0.1` is treated as trusted** (the operator, their browser, their MCP sidecar, local LLMs). The security boundary is the network edge + the master-password-gated secret store. A future SaaS / multi-tenant deployment would move this boundary and is **out of scope for 1.0** but flagged at each element where the single-host assumption is load-bearing. The Docker deployment narrows the host trust further (the container only sees `~/JarvisAgent/`).

### 2.2 Actors & adversaries

| Actor | Trusted? | Capability |
|---|---|---|
| Operator / admin | Yes | Full control: unlock store, CRUD keys/connections/interfaces/workflows, run-control, shutdown |
| Viewer | Partly | Read-only monitoring (no mutation, no run-control) |
| Operator-tier MCP agent | Partly | Run workflows, adhoc submit (gated), run-control on own runs |
| Adhoc-run author | Partly | Submit a JCWF for one-shot execution (scripts must pre-exist under `scripts/`) |
| JCWF author | Semi-untrusted | Authors workflow definitions (templates, filters, task graph) — a `.jcwf` is a portable artifact that may come from a third party |
| **External network attacker** | No | Unauthenticated traffic to the listening port |
| **Malicious AI-provider / cloud endpoint** | No | Controls the response to an outbound request |
| **Local-filesystem attacker** | No (but high bar) | Read/write the on-disk state if they already have host FS access |

### 2.3 Trust boundaries

1. **Browser ↔ j9t** (REST + WebSocket) — session-cookie auth (HttpOnly, SameSite), optional TLS.
2. **MCP agent / sidecar ↔ j9t** — Bearer MCP key (`mcp_*`), constant-time compared.
3. **Webhook caller ↔ j9t** — HMAC-SHA256-signed trigger payloads.
4. **j9t → AI providers** (outbound) — `IAuthSigner`, `UrlPolicy` (loopback-only plain http), connect-time loopback guard.
5. **j9t → cloud connectors** (outbound) — `ConnectorHttp` SSRF gate, per-connection credentials.
6. **j9t ↔ embedded Python engine** — script execution; `ConfineUnderProjectRoot`, sys.path confinement.
7. **j9t ↔ shell tasks** — argv-only (no `system()`/`popen()`), `scripts/`-gated.
8. **j9t ↔ filesystem** — path confinement on every external string; edition-aware (`ConfineUnderProjectRoot`).
9. **Studio vs Engine edition** — capability boundary enforced at build time (`removefiles`) + runtime gates.
10. **Single-host `127.0.0.1`-trust vs future SaaS** — the implicit boundary (see §2.1).
11. **Privilege tiers** — admin / operator / viewer, one auth funnel per surface.

### 2.4 Data-flow diagram

```
        ┌─────────────┐   cookie/TLS   ┌──────────────────────────────────────────┐
 Browser│ dashboard / │───────────────▶│                 WebServer                 │
  user  │   editor    │◀───────────────│  (REST + WS, Crow)  ── auth funnel ──┐     │
        └─────────────┘                │                                      │     │
                                       │   ┌───────────────┐  ┌───────────────▼───┐ │
 MCP     ──Bearer mcp_*──────────────▶ │   │ TriggerEngine │  │   KeyManager /    │ │
 agent                                 │   │ cron/webhook/ │  │  EncryptedStores  │ │──▶ keys.json.enc
                                       │   │ file/email/   │  └───────────────────┘ │    mcp_keys.json.enc
 Webhook ──HMAC-SHA256──────────────▶  │   │ cloud-poll    │                        │    API.json.enc
 source                                │   └───────┬───────┘                        │    connections.json.enc
                                       │           ▼                                │
                                       │   ┌───────────────────┐                    │
                                       │   │  WorkflowRuntime  │── DAG execution    │
                                       │   └──┬────────┬───────┴──────┬─────────────┘
                                       └──────┼────────┼──────────────┼─────────────┘
                                              ▼        ▼              ▼
                                  ┌──────────────┐ ┌──────────┐ ┌──────────────┐
                                  │ AiRequestPool│ │ Python   │ │  Cloud       │
                                  │ + dispatcher │ │ EnginePool│ │  Connectors  │
                                  └──────┬───────┘ └────┬─────┘ └──────┬───────┘
                                         │ TLS/auth     │ argv         │ TLS/SSRF gate
                                         ▼              ▼              ▼
                                 AI providers      shell/scripts   cloud services
                                 (or local LLM)    + queue/ files  (S3, Jira, …)
```

**Processes:** WebServer · TriggerEngine · WorkflowRuntime · AiRequestPool/CurlMultiDispatcher · PythonEnginePool · CloudConnectors · KeyManager/EncryptedJsonStore.
**External entities:** browser user · MCP agent · webhook source · AI provider · cloud service · JCWF author.
**Data stores:** the inventory in §2.5.

### 2.5 Data-at-rest inventory

Verified against `config.json` + the keystore code (`config.json` holds **only** non-secret scalars; the `.enc` stores are AES-256-GCM with a PBKDF2-derived key from the master password, held in `mlock`'d memory).

| Store | Holds | Form at rest | Protected by |
|---|---|---|---|
| `keys.json.enc` | AI provider keys, OAuth access/refresh tokens | AES-256-GCM | master password (PBKDF2) |
| `mcp_keys.json.enc` | MCP API keys + enrollments | AES-256-GCM | master password |
| `API.json.enc` | AI routing config (interfaces, default/jcwf selectors) | AES-256-GCM | master password |
| `connections.json.enc` | cloud connection configs (endpoints + key references) | AES-256-GCM | master password |
| `config.json` | ports, folder paths, TLS cert/key **paths**, thread counts — **no secrets** | plaintext | filesystem perms |
| `certs/j9t-key.pem` | **TLS private key** | plaintext PEM | filesystem perms |
| `workflows/*.jcwf` + extracted `workflows/<id>/` | workflow definitions + bundled input data | plaintext zip / files | filesystem perms |
| `queue/<wf>/<task>/` (STNG / CNTX / TASK / PROB / `*.output.*`) | task inputs + AI outputs — **may carry PII**; whether a template can resolve a *credential* into a queue file is a §3 question | plaintext | filesystem perms |
| `log/log.txt`, `log/security.txt` | application + security logs | plaintext, **secret-redacted** | redactor + filesystem perms |
| `_adhoc/<user_slug>/...` | per-user adhoc run scratch | plaintext | per-tenant dir + TTL reaper |
| `.email_watermarks.json` | IMAP UID watermarks per email-watch trigger | plaintext | filesystem perms |
| browser session | authenticated session | HttpOnly cookie | SameSite + TLS |

**Note (the `connections.json` regression check):** an earlier build kept this store as **plaintext `connections.json`** — exactly the system-level exposure the per-file audit missed. It is now `connections.json.enc` (AES-256-GCM). This inventory format — *form at rest* + *protected by* per store — is what makes that class of issue visible; any future store added here with "plaintext / filesystem perms" against sensitive content is a finding on sight.

---

## 3. Threat analysis (STRIDE per element)

Each row: the applicable STRIDE category → the existing mitigation (cite) **or** a gap. All boundaries (1–11) + data stores walked.

### 3.1 Boundary 1 — Browser ↔ j9t (REST + WebSocket)

| STRIDE | Threat | Mitigation / Gap |
|---|---|---|
| S | Forge a user session | Session cookie `HttpOnly; SameSite=Strict; Path=/`, plus `Secure` conditionally when TLS is active (`m_TlsEnabled`); one auth funnel per surface; per-IP failed-auth lockout (10 failures / 15 min). **Mitigated.** |
| T | Tamper a request / CSRF | `SameSite=Strict` + HSTS (when TLS) + `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Referrer-Policy`. **Mitigated.** |
| R | Deny an action | `log/security.txt` records auth success/failure with IP + endpoint. **Mitigated.** |
| I | Read another user's data | Optional TLS (recommended when reachable beyond localhost); error messages omit install paths; static-asset routes path-confined. **Mitigated** (within the single-host model). |
| D | Exhaust the service | Two-tier per-IP rate-limit (pre-auth 100/min, authed 1200/min) + lockout. **Mitigated.** |
| E | Gain admin from viewer | Role tiers (admin / operator / viewer) enforced at one funnel; run-control + mutation gated to operator/admin. **Mitigated.** |

### 3.2 Boundary 2 — MCP agent ↔ j9t (Bearer)

| STRIDE | Mitigation / Gap |
|---|---|
| S | Bearer `mcp_*` key, **constant-time** (`CRYPTO_memcmp`) compared; key must start with `mcp_`; disabled/expired keys rejected. **Mitigated.** |
| R | `mcp_auth_success` / `mcp_auth_failure` (reason, ip, user, role) → security log. **Mitigated.** |
| D / E | Same lockout + role-on-key as boundary 1. **Mitigated.** |

### 3.3 Boundary 3 — Webhook caller ↔ j9t (HMAC)

| STRIDE | Mitigation / Gap |
|---|---|
| S / T | `X-Webhook-Signature: sha256=<HMAC-SHA256(raw body)>`, verified with a **constant-time** compare (`VerifyHmacSignature`, `webServer_helpers.h`). Forgery without the secret is infeasible. **GAP:** the signature covers only the body — **no timestamp/nonce → no replay protection** (a captured valid request can be re-sent and re-triggers the workflow). Accepted for 1.0 → **AR-6.** |
| I | `runId` from the body is allowlist-validated (`IsValidWorkflowId`) before becoming a path segment — no `../` escape. **Mitigated.** |
| R | `webhook_rejected` (reason) / accepted → security log. **Mitigated.** |
| D | Per-IP rate-limit + lockout apply. **Mitigated.** |

### 3.4 Boundary 8 — Filesystem

| STRIDE | Mitigation / Gap |
|---|---|
| T / I | Every external string → `ConfineUnderProjectRoot` (fail-closed, `..`/absolute/symlink-escape rejected); `.jcwf` extraction Zip-Slip / symlink-race / zip-bomb hardened; `ScriptCatalog` skips symlinks + canonical-confines. **Mitigated** (recently hardened). |

### 3.5 Boundary 9 — Studio vs Engine edition

| STRIDE | Mitigation / Gap |
|---|---|
| E | Capability boundary enforced at **build time** (`premake removefiles` strips `assistant/**`, `aiJcwfService`, `webServer_studio.cpp` from Engine) + runtime edition gates. Engine cannot link or expose Studio-only mutation/AI-tooling surfaces. **Mitigated.** |

### 3.6 Boundary 10 — Single-host `127.0.0.1`-trust

The deployment-model boundary (§2.1). **Accepted risk AR-1** for 1.0: loopback is trusted, so a misbehaving local client shares the per-IP lockout bucket with the operator's browser (observed: a stray local MCP sidecar with a stale token can lock out `127.0.0.1`). Acceptable single-host; must be revisited for SaaS/multi-tenant.

### 3.7 Boundary 11 — Privilege tiers

| STRIDE | Mitigation / Gap |
|---|---|
| E | One auth funnel per surface; admin/operator/viewer gating; adhoc submission rejects JCWFs whose `command`/`module` isn't already under `scripts/` (no code-injection path). **Mitigated.** Adhoc runs are visible to all authenticated users (per-user privacy deferred) — **Accepted risk AR-2**. |

### 3.8 Data stores (T / I / R / D)

| Store(s) | STRIDE | Mitigation / Gap |
|---|---|---|
| `keys.json.enc`, `mcp_keys.json.enc`, `API.json.enc`, `connections.json.enc` | I | AES-256-GCM, PBKDF2-derived key from the master password held in `mlock`'d memory. **Mitigated.** |
| same | T | GCM **AAD binds** magic/version/salt/IV — any tamper invalidates the tag at decrypt. **Mitigated.** |
| `config.json` | I | Plaintext but holds **no secrets** (folders/ports/TLS *paths*/counts). **Not a finding** — the method correctly clears it. |
| `certs/j9t-key.pem` | I | **TLS private key plaintext on disk**, filesystem-perms only. **Accepted risk AR-3** (standard self-hosted posture; a host-FS compromise enables MITM — same trust level as the rest of the install). |
| `workflows/`, `queue/<wf>/`, `_adhoc/` | I | Plaintext task I/O; may carry PII. Under the single-host model this is host-FS-trust (AR-1). **A `{{...}}` template CANNOT resolve a credential into these files** — the template engine's namespaces are `inputs`/`outputs` only, with zero `KeyManager` reachability (verified). Flag PII-at-rest if SaaS. |
| `log/log.txt`, `log/security.txt` | I | Secret-redactor scrubs credentials before any log write; the redactor is the single control. **Mitigated** (redactor coverage is a standing concern, not a gap). |

### 3.9 Boundary 4 — j9t → AI providers (outbound)

| STRIDE | Mitigation / Gap |
|---|---|
| I | Prompt/output exfil to an unintended endpoint: `UrlPolicy` (plain-`http://` loopback-only, never with a `key_name`) + **connect-time loopback guard** (defeats DNS rebinding) + CRLF header-injection guard; credentialed dispatch is `https://` only. Secrets travel via the **SecureString-only HTTP path** (no plaintext heap residue). **Mitigated** (this session's 3 HIGHs + §18). |
| T / S | TLS `VERIFYPEER`/`VERIFYHOST` (the debug-localhost skip is compile-isolated to Debug + logs a WARN); HTTP/2 over ALPN. **Mitigated.** |
| — (malicious provider response) | 32 MiB response-body cap (sync + async write callbacks); `SanitizeUtf8` on parsed message fields (TUI/log byte-safety); reply parser bounds. **Mitigated.** |
| D | AIMD per-quota concurrency cap + per-request timeout. **Mitigated.** |

### 3.10 Boundary 5 — j9t → cloud connectors (outbound)

| STRIDE | Mitigation / Gap |
|---|---|
| I | SSRF exfil: `ConnectorHttp::OpensocketStrictCallback` rejects loopback / RFC-1918 / link-local / cloud-metadata at **connect** (post-DNS), `ValidatePublicHttpEndpoint` is the syntactic pre-check; Postgres `sslmode` allowlist gates plaintext-fallback modes for non-localhost; credential CRLF gate; credentials handled as `SecureString`. **Mitigated.** |
| T | `VERIFYPEER=1`/`VERIFYHOST=2` explicit; `REDIR_PROTOCOLS_STR="https"` (no http-downgrade leaking the bearer on a redirect); bounded `MAXREDIRS`. **Mitigated.** |
| — (malicious cloud response) | `BoundedStringWriteCallback` response cap. **Mitigated.** |
| D | Cloud circuit breaker + retry policy. **Mitigated.** |

### 3.11 Boundary 6 — j9t ↔ embedded Python engine

| STRIDE | Mitigation / Gap |
|---|---|
| E | Python is an **intentional code-execution surface**, gated: adhoc submissions must reference a `module` that already exists under `scripts/` (no arbitrary code via the API); Studio operators authoring workflows are trusted. → **Accepted risk AR-5.** |
| T / I | `scriptPath` parent + `scriptDir` (sys.path) + `taskWorkingDirectory` all `ConfineUnderProjectRoot`-validated before use; `PyErr_Clear` hygiene. **Mitigated.** |

### 3.12 Boundary 7 — j9t ↔ shell tasks

| STRIDE | Mitigation / Gap |
|---|---|
| E (via `args[]`) | Every `args[]` value is **single-quoted** (`QuoteForPosixShell`, embedded-quote-safe) before the `/bin/sh -c` line — closes `$(...)` / `;` / metacharacter injection through template-substituted (incl. attacker-influenced webhook-context) args. **Mitigated.** |
| E (via `command`) | The `command` field is **raw shell by design** (globbing / operators); gated for adhoc by the `scripts/`-allowlist, and operator-authored shell is an intentional admin capability. → **Accepted risk AR-4** (same class as the assistant `run_shell` forever-HIGH). |
| — (markitdown conversion) | The `popen()` markitdown call takes a **path-allowlisted** argument. **Mitigated.** |

---

## 4. Findings & accepted risks

**Full walk complete (boundaries 1–11 + all data stores). The cyber-sec posture is strong: no CRITICAL or HIGH surfaced. No open findings; six accepted risks.** The outbound + execution boundaries (AI / cloud / Python / shell) were the densest in controls and came back clean — the recent §18 hardening + this session's 3 HIGHs cover them.

### Closed (verified non-findings)

- **F2 — Session cookie `Secure` flag** — *already mitigated in shipped code*; all three `Set-Cookie` sites (two login, one logout) set `Secure` conditionally on `m_TlsEnabled` (`= TlsCert && TlsKey`), so the cookie carries `Secure` over TLS and omits it only on plain-`http://` localhost. The first-cut walk read this as missing; verification against `webServer.cpp` shows it present.
- **F3 — Stale comment** at `shellTaskExecutor.cpp` (claimed args were joined unvalidated) — *fixed*; the comment now describes the single-quoting that `JoinArgumentsForSystem` actually performs.
- **Template → credential leak into `queue/`** — *not possible*; template namespaces are `inputs`/`outputs`, no `KeyManager` reachability.
- **`config.json` at rest** — plaintext but non-secret; correctly not flagged.
- **`.enc` store integrity** — GCM AAD-binding detects tamper.

### Accepted risks (explicit decisions for 1.0)

- **AR-1 — Single-host `127.0.0.1` trust.** Loopback is trusted; shared per-IP lockout bucket. Revisit for SaaS.
- **AR-2 — Adhoc runs visible to all authenticated users.** Per-user privacy deferred (operator tier is already a boundary).
- **AR-3 — TLS private key plaintext at rest.** Standard self-hosted; host-FS-trust equivalent to the rest of the install.
- **AR-4 — Shell-task `command` field is raw shell.** An intentional admin code-execution surface (operator-authored or `scripts/`-gated for adhoc); same class as the assistant `run_shell` forever-HIGH. `args[]` injection is closed by single-quoting.
- **AR-5 — Embedded Python executes admin-deployed scripts.** The only arbitrary-code path in a workflow; gated to `scripts/`-resident modules for adhoc.
- **AR-6 — Webhook HMAC has no replay protection.** The signature covers only the body (no timestamp/nonce), so a captured valid `(body, signature)` pair can be re-sent. The attacker must first observe a valid signed request (they never hold the secret), and the per-IP loopback trust + idempotent-trigger assumption bound the impact. Accepted for 1.0; revisit if webhooks drive non-idempotent mutating actions. Fix path if reopened: fold an `X-Webhook-Timestamp` into the HMAC input + reject outside a freshness window (a sender-contract change for n8n et al.).

---

## References

- [`doc/cyber security.md`](cyber%20security.md) — per-subsystem security controls (the "here are our controls" reference).
- [`doc/architecture.md`](architecture.md) — system architecture + "Key Design Decisions" (much trust-boundary rationale).
- `doc/misc/stride-pass-dev-plan.md` — the internal plan that produced this document.
