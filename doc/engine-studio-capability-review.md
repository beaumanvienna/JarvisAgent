# Edition Architecture and Access Control

This document defines how j9t authenticates requests, how access is gated by role, and how the Studio and Engine editions diverge in feature surface.

## Goals

- One authentication funnel for both editions. Same code path, same failure modes.
- Token-based authentication on every non-bootstrap route. No anonymous bypasses.
- Gateway-injected identity headers act as a cross-check on top of the token, never as a replacement for it.
- Capability boundaries are enforced by role, not by edition. The edition controls which routes exist; the role controls who can call them.

## Authentication funnel

```
inbound request
        │
        ▼
  ┌──────────────────────────────────────────────┐
  │ Public bootstrap allowlist?                  │
  │  GET  /                                      │
  │  GET  /dash-assets/<path>                    │
  │  GET  /api/status                            │
  │  POST /api/auth/login                        │
  │  POST /api/auth/mcp-keys/activate            │
  │  GET  /api/settings/keys/status              │
  │  POST /api/settings/keys/unlock              │
  └────────────────┬─────────────────────────────┘
                   │ no
                   ▼
  ┌──────────────────────────────────────────────┐
  │ Extract credential                           │
  │  Authorization: Bearer mcp_...               │
  │   — or — session cookie                      │
  │   — or — webhook HMAC signature              │
  │   — or — n8n integration HMAC signature      │
  │ Reject 401 if no credential present          │
  └────────────────┬─────────────────────────────┘
                   ▼
  ┌──────────────────────────────────────────────┐
  │ Validate credential → (user, role)           │
  │  Webhook HMAC binds to the workflow's secret │
  │  and resolves to a synthetic identity        │
  │  with role 'webhook' (no MCP role)           │
  └────────────────┬─────────────────────────────┘
                   ▼
  ┌──────────────────────────────────────────────┐
  │ Gateway cross-check                          │
  │  When TrustedProxyHeader is configured AND   │
  │  the header is present:                      │
  │    gw_user must equal token_user → else 403  │
  │    effective_role = min(token_role, gw_role) │
  │  Gateway can downgrade, never escalate.      │
  └────────────────┬─────────────────────────────┘
                   ▼
  ┌──────────────────────────────────────────────┐
  │ Per-IP rate limit + auth-failure lockout     │
  └────────────────┬─────────────────────────────┘
                   ▼
  ┌──────────────────────────────────────────────┐
  │ Route role gate                              │
  │  CheckAuth(req, viewer | operator | admin)   │
  └────────────────┬─────────────────────────────┘
                   ▼
                handler
```

Rendered version (mermaid → PDF, vector grahpics):

[auth-funnel.pdf](auth-funnel.pdf) 

### Key properties

- Anonymous access is restricted to the seven bootstrap routes plus `POST /api/auth/logout` (which requires a session cookie). Every other route requires a valid credential.
- The credential is a primary fact. The gateway header is a secondary fact that confirms it. Without a credential, no header chain authorises a request.
- Gateway role caps the credential's role downward when both are present. This honours least-privilege when the gateway has coarser identity (e.g. an IdP group says "read-only contractor" while a stale MCP key says "operator").
- `Authenticate()` returns a single `AuthResult { user, role, mechanism }` regardless of which credential type was used. Handlers see one shape.

## Bootstrap allowlist

The seven anonymous routes plus the session-only logout. Nothing else is exempt.

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `/` | GET | none | Dashboard HTML shell |
| `/dash-assets/<path>` | GET | none | Dashboard static assets |
| `/api/status` | GET | none | Health probe |
| `/api/auth/login` | POST | the MCP key in the body | Issue session cookie |
| `/api/auth/mcp-keys/activate` | POST | the enrollment token in the body | Materialise an MCP key from an admin-issued enrollment token |
| `/api/settings/keys/status` | GET | none | Pre-unlock probe |
| `/api/settings/keys/unlock` | POST | the master password in the body | Unlock the encrypted key store |
| `/api/auth/logout` | POST | session cookie | Invalidate the session |

