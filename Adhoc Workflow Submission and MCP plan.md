# Adhoc Workflow Submission and MCP — Planning Document

> **Status: implemented (2026-04-18).** Phases 1-3 landed the core MCP + adhoc surface in both editions. Phases 4-7 (post-1.0 section 14 below) added per-user folder namespacing, the artifact retrieval download plane (list + single-file with Range + SHA-256), and the script discovery catalog — so MCP agents can now compose → submit → monitor → enumerate outputs → download end-to-end. This file is kept as the design-of-record; the canonical references for current behaviour are `doc/api-endpoints.md`, `doc/cyber security.md`, `doc/architecture.md`, `mcp/README.md`, `application/workflow/doc/todo.md`, and the 100-test suite in `test/test_auth_mcp.py`.

This document explores the two 1.0 Release features from `doc/roadmap.md`: **Adhoc Workflow Submission** and **MCP Configure-Plane Tools**. It examines what capabilities they unlock, what functions they require, security implications, UI updates, and — critically — how MCP authorization must work for enterprise deployment.

---

## Table of Contents

1. [Current State](#1-current-state)
2. [Adhoc Workflow Submission](#2-adhoc-workflow-submission)
3. [MCP Configure-Plane Tools](#3-mcp-configure-plane-tools)
4. [MCP Authorization — Enterprise Grade](#4-mcp-authorization--enterprise-grade)
5. [Settings UI Consolidation](#5-settings-ui-consolidation)
6. [Dashboard Updates](#6-dashboard-updates)
7. [Unified MCP Security — Both Editions](#7-unified-mcp-security--both-editions)
8. [Cyber Security Impact](#8-cyber-security-impact)
9. [What These Features Unlock](#9-what-these-features-unlock)
10. [MCP Tools — 1.0 Deliverables](#10-mcp-tools--10-deliverables)
11. [Dashboard Login — Enterprise Mode](#11-dashboard-login--enterprise-mode)
12. [Implementation Phases](#12-implementation-phases)
13. [Design Decisions](#13-design-decisions)
14. [Artifact Retrieval — Download-Plane (post-1.0 design)](#14-artifact-retrieval--download-plane-post-10-design)
15. [Appendix A — Detailed Development Plan](#appendix-a--detailed-development-plan)

---

## 1. Current State

### MCP Today

The MCP sidecar (`mcp/`) is a thin TypeScript proxy over stdio/SSE that translates MCP tool calls into j9t REST requests. It currently covers the **run plane** only:

| MCP Tool | REST Endpoint | Capability |
|----------|---------------|------------|
| `list_workflows` | `GET /api/workflows` | Read-only listing |
| `run_workflow` | `POST /api/workflows/<id>/run` | Start a registered workflow |
| `get_run_status` | `GET /api/workflow-runs/<runId>` | Per-task progress |
| `get_run_output` | `GET /api/workflow-runs/<runId>` | Completed run results |
| `list_active_runs` | `GET /api/workflow-runs/active` | Currently running |
| `cancel_run` | `POST /api/workflow-runs/<runId>/cancel` | Cancel a run |

**Authentication:** The MCP server reads a bearer token from `J9T_TOKEN` or `J9T_TOKEN_FILE` and passes it through to j9t. There is **no per-user authentication** at the MCP layer — whoever can connect to the MCP server inherits the token's privileges (currently always `admin`).

### REST API Today

Engine edition has bearer token auth, RBAC (admin/operator/viewer), rate limiting, lockout, TLS, HMAC webhooks, audit logging, and gateway-trusted identity headers. Studio has no auth.

### Gaps Addressed by This Plan

1. **Adhoc submission** — workflows must be pre-deployed to `workflows/`. External agents can only run what's already there.
2. **Configure-plane MCP tools** — agents cannot manage connections, keys, or upload workflows without falling back to curl.
3. **Per-user MCP auth** — the bearer token is a shared service credential with no per-user identity, audit trail, or revocation.
4. **User management UI** — no way to create/revoke MCP credentials or assign roles from the dashboard.
5. **MCP auth in both editions** — no MCP auth in Studio.
6. **Dashboard login** — Engine dashboard is accessible without authentication.

---

## 2. Adhoc Workflow Submission

### Concept

Allow external callers to submit a JCWF payload via REST, execute it in an isolated directory, and return results — without permanently registering the workflow. **Scripts cannot be submitted** — the JCWF must reference scripts that already exist on disk (created by admins, the editor's Generate function, or the AI assistant).

### REST Endpoint

```
POST /api/workflows/run-adhoc
Authorization: Bearer <token>
Content-Type: application/json

{
  "jcwf": { ... },                    // Complete canvas JSON (same as POST /api/workflows body)
  "context": { "key": "value" },       // Optional: seeded into the run's ContextMap
  "cleanup_policy": "ttl_72h"            // "on_completion", "ttl_1h", "ttl_24h", "ttl_48h", "ttl_72h" (default), "retain"
}
```

**Response (202):**
```json
{
  "ok": true,
  "runId": "adhoc_20260416T143022_0001",
  "workflowId": "_adhoc_20260416T143022_0001",
  "cleanup_policy": "ttl_72h"
}
```

**No `scripts` field.** Submitted JCWFs can only reference scripts that already exist under `scripts/`. This is a hard security boundary — external callers cannot inject executable code.

### AI Call Throttling

AI backend throttling is handled globally by the engine at two levels:

**Level 1 — Reactive (already implemented):** `CurlMultiDispatcher` reads `x-ratelimit-remaining-requests`, `x-ratelimit-remaining-tokens`, and `x-ratelimit-reset-requests` headers from AI provider responses. On HTTP 429, it auto-retries with exponential backoff (1s, 2s, 4s, 8s, 16s — up to 5 retries), preferring the provider's `reset-requests` delay when available. Per-host rate limit state is tracked in `HostRateLimitState`. This means j9t already respects whatever the AI backend reports it can accept — if the backend says "slow down," j9t slows down.

**Level 2 — Proactive inflight cap:** `max inflight ai calls` in config.json caps concurrent in-flight requests submitted to `CurlMultiDispatcher`. There is also a per-host stream limit of 48 (`kMaxActivePerHost`) for HTTP/2 multiplexing. The current default is 100 — we will **bump the default to 1000** since the reactive 429 handling is the true backstop against overwhelming the backend. The clamp range [1, 1000] in `configChecker.cpp` must be widened accordingly.

**New config.json field: `max_ai_calls_per_jcwf`**

An optional cap on the **total** number of AI calls a single JCWF run can make. This is a **per-run safety limit**, independent of the AI backend's capacity:

| `max_ai_calls_per_jcwf` | Behavior |
|--------------------------|----------|
| Not set / `0` | No cap — the run can make as many AI calls as the JCWF defines |
| `1000` (our test value) | If a run's total AI call count exceeds 1000, additional calls are rejected with a task-level error |

This prevents a single runaway JCWF (especially adhoc) from consuming the entire AI budget. The engine enforces this in `WorkflowRuntimeManager` — it counts AI calls dispatched per run and refuses to dispatch beyond the cap.

### Adhoc Directory Structure

Each adhoc run gets its own **self-contained folder** with `workflows/` and `queue/` subfolders. This mirrors the top-level j9t directory structure and ensures adhoc runs don't interfere with registered workflows or each other.

```
_adhoc/
  20260416T143022_0001/           ← one adhoc run
    workflows/
      _adhoc_20260416T143022_0001.jcwf    ← the submitted JCWF (zip container)
      _adhoc_20260416T143022_0001/        ← extracted container
        canvas.json
    queue/
      _adhoc_20260416T143022_0001/        ← runtime task folders, outputs
        ai_call_1/
        shell_1/
        ...
  20260416T150500_0002/           ← another adhoc run
    workflows/
    queue/
```

**Why this structure:**
- **Isolation** — each adhoc run is fully self-contained. No risk of cross-contamination between runs or with registered workflows.
- **Existing JCWF semantics preserved** — the JCWF's `working_directory` and file path resolution work identically because the workflow base directory (`workflows/`) is the root, same as for registered workflows.
- **Easy cleanup** — deleting `_adhoc/20260416T143022_0001/` removes everything for that run.
- **Restart-safe** — the folder name encodes timestamp + counter (see below), so j9t can discover and clean up stale adhoc runs after a restart.

**Folder naming: `<ISO8601-compact>_<counter>_<delete-at>`**

- Timestamp: `YYYYMMDDTHHMMSS` (UTC) — sorts chronologically, survives restarts.
- Counter: 4-digit zero-padded, monotonically increasing per j9t instance lifetime, resets on restart. Prevents collisions when multiple adhoc runs are submitted in the same second.
- Delete-at: `del-YYYYMMDDTHHMMSS` or `del-retain` — the absolute UTC time at which this folder should be deleted. Encoded in the folder name so the reaper can make cleanup decisions without any in-memory state, surviving j9t restarts.
- On startup, j9t scans `_adhoc/` for existing folders to set the counter floor (max existing counter + 1).

Example folder names:
```
20260416T143022_0001_del-20260419T143022     ← 72h TTL (default)
20260416T150500_0002_del-20260418T150500     ← 48h TTL
20260416T160000_0003_del-retain              ← never auto-cleaned
```

### Artifact Retention

The retention period is **configurable per user** in the MCP key settings. The default is **72 hours**.

| `cleanup_policy` | When Cleaned Up | Use Case |
|-------------------|-----------------|----------|
| `on_completion` | Immediately after run completes (succeeded, failed, or cancelled) | Fire-and-forget runs where only the API response matters |
| `ttl_1h` | 1 hour after run completion | Quick inspection |
| `ttl_24h` | 24 hours after run completion | Same-day review |
| `ttl_48h` | 48 hours after run completion | Next-day review |
| `ttl_72h` (default) | 72 hours after run completion | Multi-day review, weekend coverage |
| `retain` | Never auto-cleaned — admin must delete manually or via API | Audit trail, long-term results |

The user's configured default retention policy is stored in their MCP key record (`default_cleanup_policy`). Individual adhoc submissions can override with a shorter or equal TTL, but **cannot exceed** the user's configured maximum (admin sets the ceiling).

**Reaper thread:** `AdhocCleanupScheduler` runs every 60 seconds, scans `_adhoc/` folders, reads the `del-<timestamp>` suffix from each folder name. If the current time is past the delete-at timestamp, the folder is removed. For `on_completion`, cleanup happens inline at the end of `TickActiveRun()` (no reaper delay) and the delete-at timestamp in the folder name is set to the run completion time.

**On j9t restart:** The reaper scans `_adhoc/` and applies the same logic — the delete-at timestamp is in the folder name, so no in-memory state is needed. Folders with `del-retain` are preserved indefinitely. Folders whose `del-<timestamp>` has passed are cleaned up. No special handling for orphaned folders — the timestamp-in-name design makes them self-describing.

### Disk Quota

**Per-user disk quota** configured in user management (MCP key settings), default **1 GB**. The `AdhocWorkflowManager` tracks cumulative disk usage per MCP key user across all their active adhoc folders. Submissions that would exceed the quota are rejected with HTTP 413 `"quota_exceeded"`.

```json
// In the MCP key record:
{
  "key_id": "mcp_a1b2c3d4",
  "user": "alice@company.com",
  "role": "operator",
  "adhoc_enabled": true,
  "disk_quota_mb": 1024,
  "default_cleanup_policy": "ttl_72h",
  ...
}
```

### Access Control

Adhoc submission is **not granted by default**. It requires:

1. An MCP key with `adhoc_enabled: true` (checkbox in user management, default **off**).
2. The key must have at least `operator` role (viewers cannot submit).
3. Admin creates the key and explicitly enables adhoc access.

This means a new MCP user cannot submit adhoc workflows until an admin checks the box. Defense in depth.

### Required Backend Functions

| Function | Location | Purpose |
|----------|----------|---------|
| `HandleAdhocRunPost()` | `webServer.cpp` | Route handler — validates, stages, executes |
| `AdhocWorkflowManager` | new: `adhocWorkflowManager.h/.cpp` | Staging, cleanup, quota tracking, folder naming |
| `AdhocCleanupScheduler` | same | Background reaper thread (60s interval) |
| `max_ai_calls_per_jcwf` enforcement | `workflowRuntimeManager.cpp` | Per-run AI call counter + cap |

### Execution Flow

1. **Auth check** — verify MCP key has `adhoc_enabled: true` and sufficient role.
2. **Quota check** — verify user's cumulative disk usage is below quota.
3. **Validate** — run `WorkflowValidator` on the submitted JCWF. Reject with 400 if Tier A errors. Verify all referenced scripts exist on disk.
4. **Stage** — create `_adhoc/<timestamp_counter>/workflows/` and `queue/`, write JCWF container.
5. **Register temporarily** — add to `WorkflowRegistry` as a transient workflow.
6. **Execute** — dispatch through `WorkflowRuntimeManager` like any other run. The `max_ai_calls_per_jcwf` cap is enforced.
7. **Monitor** — standard `GET /api/workflow-runs/<runId>` works normally.
8. **Cleanup** — based on policy: immediate, TTL-based (reaper), or retain.

### Security Constraints

- **No script submission** — the JCWF can only reference existing scripts. This is the primary security boundary.
- **Adhoc requires explicit admin grant** — `adhoc_enabled` checkbox, default off.
- **Per-user disk quota** — default 1 GB, configurable per key.
- **Per-run AI cap** — `max_ai_calls_per_jcwf` in config.json (test value: 1000).
- **Rate limiting** — adhoc submissions count against existing per-IP and per-key rate limits.
- **Audit logging** — every adhoc submission logged to `log/security.txt` with user identity, IP, JCWF hash, cleanup policy, and folder path.
- **Docker recommended** — for production deployments handling untrusted adhoc JCWFs, Docker containment limits blast radius.

### What Adhoc Submission Unlocks

| Capability | Description |
|------------|-------------|
| **Agent-composed workflows** | An AI agent generates a JCWF and submits it for execution, monitors progress, retrieves results — a full agent loop. Scripts must be pre-deployed. |
| **Ephemeral data pipelines** | One-off processing jobs that don't clutter the workflow registry. |
| **CI/CD integration** | Test harnesses submit workflows, verify outputs, clean up automatically. |
| **Self-healing agents** | Agent detects a failed task, generates a fix JCWF using existing scripts, submits it adhoc. |

---

## 3. MCP Configure-Plane Tools

### New MCP Tools — 1.0 Scope

These tools proxy to existing (or new) REST endpoints, enabling MCP clients to be fully self-sufficient.

| MCP Tool | REST Endpoint | Min Role | Description |
|----------|---------------|----------|-------------|
| `manage_connections` | `GET/POST/PUT/DELETE /api/connections` | admin | CRUD cloud connections |
| `test_connection` | `POST /api/connections/<name>/test` | admin | Verify connectivity |
| `manage_keys` | `GET/POST/PUT/DELETE /api/settings/providers` | admin | CRUD credential providers |
| `upload_workflow` | `POST /api/workflows` | admin | Submit a JCWF for permanent registration |
| `validate_workflow` | `POST /api/workflows/validate` | operator | Validate JCWF without saving |
| `run_adhoc_workflow` | `POST /api/workflows/run-adhoc` | operator + `adhoc_enabled` | Submit + execute ephemeral workflow |
| `get_run_logs` | `GET /api/log?tail=N` | operator | Tail application log |
| `whoami` | `GET /api/auth/whoami` | any | Return identity, role, key metadata, quota |

### Backend Work Required

| What | Effort | Notes |
|------|--------|-------|
| `POST /api/workflows/run-adhoc` | **Medium** | New endpoint + AdhocWorkflowManager (see section 2) |
| `GET /api/auth/whoami` | **Small** | New endpoint, reads from `Authenticate()` result |
| `max_ai_calls_per_jcwf` config field | **Small** | Parse in `configParser.cpp`, enforce in runtime manager |
| MCP tool implementations | **Small per tool** | TypeScript in `mcp/src/tools.ts`, follows existing pattern |
| MCP auth layer | **Medium-Large** | See section 4 — this is the big item |

---

## 4. MCP Authorization — Enterprise Grade

### Design: MCP API Keys with Per-User Identity

#### Concept

Introduce **MCP API Keys** — named, per-user credentials that carry an identity and a role. Each user gets their own key with their own permissions. The existing engine admin token is replaced by this system.

**Critical security property: the admin never sees the user's final key.** This follows the industry-standard enrollment token pattern (used by HashiCorp Vault, Auth0, AWS IAM).

#### Data Model

```
MCP API Key:
  - key_id:                "mcp_a1b2c3d4"          (unique, prefix "mcp_" for identification)
  - key_hash:              SHA-256(raw_key)         (stored, never the raw key)
  - user:                  "alice@company.com"      (identity for audit trail)
  - role:                  "operator"               (admin | operator | viewer)
  - adhoc_enabled:         false                    (default off — admin must enable)
  - disk_quota_mb:         1024                     (default 1 GB)
  - default_cleanup_policy: "ttl_72h"               (default adhoc retention)
  - created_at:            "2026-04-16T10:00:00Z"
  - expires_at:            "2026-07-16T10:00:00Z"  (90-day default, configurable)
  - last_used_at:          "2026-04-16T14:30:00Z"
  - enabled:               true
  - description:           "Alice's Claude Code key"
```

#### Storage: `mcp_keys.json.enc`

MCP API keys are stored in `mcp_keys.json.enc` — **encrypted with the master password**, same mechanism as `keys.json.enc` (AES-256-GCM with PBKDF2 key derivation). This protects not only the key hashes but also the user metadata (emails, roles, quotas).

- Requires master password to decrypt on startup (same unlock flow as the existing key store).
- The raw key is shown exactly once — to the **user**, not the admin (see enrollment flow below).
- On disk, only the encrypted blob exists.

#### Master Password Memory Protection

After the master password is entered (at startup or via the unlock endpoint), j9t must hold it in memory to encrypt/decrypt `mcp_keys.json.enc` and `keys.json.enc` on the fly (key creation, rotation, etc.). Memory protections:

- **`mlock()`** — lock the memory page containing the master password to prevent it from being swapped to disk (`mlock()` on Linux/macOS, `VirtualLock()` on Windows). Swap files are readable by root, so unswapped memory is a meaningful defense.
- **`SecureString` wrapper** — a C++ RAII class that holds the password in a `mlock()`-ed buffer and zeroes it on destruction (`explicit_bzero()` / `SecureZeroMemory()`). Prevents the password from lingering in freed memory.
- **No logging** — the master password is never logged, not even at debug level. The `SecretRedactor` registers it on entry.
- **Limitation:** A process with `ptrace` access (root, or a debugger attached to j9t) can always read process memory. This is an OS-level threat that no userspace application can fully prevent. The mitigations above raise the bar significantly but do not claim to be unhackable — that's what Docker isolation and private subnets are for.

#### Key Provisioning: Enrollment Token Pattern

The admin **never sees the user's final MCP API key**. Instead, the admin creates a short-lived enrollment token, and the user exchanges it for their real key.

**Flow:**

```
1. Admin creates enrollment for alice@company.com
   POST /api/auth/mcp-keys/enroll
   { "user": "alice@company.com", "role": "operator", "adhoc_enabled": true, ... }

   Response: { "enrollment_token": "enroll_x9y8z7...", "expires_in_minutes": 30 }
   (admin shares this token with Alice via secure channel — Slack DM, email, in-person)

2. Alice exchanges the enrollment token for her real key
   POST /api/auth/mcp-keys/activate
   { "enrollment_token": "enroll_x9y8z7..." }

   Response: { "key_id": "mcp_a1b2c3d4", "api_key": "mcp_a1b2c3d4e5f6g7h8...", "user": "alice@company.com", "role": "operator" }
   (Alice copies the key into her Claude Code config — shown exactly once, never again)

3. j9t stores only the SHA-256 hash of the key in mcp_keys.json.enc
   The admin never saw "mcp_a1b2c3d4e5f6g7h8..." — only the enrollment token, which is now expired
```

**Enrollment token properties:**
- Prefix `enroll_` for identification (not a valid MCP key — cannot be used for API access).
- 256-bit random, single-use, expires after 30 minutes (configurable).
- Stored as a hash in the key store until activated or expired.
- If not activated within the TTL, it is automatically purged.
- Audit logged: creation (by admin), activation (by user), expiry (automatic).

**Key rotation** follows the same pattern: admin issues a new enrollment token, user activates it, old key enters 24h grace period.

#### Authentication Flow

```
MCP Client (Claude Code, etc.)
    |
    |  J9T_TOKEN=mcp_a1b2c3d4e5f6...  (MCP API key)
    |
j9t MCP Server
    |
    |  Authorization: Bearer mcp_a1b2c3d4e5f6...
    |
j9t (Engine or Studio)
    |
    |  1. Detect "mcp_" prefix -> look up in MCP key store
    |  2. Hash the key -> compare against key_hash (constant-time)
    |  3. Check enabled, not expired
    |  4. Extract user + role + adhoc_enabled + disk_quota_mb
    |  5. Enforce RBAC on the requested endpoint
    |  6. Audit log: "alice@company.com (operator) via MCP: POST /api/workflows/run"
```

#### Key Lifecycle

| Action | Trigger | What Happens |
|--------|---------|-------------|
| **Enroll** | Admin creates enrollment via Settings UI or API | Enrollment token generated (30-min TTL, single-use), admin shares with user |
| **Activate** | User calls `/api/auth/mcp-keys/activate` with enrollment token | Real MCP key generated and shown to user once; hash stored; enrollment token invalidated |
| **Rotate** | Admin issues new enrollment token for the same user | User activates new key, old key enters 24h grace period then disabled |
| **Revoke** | Admin clicks "Revoke" | Key immediately disabled, all subsequent requests rejected |
| **Expire** | Expiry date reached | Key rejected with `"token_expired"` (403), response includes renewal instructions, admin notified |
| **Auto-disable** | Key unused for N days (configurable) | Key auto-disabled, admin notified |

#### Key Expiry Notification and Self-Renewal

When a key is within 30 days of expiry, j9t adds headers to every API response authenticated with that key:

```
X-Key-Expires-In: 5d
X-Key-Self-Renew: POST /api/auth/mcp-keys/self-renew
```

A startup log warning is also emitted: `MCP key mcp_a1b2 for alice@company.com expires in 5 days`.

**Self-service renewal (no admin needed):**

As long as the key is still valid (not yet expired), the user can renew it themselves:

```
POST /api/auth/mcp-keys/self-renew
Authorization: Bearer mcp_a1b2c3d4e5f6...   (current key, still valid)

Response (200):
{
  "ok": true,
  "key_id": "mcp_x9y8z7w6",
  "api_key": "mcp_x9y8z7w6v5u4t3...",
  "expires_at": "2026-10-14T10:00:00Z",
  "message": "New key activated. Old key remains valid for 24 hours. Update your config now."
}
```

Self-renewal properties:
- Only works if the current key is **still valid** — authenticated request.
- Inherits the same user, role, adhoc_enabled, quota — no privilege escalation possible.
- Generates a new 90-day expiry from now.
- Old key enters 24-hour grace period, then is disabled.
- The admin is not involved and never sees the new key.
- Audit logged: `alice@company.com self-renewed mcp_a1b2 -> mcp_x9y8`

**Expired key (admin intervention required):**

If the user missed the renewal window and the key actually expired, the 403 response tells them to contact their admin:

```json
{
  "ok": false,
  "error": "token_expired",
  "message": "Your MCP key has expired. Contact your j9t admin to issue a new enrollment token, then activate it: POST /api/auth/mcp-keys/activate {\"enrollment_token\": \"<token>\"}."
}
```

| Scenario | Who acts | Endpoint |
|----------|----------|----------|
| Key valid, within 30 days of expiry | **User** (self-service) | `POST /api/auth/mcp-keys/self-renew` |
| Key expired | **Admin** issues enrollment token, user activates | `POST /api/auth/mcp-keys/enroll` + `POST /api/auth/mcp-keys/activate` |

#### REST Endpoints for Key Management

```
GET    /api/auth/mcp-keys                       -- List all MCP keys (admin only, keys redacted)
POST   /api/auth/mcp-keys/enroll                -- Create enrollment token (admin only, returns enrollment token)
POST   /api/auth/mcp-keys/activate              -- Exchange enrollment token for real key (no auth required)
POST   /api/auth/mcp-keys/self-renew            -- Renew own key before expiry (any MCP key holder, returns new key)
PUT    /api/auth/mcp-keys/<key_id>              -- Update key metadata (role, description, enabled, adhoc, quota) (admin only)
DELETE /api/auth/mcp-keys/<key_id>              -- Revoke/delete a key (admin only)
GET    /api/auth/whoami                         -- Return identity, role, key metadata for current auth
```

Note: `/activate` requires no auth (the enrollment token *is* the auth). This enables users to self-service without needing an existing MCP key.

#### Gateway Integration

In gateway deployments, the MCP API key system is **complementary** to gateway auth:
- The gateway authenticates the user (OIDC/SAML/MFA) and injects `X-Forwarded-User` and `X-Forwarded-Role`.
- The MCP key provides a second layer: even if the gateway is misconfigured, the MCP key constrains what the user can do.
- In non-gateway deployments (e.g. MCP over stdio on a dev machine), the MCP key is the *primary* auth mechanism.

---

## 5. Settings UI Consolidation

### Clean Up the Nav Bar

Currently configuration is spread across multiple navigation destinations. Consolidate all config dialogs into the **Settings modal** (gear icon), organized as tabs:

| Tab | Contents | Previously Located |
|-----|----------|--------------------|
| **General** | Default AI Interface, Max Threads, Max File Size, JCWF Batch Size, Verbose, max_ai_calls_per_jcwf | Settings modal (existing) |
| **AI Interfaces** | AI provider CRUD, test buttons, LED indicators | Separate "AI Manager" page |
| **Connections** | Cloud connection CRUD, test buttons | Separate "Connections" page |
| **Keys** | API key providers, master password, encrypt/save | Separate "Keys" page |
| **MCP Keys** | MCP API key management (create, rotate, revoke, enable/disable, adhoc checkbox, disk quota) | New |
| **About** | Version, license, GitHub link | Settings modal footer (existing) |

**Result:** The nav bar becomes cleaner — only functional pages (Dashboard, Workflows, Log, Editor, Assistant) remain. All configuration lives under the gear icon.

### MCP Keys Tab

#### Key List View

| Column | Content |
|--------|---------|
| Status | Green dot (active), grey dot (disabled), red dot (expired) |
| Key ID | `mcp_a1b2c3d4` (prefix shown, rest redacted) |
| User | `alice@company.com` |
| Role | Badge: Admin / Operator / Viewer |
| Adhoc | Checkbox icon (enabled/disabled) |
| Quota | `1.0 GB` / `512 MB` / etc. |
| Last Used | Date + "X ago" relative, or "Never" |
| Actions | Rotate, Disable/Enable, Delete |

#### Create Enrollment Dialog

Fields:
- **User** (required) — email or username, free text
- **Role** (required) — dropdown: admin, operator, viewer
- **Allow adhoc submission** — checkbox, default **off**
- **Disk quota** — number input with unit selector (MB/GB), default **1 GB**
- **Default retention** — dropdown: on_completion, 1h, 24h, 48h, **72h** (default), retain
- **Description** (optional) — free text
- **Key expires in** — dropdown: 30 days, 90 days (default), 180 days, 1 year, custom

On creation:
1. Backend generates a single-use enrollment token (30-min TTL).
2. Dialog shows the enrollment token in a copy-to-clipboard field with instructions: *"Share this token with the user. They have 30 minutes to activate it. You will not see their final API key."*
3. Audit log: `Enrollment token created for alice@company.com (operator, adhoc=off) by admin@company.com`

The user then calls `POST /api/auth/mcp-keys/activate` with the enrollment token to receive their actual MCP API key. The admin never sees it.

#### Key Detail / Edit Modal

- Change role, description, enabled state, adhoc checkbox, disk quota, retention policy.
- Re-enroll button (generates new enrollment token for the same user — user activates to get a new key, old key enters 24h grace period).
- Last-used timestamp and usage count.
- Current disk usage display (for adhoc-enabled keys).

---

## 6. Dashboard Updates

### Adhoc Run Display

The dashboard should show adhoc workflow runs alongside registered workflow runs. Two UI additions:

#### Rolling Last-3-Runs Display

A compact rolling display in the dashboard header or status bar showing the **last 3 runs** (both adhoc and registered):

```
 Last runs:  [OK] jarvisCppDocu (12s ago)  |  [OK] _adhoc_20260416T143022_0001 (45s ago)  |  [FAIL] emailDemo (2m ago)
```

- 3 lines maximum, scrolls as new runs complete.
- Each entry shows: status icon (green check / red X / yellow spinner), workflow ID (or adhoc ID), and time since completion.
- Adhoc runs are visually distinguished (e.g. italic text or a small "adhoc" badge).

#### Minimum Display Time

Adhoc runs that complete very quickly (sub-second) must remain visible in the dashboard for **at minimum 2 seconds** so the user can see them. The run snapshot broadcast holds the entry for 2 seconds before it's eligible for removal from the active runs display.

### Status Page Updates

- Show MCP key count: active / expired / disabled.
- Show adhoc run stats: active count, total completed, total disk usage.

---

## 7. Unified MCP Security — Both Editions

### MCP Has the Same Security in Both Editions

MCP is a programmatic interface — it can be automated, scripted, and potentially exploited remotely (especially with SSE transport). Unlike browser UI access on localhost, MCP access carries real risk even on a developer workstation. Therefore, **MCP requires authentication in both Studio and Engine**, using the same MCP API key system.

### Design

| Aspect | Studio | Engine |
|--------|--------|--------|
| **Browser UI** | No auth (localhost) | Bearer token / gateway |
| **MCP access** | **MCP key required** | **MCP key required** |
| **MCP key store** | `mcp_keys.json.enc` | `mcp_keys.json.enc` |
| **Enrollment flow** | Same (enroll + activate) | Same (enroll + activate) |
| **RBAC** | Same 3 roles | Same 3 roles |
| **Audit logging** | `log/security.txt` | `log/security.txt` |
| **WebSocket `/ws`** | No auth (browser UI) | Token-as-first-message |
| **REST API (non-MCP)** | No auth (localhost) | Bearer token / gateway / MCP keys |

### Studio First-Run Experience

On first Studio startup, j9t creates a default admin enrollment token and logs it to stdout:

```
[j9t] MCP enrollment token (admin): enroll_x9y8z7w6...
[j9t] Activate within 30 minutes: POST /api/auth/mcp-keys/activate {"enrollment_token":"enroll_x9y8z7w6..."}
[j9t] Or use the Settings > MCP Keys tab to create additional keys.
```

The developer activates this token (via curl or the MCP sidecar's built-in activate command) to get their MCP API key. This is a one-time setup. Additional keys can be created via the Settings > MCP Keys tab.

If the enrollment token expires without activation, the developer can create a new one from the Settings UI (since browser UI access remains open on localhost).

### What Stays Open in Studio (Browser UI)

- `GET /api/status` — health check, no auth.
- All browser-accessible REST endpoints — no auth (localhost assumption for browser UI).
- WebSocket `/ws` and `/ws/assistant` — no auth (browser UI).
- Settings UI including MCP Keys tab — accessible from the browser to manage keys.

### What Requires MCP Key in Both Editions

- Any request with `Authorization: Bearer mcp_...` header — validated against MCP key store.
- The MCP sidecar must be configured with a key to connect to either edition.
- All MCP-authenticated requests are audit-logged with user identity and role.

---

## 8. Cyber Security Impact

### New Threat Surface

| Threat | Severity | Mitigation |
|--------|----------|------------|
| **Adhoc JCWF abuse** — submitted JCWF references scripts to exfiltrate data or abuse resources | High | No script submission; scripts must pre-exist; admin-only adhoc grant; per-user disk quota; per-run AI cap; audit logging |
| **MCP key theft** — a leaked MCP key grants the role's full access | High | Key expiry (90d default), auto-disable on inactivity, immediate revocation via UI, audit trail, encrypted key store |
| **MCP key brute force** — attacker tries random MCP key values | Medium | Rate limiting + auth lockout apply. MCP keys are 256-bit = computationally infeasible to guess |
| **Adhoc disk exhaustion** — malicious adhoc submissions fill disk | Medium | Per-user disk quota (default 1 GB), TTL-based cleanup, admin-only adhoc grant |
| **Privilege escalation via MCP** — viewer key used to run workflows | Low | RBAC enforced at j9t route level. MCP key role checked by `Authenticate()` |
| **MCP sidecar compromise** — attacker gains control of the TypeScript process | Medium | Sidecar only has one bearer token (its own MCP key). Blast radius limited to that key's role and quota |

### Updates to `doc/cyber security.md`

The following sections need updating:

1. **MCP Security** — expand from the current 4-line paragraph to a full section covering:
   - MCP API key model (encrypted store, per-user identity, RBAC)
   - Key lifecycle (create/rotate/revoke/expire)
   - Adhoc submission security (no scripts, quota, AI cap)
   - Audit logging of MCP operations
   - Stdio vs SSE transport security
   - **Same security in both editions**

2. **Adhoc Workflow Submission** — new section covering:
   - No script submission (hard boundary)
   - Per-user disk quota
   - Per-run AI call cap
   - Cleanup policies and artifact retention
   - Folder isolation model

3. **Editions at a Glance** table — add rows for MCP keys (both editions), dashboard login (Engine), and adhoc submission.

4. **Endpoint role requirements** table — add adhoc and MCP key management endpoints.

5. **Recommended Deployment Architecture** — update diagram to show MCP sidecar placement and per-user key distribution.

### Enterprise Deployment with MCP Keys

```
                                +-----------------------------+
                                |  API Gateway                |
                                |  (OIDC + MFA + role mapping) |
                                +----------+------------------+
                                           |
    +--------------------------------------+----------------------------------+
    |  Private Subnet                      |                                  |
    |                                      v                                  |
    |  +----------------------+    +------------------+                       |
    |  |  MCP Sidecar         |    |  j9t Engine       |                      |
    |  |  (per-user MCP key)  |--->|  (RBAC enforced)  |                      |
    |  |                      |    |  (audit logging)  |                      |
    |  +----------------------+    +------------------+                       |
    |       ^                                                                 |
    |       |  stdio / SSE                                                    |
    |       |                                                                 |
    |  +----+-------------+                                                   |
    |  |  Claude Code /   |                                                   |
    |  |  Claude Desktop  |   <- each user has their own MCP key              |
    |  |  / Custom Agent  |     with their identity, role, and quota          |
    |  +------------------+                                                   |
    |                                                                         |
    +-------------------------------------------------------------------------+
```

**Two deployment patterns:**

| Pattern | Auth Flow | Use Case |
|---------|-----------|----------|
| **Gateway + MCP keys** | User -> OIDC/MFA -> gateway -> `X-Forwarded-User/Role` -> j9t. MCP key is a secondary credential. | Browser users + MCP agents in the same deployment |
| **MCP keys only** | MCP client -> `Authorization: Bearer mcp_...` -> j9t. No gateway needed. | Dedicated agent deployment, internal tooling, Studio dev |

Both patterns produce per-user audit trails in `log/security.txt`.

---

## 9. What These Features Unlock

### With Adhoc Submission

| Capability | Description |
|------------|-------------|
| **Agent-composed workflows** | An AI agent generates a JCWF and submits it for execution using pre-deployed scripts, monitors progress, retrieves results — a full agent loop. |
| **Ephemeral data pipelines** | One-off processing jobs that don't clutter the workflow registry. |
| **CI/CD integration** | Test harnesses submit workflows, verify outputs, clean up automatically. |
| **Self-healing agents** | Agent detects a failed task, generates a fix JCWF using existing scripts, submits it adhoc. |

### With Configure-Plane MCP

| Capability | Description |
|------------|-------------|
| **Fully autonomous MCP agents** | An agent can set up connections, upload workflows, run them, and read logs — no curl, no human. |
| **Self-provisioning** | A new agent can bootstrap its own environment: create connections, upload its workflows, start running. |
| **Observability** | `get_run_logs` lets agents self-debug without human log inspection. |
| **Identity-aware tooling** | `whoami` lets an agent verify its own permissions before attempting restricted operations. |

### With MCP Keys

| Capability | Description |
|------------|-------------|
| **Per-user accountability** | Security log shows *who* ran *what* via MCP, not just "bearer token used". |
| **Least-privilege access** | Give a CI bot `operator` (can run, cannot configure). Give a monitoring agent `viewer`. |
| **Instant revocation** | Compromised key? Disable it in the UI. No disruption to other users. |
| **Compliance** | SOC 2, ISO 27001 controls for access management, credential rotation, and audit trails. |

### Combined: j9t as an AI Execution Platform

With all three features, j9t becomes a **programmable orchestration layer for autonomous AI agents**:

1. Agent discovers available workflows (`list_workflows`)
2. Agent provisions what it needs (`manage_connections`, `manage_keys`)
3. Agent composes a new workflow (`run_adhoc_workflow`) or runs an existing one (`run_workflow`)
4. Agent monitors execution (`get_run_status`, `get_run_logs`)
5. Agent inspects results and iterates
6. Every step is audit-logged with the agent's identity and role
7. Per-user quotas and AI caps prevent resource abuse

This is the **AI Assistant Generator** from the post-1.0 roadmap, but the infrastructure is laid in 1.0.

---

## 10. MCP Tools — 1.0 Deliverables

| Category | Tools |
|----------|-------|
| **Configure Plane** | `manage_connections`, `test_connection`, `manage_keys`, `upload_workflow`, `validate_workflow` |
| **Adhoc Workflows** | `run_adhoc_workflow` with per-user quota, AI cap, no script submission, folder isolation |
| **Observability** | `get_run_logs` (tail application log) |
| **Security / Audit** | `whoami` (identity, role, quota, adhoc status) |
| **Debug (stub)** | `debug_signals` — live engine internals, compiled only with `DEBUG` flag |

### Debug MCP Tool (Stub)

`debug_signals` exposes live engine internals for development and diagnostics. Compiled only when the `DEBUG` preprocessor flag is set — stripped from release builds entirely.

**MCP tool:**
```typescript
server.tool("debug_signals", "Live engine debug signals (DEBUG builds only)", async () => { ... });
```

**REST endpoint:** `GET /api/debug/signals` (DEBUG builds only, admin role required)

**Example response:**
```json
{
  "ok": true,
  "signals": {
    "ai_calls_inflight": 42,
    "ai_calls_queued": 18,
    "ai_calls_total_dispatched": 1337,
    "http2_streams_active_per_host": { "api.openai.com": 38, "generativelanguage.googleapis.com": 4 },
    "python_engines_busy": 3,
    "python_engines_total": 4,
    "python_queue_depth": [0, 2, 1, 0],
    "thread_pool_active": 12,
    "thread_pool_size": 20,
    "workflow_runs_active": 2,
    "workflow_runs_paused": 0,
    "adhoc_runs_active": 1,
    "adhoc_disk_usage_bytes": 52428800,
    "websocket_clients": 3,
    "mcp_keys_loaded": true,
    "keys_unlocked": true,
    "curl_multi_retry_queue": 0,
    "rate_limit_buckets_tracked": 5,
    "uptime_seconds": 3847
  }
}
```

This is a stub for 1.0 — signals will be expanded as needed during development. The endpoint collects values from existing in-memory counters (`m_InFlightCount`, `m_ClientCount`, `PythonEnginePool` queue depths, etc.) with no additional tracking overhead.

---

## 11. Dashboard Login — Enterprise Mode

### Problem

Engine edition serves the dashboard UI (`/`, `/dash-assets/*`) without authentication. Anyone who can reach port 8443 sees the full dashboard — workflow list, run status, logs. For enterprise deployment this is unacceptable.

### Design

Engine edition requires login. Studio stays open on localhost (standard developer workstation model).

Users authenticate with their **MCP API key** — the same credential they use for Claude Code. One identity system, not two.

#### Login Flow

```
Browser -> opens https://j9t.company.com:8443/
        -> j9t serves login page (static HTML, no auth required)
        -> user enters their MCP key
        -> POST /api/auth/login { "api_key": "mcp_a1b2c3d4..." }
        -> j9t validates key, creates server-side session
        -> returns HttpOnly + Secure + SameSite=Strict cookie
        -> browser stores cookie, all subsequent requests include it
        -> j9t validates cookie on every request, enforces RBAC
```

#### Gateway Login Flow

When deployed behind an API gateway (OIDC/SAML/MFA), the gateway handles authentication. j9t creates a session from the gateway-injected identity headers — no MCP key needed for browser login:

```
Browser -> opens https://j9t.company.com/
        -> gateway intercepts, redirects to SSO (e.g. Okta, Azure AD)
        -> user authenticates with company credentials + MFA
        -> gateway injects X-Forwarded-User + X-Forwarded-Role
        -> j9t creates session from gateway identity
        -> returns session cookie
```

#### Session Properties

| Property | Value |
|----------|-------|
| **Storage** | Server-side: `SessionManager` holds session map in memory (not persisted — all sessions invalidate on restart) |
| **Cookie flags** | `HttpOnly` (no JavaScript access), `Secure` (HTTPS only), `SameSite=Strict` (no cross-site) |
| **Lifetime** | 8 hours default (configurable via `"session_timeout_hours"` in config.json) |
| **Idle timeout** | Sliding window — each request resets the expiry clock |
| **Logout** | Explicit logout button: `POST /api/auth/logout` destroys the session |
| **Session ID** | 256-bit random, regenerated on login (prevent session fixation) |

#### RBAC in the Dashboard UI

The dashboard reads the user's role from the session and hides UI elements accordingly:

| Role | Dashboard Access |
|------|-----------------|
| **Viewer** | Workflow list (read-only), run monitoring, status |
| **Operator** | + Run control (pause/resume/stop/cancel), application log |
| **Admin** | + Settings (AI interfaces, connections, keys, MCP keys), security log, shutdown |

The backend enforces RBAC regardless of what the frontend shows — the UI gating is cosmetic, the route-level check is authoritative.

#### WebSocket Auth

For browser sessions, the WebSocket upgrade validates the session cookie (replaces the current token-as-first-message pattern for browser clients). MCP clients still use bearer token auth on the first WebSocket message.

| Client | WebSocket Auth |
|--------|---------------|
| Browser (Engine) | Session cookie validated on `/ws` upgrade |
| Browser (Studio) | No auth (localhost) |
| MCP sidecar | Bearer token as first message (existing, unchanged) |

#### Public Pages (No Auth Required)

These are accessible without login in Engine mode:

- Login page HTML (`/login`, static)
- `GET /api/status` — health check
- `POST /api/auth/mcp-keys/activate` — enrollment token activation (the token *is* the auth)
- `POST /api/auth/login` — login endpoint itself

Everything else requires a valid session cookie or MCP bearer token.

#### Edition Comparison

| Aspect | Studio | Engine |
|--------|--------|--------|
| **Dashboard** | Open (localhost) | Login required |
| **Login method** | N/A | MCP key or gateway SSO |
| **Session cookie** | N/A | HttpOnly + Secure + SameSite=Strict |
| **MCP access** | MCP key required | MCP key required |
| **WebSocket** | Open | Session cookie or bearer token |

---

## 12. Implementation Phases

### Phase 1 — MCP API Keys + Dashboard Login + Unified Security

**Backend:**
- `McpKeyManager` class (`application/web/mcpKeyManager.h/.cpp`) — encrypted key store CRUD, hash verification, expiry check, quota tracking
- `SecureString` class — `mlock()`-ed buffer for master password with `explicit_bzero()` on destruction
- `mcp_keys.json.enc` file format — encrypted with master password (AES-256-GCM + PBKDF2)
- Enrollment token system — `POST /api/auth/mcp-keys/enroll` (admin) + `POST /api/auth/mcp-keys/activate` (user, no auth)
- Self-renewal — `POST /api/auth/mcp-keys/self-renew` (any valid MCP key holder)
- Update `Authenticate()` in `webServer.cpp` — detect `mcp_` prefix, route to McpKeyManager (both editions)
- `GET /api/auth/whoami` endpoint
- Dashboard login — `POST /api/auth/login`, `POST /api/auth/logout`, `SessionManager` with server-side sessions
- Session cookie auth — `HttpOnly` + `Secure` + `SameSite=Strict`, 8h sliding timeout
- WebSocket upgrade validates session cookie (Engine browser clients)
- `X-Key-Expires-In` response header on MCP-authenticated requests within 30 days of expiry
- Security audit logging for all auth events (enroll, activate, login, logout, revoke, expire, auth success/failure)
- Studio: auto-generate default admin enrollment token on first run, log to stdout

**Frontend:**
- Login page (`LoginPage.tsx`) — MCP key input, error display, redirect to dashboard on success (Engine only)
- Session-aware routing — redirect to `/login` if no valid session (Engine only)
- Logout button in dashboard header
- RBAC-gated UI elements — hide settings/security log/run control based on role from session
- Consolidate Settings modal: add MCP Keys tab alongside existing General, AI Interfaces, Connections, Keys tabs
- Move AI Manager, Connections, Keys pages into Settings tabs
- Clean up nav bar
- Enrollment dialog (admin creates enrollment token, shown once)
- Key list with status indicators and actions (revoke, re-enroll, disable/enable)

**MCP sidecar:**
- No changes needed — it already passes bearer tokens through

**Tests:**
- Enrollment token create/activate/expire lifecycle
- Self-renewal (valid key, expired key rejection)
- Dashboard login/logout session lifecycle
- Session cookie security flags (HttpOnly, Secure, SameSite)
- Session expiry after timeout
- WebSocket auth via session cookie
- Key RBAC enforcement with MCP keys (both editions)
- RBAC UI gating (viewer cannot see settings, operator cannot see security log)
- Rate limiting and lockout with MCP keys
- Audit log verification (enroll, activate, login, logout, auth success/failure, revoke)
- Studio MCP key requirement (unauthenticated MCP access rejected)
- Master password memory protection (SecureString zeroing)

### Phase 2 — MCP Configure-Plane Tools

**MCP tools to add:**
- `manage_connections` (list, create, update, delete, test)
- `manage_keys` (list, create, update, delete providers)
- `upload_workflow` (create/update registered workflow)
- `validate_workflow`
- `get_run_logs`
- `whoami`

**Backend:** Minimal — these proxy to existing REST endpoints. The only new endpoint is `GET /api/auth/whoami` (done in Phase 1).

**MCP sidecar:** Add tool registrations in `tools.ts`, following the existing pattern.

### Phase 3 — Adhoc Workflow Submission

**Backend:**
- `AdhocWorkflowManager` class — staging, folder naming (`timestamp_counter_del-timestamp`), cleanup, quota enforcement
- `POST /api/workflows/run-adhoc` endpoint
- `max_ai_calls_per_jcwf` config field — parse in `configParser.cpp`, enforce in `workflowRuntimeManager.cpp`
- Bump `max inflight ai calls` default to 1000, widen clamp range in `configChecker.cpp`
- `AdhocCleanupScheduler` — background reaper thread (reads delete-at from folder name, restart-safe)
- Adhoc folder structure: `_adhoc/<timestamp_counter_del-timestamp>/workflows/` + `queue/`
- Per-user retention policy enforcement (cannot exceed admin-configured ceiling)

**Frontend:**
- Dashboard: rolling last-3-runs display with 2-second minimum visibility
- Dashboard: adhoc run stats in status display

**MCP sidecar:**
- `run_adhoc_workflow` tool

**Documentation:**
- Update `doc/api-endpoints.md`
- Update `doc/cyber security.md` — add to Admin Responsibility section:
  > **Data retention.** Adhoc workflow artifacts are automatically deleted according to the retention policy configured per MCP key (default: 72 hours). The admin is responsible for configuring retention policies appropriate to their organization's requirements and for informing users that adhoc run data is ephemeral. j9t provides no backup or recovery mechanism for cleaned-up adhoc artifacts. The `retain` policy is available for admins who need permanent data.
- Update `doc/cyber security.md` — add to Admin Responsibility section:
  > **Master password after restart.** After every j9t restart, an admin must provide the master password via the dashboard login or `POST /api/settings/keys/unlock`. Until unlocked, the following services are unavailable: MCP key authentication, AI provider API calls, OAuth token refresh (OneDrive, Google Sheets), and all cloud connections that require encrypted credentials. The `JARVIS_MASTER_PASSWORD` environment variable is not supported — the master password is held exclusively in `mlock()`-protected memory (`SecureString`) and is never written to disk, environment variables, or logs.
- Update `doc/cyber security.md` — expand the "Are Credentials Sent Unencrypted?" or "Admin responsibility" section to document `SecureString` memory protection and the services that depend on the master password being provided.
- Update `doc/jarvisagent.md` (user manual) — add a "First Steps After Restart" section explaining that the admin must unlock the master password and which services depend on it.
- Update `mcp/README.md`

---

## 13. Design Decisions

| Decision | Resolution |
|----------|-----------|
| **MCP key provisioning** | Always manual via enrollment tokens. Admin makes deliberate per-user access decisions. |
| **MCP transport security** | TLS required for SSE. No change for stdio (local-only). |
| **Key expiry notification** | `X-Key-Expires-In` response header within 30 days of expiry. Self-service renewal via `POST /api/auth/mcp-keys/self-renew`. Admin only needed if key already expired. |
| **Rate limiting** | Per-IP only for 1.0. Per-key limits deferred. |
| **Master password** | Same password for both `keys.json.enc` and `mcp_keys.json.enc`. Admin provides it once after each restart via dashboard or `POST /api/settings/keys/unlock`. `JARVIS_MASTER_PASSWORD` env var removed. j9t does not process MCP-authenticated requests or AI calls until unlocked. `GET /api/status` includes `"keys_unlocked": false`. |
| **Adhoc JCWF size** | 1 GB. Per-user disk quota is the real safeguard. `MaxRequestBodyMB` (default 10 MB) caps individual HTTP requests. |
| **Adhoc run visibility** | Visible to all for 1.0, with user identity shown on each run. Per-user run privacy deferred to post-1.0. |

---

## 14. Artifact Retrieval — Download-Plane (post-1.0 design)

### Why we need it

The 1.0 surface gives callers submit (`run_adhoc_workflow`) and status (`get_run_status`) — but no way to pull the bytes a task produced. Closing that gap is the only goal of this section. We aim for fast, secure, and convenient from an agent's perspective. Everything else is out of scope.

### Two retrieval modes, one answer

An agent may live on the same host as j9t (Claude Code on a dev machine) or on a different one (remote deployment). We support both by returning **both** a local path and a download URL in the discovery response. The agent picks.

- **Local path** — `file:///…` absolute path into the run folder. Fast: zero-copy disk read. Requires the agent to share a filesystem with j9t.
- **Download URL** — `GET /api/workflow-runs/<runId>/files/<path>`. Works anywhere. Goes through the same auth as every other request.

The discovery response includes the run's retention policy and `delete_at` so the agent knows how long the artifacts will be available in either mode — it can race the reaper if it needs to, or pull immediately.

### Folder reorganization — per-user namespace

Today: `_adhoc/<ts>_<counter>_del-<ts>/`
Proposed: `_adhoc/<user_slug>/<ts>_<counter>_del-<ts>/`

- `<user_slug>` is derived from the MCP key's `user` field: allowed chars `[A-Za-z0-9._@-]`, everything else collapsed to `_`, length ≤ 64. Cross-platform safe.
- Sanitiser is a pure function, unit-tested.
- Stored once at stage time in `meta.json`.

Authorization then becomes a filesystem prefix check: `_adhoc/<caller_slug>/` must prefix the requested folder — admin bypasses. Belt-and-braces: even if a bug skips the ownership check in code, the folder layout still prevents cross-tenant reads via path traversal alone.

Legacy top-level folders (from pre-feature builds) are treated as admin-owned on the `Init()` scan.

### Discovery — `GET /api/workflow-runs/<runId>/files`

Lists every regular file in the run folder. Returns both retrieval modes plus retention.

**Response (200):**
```json
{
  "ok": true,
  "runId": "adhoc_20260418T174752_0006",
  "owner": "alice@company.com",
  "terminal": true,
  "retention": {
    "policy": "ttl_1h",
    "delete_at": "2026-04-18T18:47:52Z",
    "seconds_remaining": 2931
  },
  "files": [
    {
      "path": "workflows/_adhoc_.../attack_stats.json",
      "size_bytes": 33307,
      "modified_at": "2026-04-18T17:47:54Z",
      "task_id": "parse",
      "content_type": "application/json",
      "sha256": "9f1a…",
      "local_path": "/home/beaumanvienna/dev/jarvisAgent/_adhoc/alice@company.com/20260418T174752_0006_del-20260418T184752/workflows/_adhoc_.../attack_stats.json",
      "download_url": "/api/workflow-runs/adhoc_20260418T174752_0006/files/workflows/_adhoc_.../attack_stats.json"
    }
  ]
}
```

- `retention.seconds_remaining` is computed at response time — the agent can see "I've got 49 minutes left" without reasoning about TTL strings.
- `local_path` is always the absolute path (not `file://` URL) so the agent can hand it straight to stdlib `open()`. Included regardless of whether the caller seems local — the agent decides.
- `terminal: false` means the run is still active and file contents may still change.
- Query params: `prefix=<subdir>` or `glob=<pattern>` to narrow the listing server-side.

### Download — `GET /api/workflow-runs/<runId>/files/<path>`

Returns the file's bytes. Path is URL-encoded, resolved relative to the run folder, must stay inside it.

- `Content-Type` detected from extension (JSON / CSV / PNG / PDF / text / octet-stream fallback)
- `Content-Length` exact
- `X-Content-SHA256` hex digest — agent compares against the manifest
- `X-Retention-Delete-At` echoes the `delete_at` so streaming clients see it without a second request
- Supports `Range: bytes=start-end` → 206 Partial Content (large files don't need a 10 MB pipe)
- Single-response cap 10 MB; above that the response is 413 with a suggested `Range`

### Manifest — machine-readable record

At run completion, `AdhocWorkflowManager::OnRunCompleted()` writes `manifest.json` in the run folder:

```json
{
  "runId": "adhoc_20260418T174752_0006",
  "owner": "alice@company.com",
  "state": "succeeded",
  "submitted_at": "2026-04-18T17:47:52Z",
  "completed_at": "2026-04-18T17:47:54Z",
  "retention": { "policy": "ttl_1h", "delete_at": "2026-04-18T18:47:52Z" },
  "files": [
    {
      "path": "workflows/_adhoc_.../attack_stats.json",
      "task_id": "parse",
      "size_bytes": 33307,
      "sha256": "9f1a…",
      "content_type": "application/json"
    }
  ]
}
```

The listing endpoint reads from the manifest when present — no directory walk, no on-the-fly hashing. Restart-safe. If the run is still active, the endpoint falls back to a live filesystem walk and omits `sha256`.

### Authorization

| Role | Own runs | Others' runs |
|------|----------|--------------|
| `viewer` | — (viewers can't submit adhoc) | 403 `insufficient_role` |
| `operator` | list + download | 403 `not_owner` |
| `admin` | list + download | list + download |

Sequence per request: `Authenticate()` → extract user + role → resolve `runId` → compare caller's user-slug to the folder's user-slug → allow or 403. Admin cross-user reads are audit-logged at INFO; denials at WARN.

### Edge cases

| Scenario | Behaviour |
|----------|-----------|
| `runId` never existed | 404 `run_not_found` |
| Run folder already reaped | 404 `run_cleaned_up` + `cleaned_at` (in-memory ledger retained 24 h after reaping) |
| File doesn't exist in run folder | 404 `file_not_found` with the current `files:` summary so the agent can self-recover |
| `..` path segment escaping the run folder | 400 `path_escape` — always, regardless of the resolved landing point |
| Absolute path sent by client | 400 `absolute_path_rejected` |
| Target is a symlink (even one pointing inside) | 400 `symlink_rejected` — no symlink following, closes a TOCTOU class |
| Target is a FIFO / socket / device | 400 `not_regular_file` |
| File > 10 MB with no `Range:` | 413 `file_too_large` + `X-Suggested-Range: bytes=0-10485759` |
| File mid-write (mtime < now - 500 ms and run active) | 409 `still_writing` + `Retry-After: 1` |
| Zero-byte file | 200 + `Content-Length: 0` + SHA-256 of the empty string |
| URL-encoded traversal (`.%2E/foo`) | Normalisation happens after URL-decode, so the path-escape check still fires |
| Reaper deletes folder mid-read | File handler holds the run's mutex while opening the fd. POSIX keeps the fd alive until close; Windows holds the mutex through the read |

### Agent loop — worked example

```
Agent  →  MCP sidecar              →  j9t
─────────────────────────────────────────────────────────────────
1.  run_adhoc_workflow(jcwf)       →  POST /api/workflows/run-adhoc
2.  get_run_status(runId)           →  state="running" (agent loops)
3.  get_run_status(runId)           →  state="succeeded"
4.  list_run_files(runId)           →  {
                                         retention: { delete_at: "…18:47:52Z", seconds_remaining: 2931 },
                                         files: [{ path, sha256, local_path, download_url, … }]
                                       }
5a. If local:  agent reads local_path directly from disk      (fast path)
5b. If remote: get_run_file(runId, path)  →  bytes + X-Content-SHA256
                                         (agent compares against manifest sha256)
6.  agent processes the file
```

### MCP tools

```typescript
server.tool("list_run_files", "Discover outputs of a run (paths, retention, hashes, both local path and download URL)", {
    runId: z.string(),
    prefix: z.string().optional(),
    glob: z.string().optional(),
}, …);

server.tool("get_run_file", "Download a single artifact (honours the caller's MCP identity; respects Range for large files)", {
    runId: z.string(),
    path: z.string(),
    range_start: z.number().optional(),
    range_end: z.number().optional(),
}, …);
```

### Implementation phases (post-1.0)

> Phases 4-7 landed on **2026-04-18**. Phase 7 (script catalog) was added after 4-6 when we realised agents had no way to discover which scripts are available to reference. Each phase listed here is implemented, tested, and documented.

**Phase 4 — Folder reorg + manifest (backend-only, no new tools):**
- `SanitizeUserSlug()` pure function + unit tests.
- `Stage()` writes under `_adhoc/<slug>/`, records owner+slug in `meta.json`.
- `Init()` scans both layouts; legacy top-level treated as admin-owned.
- `OnRunCompleted()` writes `manifest.json`; SHA-256 computed off the tick thread.
- In-memory `runId → owner` map.
- Extend `test_auth_mcp.py`: new layout, manifest-on-completion, cross-user 403.

**Phase 5 — Listing endpoint + `list_run_files` MCP tool:**
- `GET /api/workflow-runs/<runId>/files` — reads manifest if present, walks otherwise.
- Shared `ResolveSafeRunPath()` helper — path escape, symlink rejection, normalisation.

**Phase 6 — Download endpoint + `get_run_file` MCP tool:**
- `GET /api/workflow-runs/<runId>/files/<path>` with `Range:` support and `X-Content-SHA256`.
- Edge cases per the table above.
- MCP sidecar handles binary payloads (base64) vs text pass-through.

**Phase 7 — Script catalog endpoint + `list_scripts` MCP tool (added after 4-6):**
- `ScriptCatalog` (`application/workflow/scriptCatalog.{h,cpp}`) — scans `scripts/` on startup; on-demand refresh via `?refresh=1`. Parses `@jarvis-script` headers (short / params / description / outputs) into structured metadata.
- `GET /api/scripts` — viewer+, filters by `?type=shell|python`. Returns the catalog including `has_jarvis_marker` and `executable` flags per entry.
- `list_scripts` MCP tool — thin wrapper. Agents call this first to discover what's available before composing an adhoc JCWF.
- Dashboard: Scripts tab in Settings modal with a table view, type filter, and manual Refresh button (calls `?refresh=1`).
- Metadata backfilled on existing scripts so 29/35 carry the `@jarvis-script` header (the remaining 6 are internal helpers / entry-point facades, correctly excluded from agent-visible discovery).

### Non-goals for this feature

- Write access through the API — read-only, always. Mutation stays with the workflow runtime.
- Artefact versioning — the manifest records latest state; overwrites aren't tracked.
- Cross-run search — listing is per-run only.
- Archive / ZIP download, signed URLs, replication, scanning, encryption-at-rest, streaming stdout — none of these are on the roadmap for this feature. Revisit if a concrete need appears.

---

## Appendix A — Detailed Development Plan

This section maps the implementation phases to concrete C++ classes, file changes, and build order. Based on the existing codebase: `Authenticate()` at `webServer.cpp:1025`, `KeyEncryption` (AES-256-GCM + PBKDF2) at `engine/keys/keyEncryption.h`, `KeyManager` at `engine/keys/keyManager.h`, `EngineConfig` at `engine/json/configParser.h`.

---

### Phase 1 — MCP API Keys + Unified Security

#### Step 1.1: SecureString

Replaces the plain `std::string m_CachedMasterPassword` in `KeyManager` (line 137 of `keyManager.h`).

**New file: `engine/keys/secureString.h`**

```cpp
class SecureString
{
public:
    SecureString();
    ~SecureString();                              // explicit_bzero + munlock
    SecureString(SecureString&&) noexcept;
    SecureString& operator=(SecureString&&) noexcept;
    SecureString(SecureString const&) = delete;
    SecureString& operator=(SecureString const&) = delete;

    void Set(std::string_view value);             // mlock + copy
    std::string_view Get() const;                 // returns view into locked buffer
    bool IsEmpty() const;
    void Clear();                                 // explicit_bzero

private:
    char* m_Buffer{nullptr};
    size_t m_Size{0};
    size_t m_Capacity{0};
};
```

**Changes:**
- `engine/keys/keyManager.h` — replace `std::string m_CachedMasterPassword` with `SecureString m_CachedMasterPassword`
- `engine/keys/keyManager.cpp` — update `Load()`, `Save()`, `Unlock()`, `GetCachedMasterPassword()` to use `SecureString::Get()`
- `application/web/webServer.cpp` — remove `JARVIS_MASTER_PASSWORD` env var reads (lines ~6191 and ~6984)

#### Step 1.2: McpKeyManager

**New files: `application/web/mcpKeyManager.h` / `mcpKeyManager.cpp`**

```cpp
struct McpKeyRecord
{
    std::string m_KeyId;                  // "mcp_a1b2c3d4"
    std::string m_KeyHash;                // SHA-256 of raw key
    std::string m_User;                   // "alice@company.com"
    std::string m_Role;                   // "admin" | "operator" | "viewer"
    bool m_AdhocEnabled{false};
    int m_DiskQuotaMb{1024};
    std::string m_DefaultCleanupPolicy{"ttl_72h"};
    std::string m_CreatedAt;
    std::string m_ExpiresAt;
    std::string m_LastUsedAt;
    bool m_Enabled{true};
    std::string m_Description;
};

struct EnrollmentToken
{
    std::string m_TokenHash;              // SHA-256 of raw enrollment token
    std::string m_User;
    std::string m_Role;
    bool m_AdhocEnabled{false};
    int m_DiskQuotaMb{1024};
    std::string m_DefaultCleanupPolicy{"ttl_72h"};
    std::string m_Description;
    std::string m_ExpiresAt;              // 30-min TTL
    std::string m_CreatedBy;              // admin who created it
};

class McpKeyManager
{
public:
    // Lifecycle
    bool Load(std::string const& path, std::string_view masterPassword);
    bool Save(std::string const& path, std::string_view masterPassword);
    bool IsLoaded() const;

    // Enrollment flow
    std::string CreateEnrollment(EnrollmentToken const& token);  // returns raw enrollment token (once)
    struct ActivateResult { std::string keyId; std::string rawKey; McpKeyRecord record; };
    std::optional<ActivateResult> ActivateEnrollment(std::string const& rawEnrollmentToken);

    // Authentication
    struct AuthResult { McpKeyRecord record; int daysUntilExpiry; };
    std::optional<AuthResult> Authenticate(std::string const& rawKey);

    // Self-renewal
    struct RenewResult { std::string keyId; std::string rawKey; std::string expiresAt; };
    std::optional<RenewResult> SelfRenew(std::string const& currentRawKey);

    // Admin CRUD
    std::vector<McpKeyRecord> ListKeys() const;                  // keys redacted
    bool UpdateKey(std::string const& keyId, /* fields */);
    bool RevokeKey(std::string const& keyId);

    // Quota tracking
    void RecordDiskUsage(std::string const& user, size_t bytes);
    size_t GetDiskUsage(std::string const& user) const;

private:
    void PurgeExpiredEnrollments();
    void DisableGracePeriodKeys();

    std::vector<McpKeyRecord> m_Keys;
    std::vector<EnrollmentToken> m_Enrollments;
    std::unordered_map<std::string, size_t> m_DiskUsageByUser;  // bytes
    mutable std::mutex m_Mutex;
    bool m_Loaded{false};
};
```

**Encryption:** Reuses existing `KeyEncryption::Encrypt()` / `KeyEncryption::Decrypt()` from `engine/keys/keyEncryption.h`. Same file format (JKEY magic, salt, IV, GCM tag), different file name (`mcp_keys.json.enc`).

#### Step 1.3: Integrate into WebServer

**Changes to `application/web/webServer.h`:**
```cpp
#include "mcpKeyManager.h"

// New member:
McpKeyManager m_McpKeyManager;
```

**Changes to `application/web/webServer.cpp`:**

`Authenticate()` (line ~1025) — add MCP key branch:
```cpp
AuthResult Authenticate(crow::request const& req) const
{
    // ... existing lockout and rate limit checks ...

    std::string token = ExtractBearerToken(req);

    // NEW: MCP key path
    if (token.starts_with("mcp_"))
    {
        auto result = m_McpKeyManager.Authenticate(token);
        if (!result)
        {
            RecordAuthFailure(ip);
            LOG_SECURITY_WARN("MCP auth failed: invalid key from {}", ip);
            return {"invalid_token", "", ""};
        }
        if (!result->record.m_Enabled)
            return {"key_disabled", "", ""};
        if (result->daysUntilExpiry < 0)
            return {"token_expired", "", ""};

        LOG_SECURITY_INFO("MCP auth: {} ({}) from {}", result->record.m_User, result->record.m_Role, ip);
        return {"", result->record.m_User, result->record.m_Role};
    }

    // Existing admin token path (unchanged)
    // ...
}
```

**New route handlers:**
```
RegisterCommonRoutes():  (both editions)
    GET    /api/auth/mcp-keys              → HandleMcpKeysGet()
    POST   /api/auth/mcp-keys/enroll       → HandleMcpKeysEnrollPost()
    POST   /api/auth/mcp-keys/activate     → HandleMcpKeysActivatePost()
    POST   /api/auth/mcp-keys/self-renew   → HandleMcpKeysSelfRenewPost()
    PUT    /api/auth/mcp-keys/<key_id>     → HandleMcpKeysPut()
    DELETE /api/auth/mcp-keys/<key_id>     → HandleMcpKeysDelete()
    GET    /api/auth/whoami                → HandleWhoamiGet()
```

**Unlock flow change** — `HandleKeysUnlockPost()` (line ~5852): after unlocking `keys.json.enc`, also unlock `mcp_keys.json.enc` with the same password.

**Status endpoint** — `HandleStatusGet()`: add `"keys_unlocked": bool`.

**Response header injection** — after `Authenticate()`, if `daysUntilExpiry <= 30`, add `X-Key-Expires-In` header.

#### Step 1.4: Studio Security

**Change in `Authenticate()`** — remove the early return for Studio:

```cpp
// BEFORE (line ~1030):
#ifndef J9T_STUDIO
    // Engine auth logic...
#else
    return {"", "studio", "admin"};  // No auth in Studio
#endif

// AFTER:
    // MCP key check (both editions)
    if (token.starts_with("mcp_")) { /* MCP key path — same in both editions */ }

#ifndef J9T_STUDIO
    // Engine: admin token + gateway header path (existing)
#else
    // Studio: no auth required for non-MCP requests (browser UI)
    if (token.empty()) return {"", "studio", "admin"};
    // If a token was provided but not MCP, reject it
    return {"invalid_token", "", ""};
#endif
```

**First-run enrollment:** In `RegisterRoutes()`, if `mcp_keys.json.enc` doesn't exist, auto-create a default admin enrollment token and log it to stdout.

#### Step 1.5: Dashboard Login (SessionManager)

**New file: `application/web/sessionManager.h`** (note: different from the existing `application/session/sessionManager.h` which manages AI queue sessions)

```cpp
struct Session
{
    std::string m_SessionId;              // 256-bit random hex
    std::string m_User;                   // from MCP key or gateway header
    std::string m_Role;                   // admin | operator | viewer
    std::chrono::steady_clock::time_point m_CreatedAt;
    std::chrono::steady_clock::time_point m_LastActivity;
};

class WebSessionManager
{
public:
    // Login: validate MCP key or gateway headers, create session
    struct LoginResult { std::string sessionId; std::string user; std::string role; };
    std::optional<LoginResult> Login(std::string const& mcpKey);
    std::optional<LoginResult> LoginFromGateway(std::string const& user, std::string const& role);

    // Session validation (called on every request)
    std::optional<Session> Validate(std::string const& sessionId);

    // Logout
    void Destroy(std::string const& sessionId);

    // Maintenance
    void PurgeExpired();                  // called periodically from OnUpdate()

    void SetTimeoutHours(int hours);

private:
    std::unordered_map<std::string, Session> m_Sessions;
    mutable std::mutex m_Mutex;
    int m_TimeoutHours{8};
};
```

**Changes to `application/web/webServer.h`:**
```cpp
#include "sessionManager.h"   // WebSessionManager (rename to avoid collision)

// New member:
WebSessionManager m_WebSessionManager;
```

**Changes to `application/web/webServer.cpp`:**

New route handlers:
```
POST /api/auth/login   -> HandleLoginPost()
POST /api/auth/logout  -> HandleLogoutPost()
```

`HandleLoginPost()`:
1. Parse `api_key` from request body.
2. Call `m_McpKeyManager.Authenticate(apiKey)`.
3. If valid, call `m_WebSessionManager.Login(apiKey)`.
4. Set response cookie: `session=<id>; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=28800`.
5. Return 200 with `{ "ok": true, "user": "...", "role": "..." }`.
6. Audit log: `alice@company.com logged in from <IP>`.

`HandleLogoutPost()`:
1. Extract session ID from cookie.
2. Call `m_WebSessionManager.Destroy(sessionId)`.
3. Clear cookie: `session=; Max-Age=0`.
4. Audit log: `alice@company.com logged out`.

**Request authentication update** — `Authenticate()` gains a third auth path:

```cpp
AuthResult Authenticate(crow::request const& req) const
{
    // Path 1: MCP key (Bearer mcp_...)
    // Path 2: Gateway headers (X-Forwarded-User/Role)
    // Path 3 (NEW): Session cookie
    std::string sessionId = ExtractSessionCookie(req);
    if (!sessionId.empty())
    {
        auto session = m_WebSessionManager.Validate(sessionId);
        if (session)
        {
            // Slide the expiry window
            return {"", session->m_User, session->m_Role};
        }
        // Expired session — fall through to "no auth"
    }

    // Path 4: Studio — no auth for non-MCP requests
    // Path 5: Engine — reject (no valid auth)
}
```

**WebSocket upgrade** — in the `/ws` `onopen` handler, check for session cookie before falling back to token-as-first-message:
```cpp
// Engine browser clients: validate session cookie from upgrade request headers
// MCP clients: continue using token-as-first-message (existing)
```

**Static file serving** — Engine edition: all routes except login page, `/api/status`, and `/api/auth/*` endpoints require a valid session or bearer token. Unauthenticated requests to `/` redirect to `/login`.

#### Step 1.6: Config Changes

**`engine/json/configParser.h`** — add to `EngineConfig`:
```cpp
int m_SessionTimeoutHours{8};             // dashboard session lifetime
```

**`engine/json/configParser.cpp`** — parse `"session_timeout_hours"` from config.json.

#### Step 1.7: Frontend — Login Page + Settings Consolidation

**Build order:**
1. Create `LoginPage.tsx` — MCP key input field, submit button, error display. Only shown in Engine mode.
2. Add session-aware routing to `App.tsx` — on mount, call `GET /api/auth/whoami`. If 401, redirect to `/login`. Store user/role in React context.
3. Add logout button to dashboard header (Engine only).
4. Add RBAC-gated rendering — read role from session context, hide settings/security log/run control for insufficient roles.
5. Create `SettingsMcpKeysTab.tsx` — key list, enrollment dialog, edit modal
6. Move `AiManagerView.tsx` content into `SettingsAiInterfacesTab.tsx`
7. Move `ConnectionsView.tsx` content into `SettingsConnectionsTab.tsx`
8. Move `ProvidersSettingsView.tsx` content into `SettingsKeysTab.tsx`
9. Update `SettingsModal.tsx` — add tab bar with: General | AI Interfaces | Connections | Keys | MCP Keys | About
10. Remove old nav items, update `App.tsx` routing

---

### Phase 2 — MCP Configure-Plane Tools

#### Step 2.1: MCP Sidecar Tool Registration

**File: `mcp/src/tools.ts`** — add tool registrations following the existing pattern. Each tool calls `client.get()` or `client.post()` on the corresponding REST endpoint.

New tools (6):

```typescript
// manage_connections — wraps GET/POST/PUT/DELETE /api/connections
server.tool("manage_connections", "...", {
    action: z.enum(["list", "create", "update", "delete", "test"]),
    name: z.string().optional(),
    connection: z.object({...}).optional(),
}, async ({ action, name, connection }) => { ... });

// manage_keys — wraps GET/POST/PUT/DELETE /api/settings/providers
server.tool("manage_keys", "...", { ... }, async () => { ... });

// upload_workflow — wraps POST /api/workflows
server.tool("upload_workflow", "...", {
    jcwf: z.object({}).passthrough(),
}, async ({ jcwf }) => { ... });

// validate_workflow — wraps POST /api/workflows/validate
server.tool("validate_workflow", "...", { ... }, async () => { ... });

// get_run_logs — wraps GET /api/log?tail=N
server.tool("get_run_logs", "...", {
    tail: z.number().optional().default(100),
}, async ({ tail }) => { ... });

// whoami — wraps GET /api/auth/whoami
server.tool("whoami", "...", async () => { ... });
```

**File: `mcp/src/j9tClient.ts`** — add `put()` and `delete()` methods (currently only has `get()` and `post()`).

---

### Phase 3 — Adhoc Workflow Submission

#### Step 3.1: Config Changes

**`engine/json/configParser.h`** — add to `EngineConfig`:
```cpp
size_t m_MaxAiCallsPerJcwf{0};           // 0 = no cap; test value: 1000
size_t m_MaxInflightAiCalls{1000};        // bumped from 100
```

**`engine/json/configParser.cpp`** — parse `"max_ai_calls_per_jcwf"` from config.json.

**`engine/json/configChecker.cpp`** — widen clamp: `m_MaxInflightAiCalls` range [1, 10000] (was [1, 1000]).

#### Step 3.2: AdhocWorkflowManager

**New files: `application/workflow/adhocWorkflowManager.h` / `adhocWorkflowManager.cpp`**

```cpp
class AdhocWorkflowManager
{
public:
    AdhocWorkflowManager(McpKeyManager& keyManager);

    struct SubmitRequest
    {
        std::string m_JcwfJson;               // raw canvas JSON
        std::string m_User;                    // from MCP key
        std::map<std::string, std::string> m_Context;
        std::string m_CleanupPolicy;          // "on_completion", "ttl_1h", etc.
    };

    struct SubmitResult
    {
        std::string m_RunId;
        std::string m_WorkflowId;
        std::string m_FolderPath;
    };

    // Core operations
    std::expected<SubmitResult, std::string> Stage(SubmitRequest const& req);
    void CleanupOnCompletion(std::string const& folderPath);
    void StartReaperThread();
    void StopReaperThread();

    // Quota
    bool CheckQuota(std::string const& user, size_t additionalBytes) const;

    // Folder naming
    static std::string GenerateFolderName(std::string const& cleanupPolicy);
    // e.g. "20260416T143022_0001_del-20260419T143022"

private:
    void ReaperLoop();                        // 60s interval, scans _adhoc/
    std::string ComputeDeleteAt(std::string const& cleanupPolicy) const;

    McpKeyManager& m_KeyManager;
    std::atomic<uint32_t> m_Counter{0};
    std::thread m_ReaperThread;
    std::atomic<bool> m_ReaperRunning{false};
    std::string m_AdhocBasePath;              // "_adhoc/"
};
```

#### Step 3.3: Per-Run AI Call Cap

**`application/workflow/workflowTypes.h`** — add to `WorkflowRun`:
```cpp
std::atomic<size_t> m_AiCallsDispatched{0};
```

**`application/workflow/workflowRuntimeManager.cpp`** — in the task dispatch path (where `ai_call` tasks are submitted to `AiRequestPool`):
```cpp
size_t cap = Core::g_Core->GetConfig().m_MaxAiCallsPerJcwf;
if (cap > 0 && run.m_AiCallsDispatched.fetch_add(1) >= cap)
{
    // Reject: set task to Failed with "AI call cap exceeded (max_ai_calls_per_jcwf=1000)"
    run.m_AiCallsDispatched.fetch_sub(1);
    FailTask(task, "AI call cap exceeded");
    return;
}
```

#### Step 3.4: WebServer Integration

**`application/web/webServer.h`** — add member:
```cpp
AdhocWorkflowManager m_AdhocManager;
```

**`application/web/webServer.cpp`** — new route:
```
POST /api/workflows/run-adhoc → HandleAdhocRunPost()
```

Handler logic:
1. `Authenticate()` — verify MCP key, check `adhoc_enabled`
2. `m_AdhocManager.CheckQuota()` — verify disk quota
3. `WorkflowValidator::Validate()` — reject Tier A errors
4. `m_AdhocManager.Stage()` — create folder, write JCWF container
5. Register in `WorkflowRegistry` as transient
6. Dispatch via `WorkflowRuntimeManager`
7. Return 202 with runId

#### Step 3.5: MCP Sidecar

**`mcp/src/tools.ts`** — add:
```typescript
server.tool("run_adhoc_workflow", "Submit and execute an ephemeral workflow", {
    jcwf: z.object({}).passthrough().describe("Complete JCWF canvas JSON"),
    context: z.record(z.string(), z.string()).optional(),
    cleanup_policy: z.enum(["on_completion","ttl_1h","ttl_24h","ttl_48h","ttl_72h","retain"]).optional(),
}, async ({ jcwf, context, cleanup_policy }) => { ... });
```

#### Step 3.6: Dashboard Updates

**`dashboard/ui/src/`:**
- Add `LastRunsBar.tsx` — rolling 3-line display component. Subscribes to `workflow-runs-snapshot` WebSocket. Holds completed runs for minimum 2 seconds before removal.
- Update `App.tsx` — render `LastRunsBar` in header area.
- Show adhoc runs with italic text + "adhoc" badge.
- Show user identity on each run entry.

---

### Build Order Summary

```
Phase 1 (MCP Keys + Dashboard Login):
  1.1  SecureString                  (engine/keys/)
  1.2  McpKeyManager                 (application/web/)
  1.3  WebServer auth integration    (application/web/webServer.cpp)
  1.4  Studio security branch        (application/web/webServer.cpp)
  1.5  WebSessionManager + login     (application/web/)
  1.6  Config — session_timeout_hours (engine/json/)
  1.7  Frontend: login page + settings consolidation (dashboard/ui/ + workflow-editor/ui/)
  ---  Tests: enrollment lifecycle, self-renewal, login/logout, session expiry,
       cookie security flags, WebSocket session auth, RBAC UI gating,
       rate limiting, audit log, Studio MCP key requirement

Phase 2 (MCP Tools):
  2.1  j9tClient.ts — add put/delete  (mcp/src/)
  2.2  Tool registrations             (mcp/src/tools.ts)
  ---  Tests: each tool round-trip

Phase 3 (Adhoc):
  3.1  Config changes                 (engine/json/)
  3.2  AdhocWorkflowManager           (application/workflow/)
  3.3  Per-run AI cap                 (application/workflow/)
  3.4  WebServer route                (application/web/)
  3.5  MCP tool                       (mcp/src/)
  3.6  Dashboard updates              (dashboard/ui/)
  ---  Tests: adhoc submit, quota, cleanup, AI cap, folder naming
```