WebSocket upgrades (`/ws`, `/ws/assistant` in Studio) are not in the allowlist. They run the funnel at handshake time via Crow's `.onaccept` hook.

## Edition split

| | Studio | Engine |
|-|--------|--------|
| **Purpose** | Developer workstation | Production server |
| **Browser UI** | Dashboard + workflow editor | Dashboard only |
| **Auth model** | Identical to Engine | Identical to Studio |
| **Webhook secret** | Mandatory | Mandatory |
| **Workflow CRUD + AI assistant + AI JCWF generation** | Available | Compile-time excluded (`removefiles` + `#ifdef J9T_STUDIO`) |
| **Editor support endpoints** (`/api/scripts/check`, `/api/scripts/registry`, `/api/files/check`, validate, versions write path) | Available | Compile-time excluded |
| **TLS** | Optional via `TlsCert`/`TlsKey` | Optional via `TlsCert`/`TlsKey` |
| **Rate limiting** | On | On |
| **Failed-auth lockout** | On | On |
| **Audit log** | On | On |
| **Gateway cross-check** | Available; opt in via `TrustedProxyHeader` | Available; opt in via `TrustedProxyHeader` |

The "edition" of a deployment is a binary build property. There are no runtime flags that turn a Studio binary into an Engine binary or vice versa.

## Route surface

For each route: edition where it is registered, and the minimum role the handler enforces.

### Public bootstrap

See the table above.

### Identity and key management

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/auth/whoami` | Both | viewer |
| `POST /api/auth/mcp-keys/self-renew` | Both | operator |
| `GET /api/auth/mcp-keys` | Both | admin |
| `POST /api/auth/mcp-keys/enroll` | Both | admin |
| `PUT /api/auth/mcp-keys/<id>` | Both | admin |
| `DELETE /api/auth/mcp-keys/<id>` | Both | admin |

### Workflow registry — read

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/workflows` | Both | viewer |
| `GET /api/workflows/<id>` | Both | viewer |
| `GET /api/workflows/<id>/tree` | Both | viewer |
| `GET /api/workflows/dependency-graph` | Both | viewer |

### Workflow registry — manage

| Endpoint | Edition | Role |
|----------|---------|------|
| `POST /api/workflows` | Studio | admin |
| `PUT /api/workflows/<id>` | Studio | admin |
| `DELETE /api/workflows/<id>` | Studio | admin |
| `POST /api/workflows/reload` | Both | admin |
| `POST /api/workflows/validate` | Studio | admin |
| `POST /api/workflows/<id>/validate` | Studio | admin |
| `GET /api/workflows/<id>/versions` | Both | admin |
| `GET /api/workflows/<id>/versions/<ts>` | Both | admin |
| `POST /api/workflows/<id>/versions/<ts>/restore` | Both | admin |

In Engine the registry is managed on disk (admin drops or removes JCWF files in `workflows/`) and refreshed via `POST /api/workflows/reload`. The mutation routes that exist in Studio are the editor's authoring surface and are absent from Engine.

### Run control

| Endpoint | Edition | Role |
|----------|---------|------|
| `POST /api/workflows/<id>/run` | Both | operator |
| `POST /api/workflow-runs/<id>/cancel` | Both | operator |
| `POST /api/workflow-runs/<id>/pause` | Both | operator |
| `POST /api/workflow-runs/<id>/resume` | Both | operator |
| `POST /api/workflow-runs/<id>/stop` | Both | operator |
| `POST /api/workflows/<id>/clean` | Both | operator |
| `POST /api/workflows/run-adhoc` | Both | operator + `adhoc_enabled` on the MCP key |

### Run monitoring and artifacts

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/workflow-runs/active` | Both | viewer |
| `GET /api/workflow-runs/last` | Both | viewer |
| `GET /api/workflow-runs/<id>` | Both | viewer |
| `GET /api/workflow-runs/<id>/files` | Both | operator (own runs only) |
| `GET /api/workflow-runs/<id>/files/<path>` | Both | operator (own runs only) |

### Logs

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/log` | Both | operator |
| `GET /api/log/security` | Both | admin |
| `GET /api/log/analyze-last-run` | Both | operator |

### Heartbeat and health

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/mcp/heartbeat` | Both | viewer |
| `POST /api/task/<id>/heartbeat` | Both | operator |
| `GET /api/debug/signals` (debug builds only) | Both | viewer |

### Webhooks and integrations

Webhook callers (CI systems, n8n, GitHub, Slack, etc.) sign requests with HMAC-SHA256 over the raw body. The HMAC is the credential. No MCP token is required and no MCP token can substitute for a missing or invalid HMAC.

| Endpoint | Edition | Auth |
|----------|---------|------|
| `POST /api/webhook/<id>` | Both | HMAC-SHA256 with the per-workflow `secret` from the trigger params (mandatory) |
| `POST /api/integrations/n8n/start` | Both | HMAC-SHA256 (mandatory) |

A webhook secret is mandatory in both editions. A workflow whose webhook trigger lacks a `secret` is rejected at validation time; a request without a valid signature is rejected with HTTP 401.

### Settings

Settings management is reachable via the dashboard UI and via direct REST in both editions, restricted to admin role. Settings are not exposed through MCP tools — `manage_keys` and `manage_connections` remain the only configuration tools on the MCP surface, and no equivalent tools are added for AI interfaces or `config.json` editing.

| Endpoints | Edition | Role |
|-----------|---------|------|
| `GET/POST/PUT/DELETE /api/settings/ai-interfaces`, `<name>`, `/save`, `/test` | Both | admin |
| `GET/POST/PUT/DELETE /api/settings/providers`, `<name>`, `<name>/default`, `/save` | Both | admin |
| `GET/PUT /api/settings/config`, `POST /api/settings/config/reload` | Both | admin |
| `GET/POST/PUT/DELETE /api/connections`, `<name>`, `<name>/test`, `/save`, `<name>/oauth/authorize`, `<name>/oauth/callback` | Both | admin |

### Editor support

| Endpoint | Edition | Role |
|----------|---------|------|
| `GET /api/scripts` | Both | viewer |
| `GET /api/scripts/check` | Studio | viewer |
| `GET /api/scripts/registry` | Studio | viewer |
| `GET /api/files/check` | Studio | viewer |

### AI assistant and AI JCWF generation

All endpoints under `/ws/assistant`, `/api/chat`, and the WebSocket message types `ai-explain-jcwf`, `ai-generate-jcwf`, `ai-write-scripts`, `ai-fix-failed-script` are Studio-only. The implementing source files (`application/assistant/**`, `application/web/aiJcwfService.{h,cpp}`, `application/web/webServerStudio.cpp`) are removed from the Engine compile graph by `removefiles` in `premake5.lua`.

### Shutdown

| Endpoint | Edition | Role |
|----------|---------|------|
| `POST /api/shutdown` | Both | admin |

## Implementation plan

1. **Authenticate() rewrite.** One function in `webServer.cpp`. Returns `AuthResult { user, role, mechanism }`. Steps in order: bootstrap allowlist short-circuit; credential extraction; credential validation; gateway cross-check (identity equality, role minimum); rate limit and lockout; role gate. The current Studio anonymous-localhost branch is removed. The current "gateway header alone is sufficient" branch is removed.

2. **Route registration restructure.** `webServer.cpp`. Delete `RegisterEngineRoutes()` and inline its two routes into `RegisterCommonRoutes()`. Move from `RegisterStudioRoutes()` into `RegisterCommonRoutes()`: `POST /api/workflows/<id>/run`, `POST /api/workflows/<id>/clean`, `GET /api/workflows/<id>/tree`, `GET /api/workflows/dependency-graph`, `POST /api/workflows/reload`, `GET /api/workflows/<id>/versions`, `GET /api/workflows/<id>/versions/<ts>`, `POST /api/workflows/<id>/versions/<ts>/restore`, `GET /api/log/analyze-last-run`, and the full settings surface (`/api/settings/ai-interfaces/*`, `/api/settings/providers/*`, `/api/settings/config*`, `/api/connections/*`). Each handler's `CheckAuth(req, role)` becomes the load-bearing gate.

3. **Webhook secret enforcement.** `workflowValidator.cpp`: a webhook trigger without a `secret` field is a Tier B error in both editions. `webServer.cpp` HMAC verification path: drop the Studio "no secret configured" pass-through.

4. **Status capabilities.** `GET /api/status` returns a `capabilities` object reflecting the actual route registration. The dashboard reads this to show or hide UI affordances. Update to mark workflow-run, settings-admin, log-analyze, and settings tabs as available in Engine when admin role is held.

5. **Studio dashboard auth.** Studio dashboard ships the same login flow as Engine. The "skip login on Studio" branch in the dashboard React app is removed. The dashboard relies on `GET /api/auth/whoami` to drive role-aware UI.

6. **MCP sidecar.** No code change. `run_workflow` already exists in `mcp/src/tools.ts`; it begins working in Engine when the route registration moves. No new MCP tools are added for settings management.

7. **Contract tests.** `test/test_edition_contract.py` extended to cover, for both editions: anonymous → 401 on every non-bootstrap route; viewer token → 200 on read routes, 403 on operator routes; operator token → 200 on run-control, 403 on admin-only routes; admin token → 200 on settings/admin routes. Engine returns 404 only on routes legitimately gated by `J9T_STUDIO` (workflow CRUD, validate, AI assistant, editor-support endpoints).

## Documentation and integration sweep

The implementation lands together with these doc updates in the same PR.

1. **`doc/cyber security.md`.**
   - "Editions at a Glance" table: drop the Studio rows that asserted no browser auth, optional webhook secrets, no rate limiting, and no failed-auth lockout. Both editions share the same auth posture; only the feature surface differs.
   - "j9t Studio — Developer Workstation → Remaining threats → No browser-UI authentication" bullet: remove. Replace with a brief note that Studio enforces the same auth funnel as Engine and is reachable only with a valid MCP key or session cookie.
   - "j9t Engine → Safety measures → Three auth paths, nothing else": rewrite the third path. The gateway header is no longer an authentication mechanism; it is a cross-check that confirms the credential and may cap the role.
   - "Remaining threats → Gateway header spoofing": remove. The threat is closed by always-token plus identity cross-check.
   - "Public endpoints are read-only and non-sensitive": replace with the bootstrap allowlist defined in this document.

2. **`doc/api-endpoints.md`.** Per-endpoint "Studio only" / "Both editions" annotations regenerated from the route surface tables in this document. Add the role column.

3. **`integration/README.md`.** The "In Studio mode, if no secret is configured the webhook is open" sentence is removed. The webhook secret is mandatory in both editions; document it as a required field of the webhook trigger.

4. **`integration/n8n-node/README.md` and the n8n custom node v2.** Update the node's setup instructions to require an HMAC secret. The "use legacy endpoint without HMAC" toggle is removed if it exists; if kept for backward compatibility with users on the deprecated `POST /api/integrations/n8n/start`, label it deprecated.

5. **`README.md`.** "Editions" section updated to match the new Editions table.

## Verification

After the implementation lands, the following commands and checks confirm the design holds.

- `nm bin/Debug/jarvisAgent-engine | grep -c "AssistantController\|AiJcwfService"` returns 0. The Studio-source isolation continues to hold.
- `curl -fsS https://localhost:8443/api/workflows/example/run -X POST` (no token) returns 401, both editions.
- Same call with a viewer-role MCP token returns 403.
- Same call with an operator-role MCP token returns 200 in both editions.
- `curl -fsS -H "X-Forwarded-User: alice" https://localhost:8443/api/auth/whoami` (when `TrustedProxyHeader` is configured) without a token returns 401.
- Same call with a token belonging to user `bob` returns 403 `identity_mismatch`.
- The contract test suite at `test/test_edition_contract.py` passes for both `--engine` and `--studio` builds.

---

## Implementation status (end of 2026-04-25 session)

Code-complete and build-clean — runtime verification deferred to the next session.

**Done (this session):**
- `Authenticate()` rewrite — bootstrap allowlist short-circuit handled at the route level (handlers in the allowlist do not call `CheckAuth`); MCP token / session cookie / webhook HMAC are the only credential paths; gateway header is a cross-check that may downgrade role; rate limit + lockout always-on. The Studio anonymous-localhost branch is removed.
- Route restructure — `RegisterEngineRoutes()` deleted (its 2 routes moved into Common). 10 routes moved Studio → Common, each with the appropriate `CheckAuth(req, role)` gate. Studio-only routes that remain (workflow CRUD mutate, validate, scripts/file check, editor SPA) all gained explicit `CheckAuth` gates too — the anonymous bypass is gone in Studio.
- Webhook secret is mandatory in both editions. `workflowValidator.cpp` raises `webhook_secret_required` (Tier B Error) on missing/empty `params.secret`. The Studio "no secret = open webhook" branch in `HandleWebhookPost` is removed. `example/workflows/hamburg-tourist-day-planner.jcwf` got a placeholder secret. `integration/README.md`, `integration/n8n-node/README.md`, `doc/jcwf_generation_guide.md`, and the embedded generation-guide header all updated.
- `GET /api/status` `capabilities` now reports the real route-registration set: `workflow_run_endpoint`, `settings_api`, `log_analyze`, `workflow_versions`, `workflow_reload` are all `true` in both editions; the Studio-only flags (`workflow_crud`, `ai_assistant`, `ai_jcwf`) remain edition-conditional.
- Dashboard React (`dashboard/ui/`): the `isEngine` gates on the login dialog and identity display are removed. The `studio`-synthetic-user suppression in `StatusBar` is gone. Both editions now show the same `AdminLoginDialog` when `whoami` returns no user.
- `webServer.cpp` decomposed into three files:
  - `webServer.cpp` (~7.9K loc — common routes + handlers + auth funnel)
  - `webServer_studio.cpp` (~770 loc — Studio-only methods, wrapped in one `#ifdef J9T_STUDIO ... #endif` defence-in-depth guard, excluded from Engine via `removefiles` in `premake5.lua`)
  - `webServer_helpers.h` (~520 loc — `inline` helpers shared between the two .cpp files: `MakeJsonResponse`, `MakeAuthErrorResponse`, `IsValidWorkflowId`, `GetWorkflowsDirectoryAbsolute`, validator scaffolding, etc.)
  - `#ifdef J9T_STUDIO` site count in `webServer.cpp` dropped 17 → 10.
- Contract test (`test/test_edition_contract.py`) updated for the new capability set, the route moves, and the no-anonymous-/ws assertion in both editions.
- Documentation sweep: `doc/cyber security.md` (Editions table, Studio "Remaining threats", auth-paths bullet, "Public bootstrap allowlist", gateway-spoofing threat closed), `doc/api-endpoints.md` (7 section headers re-annotated), `README.md` (Editions blurb).

**Build state at end of session:** all four binaries (`bin/{Debug,Release}/jarvisAgent-{studio,engine}`) built clean. Symbol isolation verified — engine binary has 0 Studio symbols; studio has 639. Dashboard React rebuilt. `.build-edition` = `studio`.

## Next session — runtime smoke + then full testing

JC's checkpoint plan for the next session:

1. **Studio start.** `./jarvisagent.sh`. Open the dashboard. Confirm the `AdminLoginDialog` appears (no anonymous bypass). Activate the first-run admin enrollment token printed to stderr if the master key store hasn't been unlocked yet, then sign in with the resulting MCP key. Trigger one JCWF (e.g. `make-example`) from the dashboard's Run button. Confirm it succeeds.
2. **Engine start.** `premake5 gmake --engine && make config=release && ./bin/Release/jarvisAgent-engine`. Open the dashboard. Same login flow. Trigger one JCWF via `POST /api/workflows/<id>/run` (operator+ MCP key) — same workflow, confirm it runs end-to-end on Engine.
3. **API6 round-trip on Engine** (deferred §5i blocker resolution): now that `POST /api/settings/ai-interfaces` and the run route both work in Engine, re-run `python3 test/dispatch/test_api6_live.py --token mcp_...` against the engine binary. The simulator container `aoai_simulator` is still running on port 8000; the `azure-simulator-key` MCP-managed key still exists in the encrypted store; the AI interface `aoai-simulator/gpt-4/API6` was saved to disk by JC during the studio test and is in `config.json`.
4. **Contract test** end-to-end. `J9T_TOKEN=mcp_... python3 test/test_edition_contract.py --edition engine` and same for `--edition studio`. Address any failures — likely candidates: subtle role / status-code expectations diverging from current handler behaviour.
5. **Webhook smoke.** Use the updated `integration/README.md` curl example with the new HMAC-signing requirement (the hamburg JCWF placeholder secret is `demo-shared-secret-change-before-production`).

## Follow-up — two-tier rate limiting (2026-04-25, post-§5i)

`test_edition_contract.py` against the Engine binary surfaced a real design issue inherited from before §5i: the per-IP token bucket fired *before* `Authenticate()`, so a legitimate admin's contract-test bursts shared the same 20-burst / 100-req/min quota as a credential-stuffing attacker. The test's "Engine — security features" section, which deliberately fires wrong-token probes back-to-back, drained the bucket and cascaded 7 false-failures (each 429 instead of the contracted 401/403/200).

**Fix applied:** rate limiting is now two-tier and runs *after* credential validation:

- **Pre-auth (per-IP), tight:** 100 req/min, burst 20. Applies on the failure paths inside `Authenticate()` — `mcp_`-prefixed-but-invalid token, missing credential, unrecognised credential. Defends against credential-stuffing and anonymous flooding; backed by the existing `kMaxAuthFailures = 10 / 5 min → 15-min ban` lockout for persistent attackers.
- **Authenticated (per-user), loose:** 1200 req/min, burst 200. Applies once an MCP key or session has validated, keyed by the credential's user. Sized so dashboard polling, MCP heartbeats, and the contract test's ~75-request burst pass through without throttling; runaway authenticated traffic surfaces in the audit log with the user attached for investigation rather than blanket-blocked.
- **Studio short-circuit removed.** `IsRateLimited` no longer has a `#ifdef J9T_STUDIO return false`; both editions now apply the same two-tier policy. The "Editions at a Glance" / Summary tables in `doc/cyber security.md` are aligned to match.

**Code:** `application/web/webServer.{h,cpp}` — single new method `WebServer::IsRateLimited(RateLimitTier, std::string const&)` replaces the old `IsRateLimited(crow::request const&)`. `Authenticate()` reorder: lockout → credential extraction → tier-appropriate rate limit → gateway cross-check. `debug_signals` exposes both bucket map sizes (`rate_limit_buckets_preauth`, `rate_limit_buckets_authenticated`).

**Audit log:** the single `rate_limited` marker is split into `rate_limited_preauth ip=…` and `rate_limited_authenticated user=… ip=…` so the analyzer can tell an attack apart from a runaway authenticated client.

**Verification:** with the new policy, the contract test passes 75/75 on Engine without pacing changes. The §3 API6 round-trip continues to pass end-to-end.

## Open items / follow-ups

- **Dashboard "Run" button visibility on Engine.** The dashboard UI hides the Run button when `capabilities.workflow_run_endpoint` was `false`. After §5i it is `true` in Engine; verify the button now appears for operator+ users on the Engine dashboard. Likely already correct — flag if not.
- **AI-WebSocket dispatch extraction.** Inside `webServer.cpp`'s `/ws` `.onmessage` lambda there is a ~150-line `#ifdef J9T_STUDIO` block dispatching `ai-explain-jcwf` / `ai-generate-jcwf` / `ai-write-scripts` / `ai-fix-failed-script` / `chat` message types. Extracting this into a dedicated helper class (e.g. `WebServer::HandleAssistantWebSocketMessage`) and moving that helper into `webServer_studio.cpp` would drop the `#ifdef` count in `webServer.cpp` from 10 to ~7. Mechanical, but enough scope to deserve its own commit.
- **`HandleAiInterfaceTestPost` Engine fallback.** Today the Engine path returns `ai_test_not_available_in_engine`. Two cleaner options for follow-up: (a) move the test path into `aiRequestPool` so it's available without `aiJcwfService`, then both editions support it; (b) make the route Studio-only and adjust the dashboard to hide the Test button on Engine. Either is fine; (a) is more useful for Engine admins debugging interface config.
- **Sub-workflow tree route.** `GET /api/workflows/<id>/tree` now lives in Common with `viewer` gate. Verify the `WorkflowTreeView` left-sidebar component in the workflow editor still works in Studio and reports a sensible empty state in Engine (where the editor isn't shipped, but the route is still reachable for MCP introspection).
- **`HandleAiInterfaceTestPost` is in `webServer.cpp` not `webServer_studio.cpp`.** It's not entirely Studio-only any more (Engine returns a not-implemented stub), so it lives in the common file. The single inline `#ifdef J9T_STUDIO` around the `m_AiJcwfService.TestAiInterface(...)` call is one of the 10 remaining sites.
- **`POST /api/shutdown` skips the role-gate audit line.** Caught while running the operator RBAC matrix on Engine: every other admin route emits `forbidden reason=insufficient_role` when an operator's call is denied; shutdown emits only `mcp_auth_success` and silently 403s. Trace the shutdown handler's auth check and route it through `CheckAuth(req, "admin")` so the audit log captures denials of the most consequential admin action.
- **Bootstrap admin user collides with the role name.** The first-run admin's user is literally `admin`, which renders as `admin / admin` in the dashboard StatusBar (user / role badge) and confuses new operators. Either rename the bootstrap user (e.g. `j9t-bootstrap-admin`) at MCP key activation time, or have the React StatusBar suppress the role badge when the user string equals it. Cosmetic only — no security impact.

## Reference — final route surface checklist

When sanity-checking the new shape, these are the routes that must respond per edition (with valid auth):

| Route | Engine | Studio |
|-------|:------:|:------:|
| `GET /api/workflows` | viewer | viewer |
| `GET /api/workflows/<id>/tree` | viewer | viewer |
| `GET /api/workflows/dependency-graph` | viewer | viewer |
| `POST /api/workflows/<id>/run` | operator | operator |
| `DELETE /api/workflows/<id>/clean` | operator | operator |
| `POST /api/workflows/reload` | admin | admin |
| `GET /api/workflows/<id>/versions[/<ts>]` | admin | admin |
| `POST /api/workflows/<id>/versions/<ts>/restore` | admin | admin |
| `GET /api/log/analyze-last-run` | operator | operator |
| `/api/settings/ai-interfaces/*`, `/api/settings/config*`, `/api/settings/providers/*`, `/api/connections/*` | admin | admin |
| `POST /api/workflows` (CRUD create), `PUT/DELETE /api/workflows/<id>`, `POST /api/workflows/validate`, `GET /api/workflows/<id>/validate`, `GET /api/scripts/check`, `GET /api/scripts/registry`, `GET /api/files/check`, `/editor`, `/assets/<path>` | **404** | admin / viewer / public |
| `/ws/assistant`, `POST /api/chat`, AI-WebSocket message types | **404** / refuse | available |
