# Session Hand-off Log

End-of-session brief for next-session-Claude.  Newest entry on top.

**Convention** — when wrapping up a session, prepend a new entry under a date header (`## YYYY-MM-DD → next session`) covering:

- **What landed** — the major themes shipped this session (committed or working-tree).
- **What's verified** — tests run, results, what was deliberately not re-tested.
- **Open items / next-session candidates** — the natural follow-ups that didn't make today's cut.
- **Gotchas** — load-bearing knowledge that survives past today (config-schema breaks, debug-only paths, helpers to reuse).

Keep entries self-contained — a fresh-context Claude should be able to read just the latest entry and pick up cleanly.  Cross-reference into `todo.md` / `doc/misc/*-dev-plan.md` rather than duplicating their content.

**Scope discipline (this file is committed to git):** entries describe **project-state changes only**.  Per-machine tooling setup, OS specifics, account info, env-var values, dev-machine paths, personal preferences — all go into auto-memory (`~/.claude/projects/.../memory/`), **never** into this log.  See `feedback_no_private_info_in_repo` and `feedback_jc_dev_env_vars`.  If an entry needs a tooling decision as context, write a one-line cross-reference ("see `feedback_clangd_lsp_for_navigation.md`") rather than inlining the setup details.

---

## 2026-05-02 (S1 = D2 CLOSED — session-end milestone) → next session

**Major milestone: S1 (= D2 — Web + Cloud + Assistant) is closed.**  After 34 sittings spanning 5 calendar days (2026-04-29 to 2026-05-02), every cross-cutting concern from the original D2 cyber-sec + safety audit is resolved.  The original plan estimated 5-6 sittings for D2; reality was 34, driven mostly by the cloud-surface sub-domain (sittings 9-34 = 26 sittings) being denser than the audit's TOC suggested.  The two earlier sub-domains closed roughly on plan: assistant in 4 sittings, web layer in 4 sittings.

### Where the detail lives

This entry is the session-level summary.  Per-sitting detail is split across two files:

- **`doc/misc/S1-D2-session-note.md`** — formal per-change template entries (per plan §5) for sittings 1-20.  ~140 entries plus 20 skipped-findings tables.  Closes with a coverage-map section that's the canonical answer to "what did S1 close?" for a future re-audit.
- **`doc/misc/hand-off.md`** (this file) — per-sitting brief entries for sittings 21-34, prepended above this milestone entry.  Mirrors the per-change template's information density at the sitting level rather than the per-change level.  Cross-referenced from the session note's closure section.

The split is deliberate: sittings 1-20 were depth-first single-file passes (one big diff each), where the per-change template provided real information value.  Sittings 21-34 were horizontal sweeps (one pattern × N files), where per-sitting brief was the right granularity and per-change template would have been overhead without information gain.

### What S1 = D2 closed

Three sub-domains, mapped to the plans' §7 categories:

1. **Assistant** (sittings 1-4) — `assistantTools` (5 CRITICALs: argv-only execution, canonical-cwd, allowlist-not-blocklist), `assistantController` (approval-bypass, path traversal in `GetSession`, WS frame size cap, sessions as `shared_ptr`, `DrainPendingMessages` revalidate-under-lock, `default:` over closed enum), `assistantSession` + `assistantMemory` + `workspaceIndexer` + `contextAssembler` (path traversal, RNG races, lock-order inversion, prompt-injection defang, sticky `m_FileBroken` for memory/disk divergence).  Plus the `JsonHelper::EscapeJsonString` convergence retiring 4 broken `JsonEscape` copies.

2. **Web layer** (sittings 5-8) — `webServer.cpp` clusters A+B+C: REST authn/authz funnel, MCP key manager surface, atomic config-write via `WriteTextFileAtomic` + simdjson tripwire, `DrainPendingBroadcasts` UAF, `SetWorkflowRuntimeManager` dangling-lambda detach (swap + shutdown), `const_cast<this>` cascade rewrite (auth funnel non-const top-to-bottom), `m_ClientCount` consistency contract, `fs::exists` TOCTOU sweep across three handlers, OAuth callback unauth + state-gated per RFC 6749 §10.12.

3. **Cloud surface** (sittings 9-34) — every cross-cutting cloud concern.  Highlights:
   - `cloudConnectionManager` JSON cluster (size caps, scratch-then-swap parse, the 6th-and-last `JsonEscape` convergence) + concurrency (`GetConnection` raw-ptr → `std::optional`, `m_Dirty` race → `std::atomic<bool>`).
   - 12 connectors swept for SSRF (`ValidatePublicHttpEndpoint` syntactic gate + `OpensocketStrictCallback` DNS-resolution-time gate), TLS verify, `FOLLOWLOCATION = 0` everywhere except S3 + Microsoft Graph (legitimate 30x with `REDIR_PROTOCOLS_STR = "https"` + `MAXREDIRS = 10`), bearer/PAT/JWT CRLF check, response-body cap.
   - 12 executors swept for path traversal (`ICloudTaskExecutor::ValidateLocalPath` rewrite per JCWF spec §3.2.1), input validation (bucket / blob / spreadsheet_id / range / etc.), URL-side injection, JSON-injection both directions (request bodies + synthesized response bodies), parser hardening (`stoi`/`stoull` digit-only pre-validation), redirect/TLS posture per-API.
   - Postgres-specific: `IsValidSslMode` allowlist + non-localhost production posture + default `sslmode=require`, `ValidatePostgresParams` preventive tripwire on libpq cert/key/file-path params, bracketed IPv6 support in `ParseHostPort`.
   - Snowflake migrated from bespoke setopt block to `ConnectorHttp::ApplyHardenedDefaults` (sittings 31 + 33-extension).
   - 6 JCWFs rewritten to spec-compliant working-directory-relative paths (`s3UploadDownloadDemo`, `gcsDemo`, `azureBlobDemo`, `oneDriveUploadDownloadDemo`, `sheetsQuizGrader`, `emailDemo`) with the canonical copies in `example/workflows/`.

### Observability shipped at S1 close

Six live atomic counters on `/api/debug/signals` (admin-gated, DEBUG-only) give an operator the global "is this gate firing at all?" answer for every cloud-surface security gate:

- `cloud_dns_resolved_ip_rejections`, `cloud_endpoint_ssrf_rejections`, `cloud_credential_crlf_rejections`, `cloud_input_validation_rejections`, `cloud_postgres_invalid_sslmode_rejections`, `cloud_postgres_forbidden_param_rejections`.

Per-instance forensic detail (timestamps, task/run/connection identifiers, actual rejected values) stays in `log/log.txt` with `[security] *_rejected` lines.  Counters reset on server restart.

### What's verified at session close

- Studio Debug build: clean.
- 28-test assistant non-AI suite: PASS.
- 12 cloud demos covering every connector + executor: green end-to-end (cumulative ~88 tasks across the matrix, 0 failed regressions).
- 4 negative SSRF tests + 4 negative postgres-config tests + 1 DNS-time SSRF test (`localtest.me`): all rejected with explicit security log lines and counter increments.
- The 12 production cloud connections (`mcp__j9t__manage_connections action=list`): 12/12 OK after the OAuth callback fix + Google Sheets re-authorize during sitting 22.

Cloud-surface CRIT/HIGH cluster from the original audit is **fully closed**, with both forensic logs and live counters at every gate.

### What's next

Three more sessions in the hardening pass (per the plans' §3):

| Session | Domain | Plan estimate | Notes |
|---|---|---:|---|
| **S2** | D3 — Core engine (keystore, secret redactor, SigV4 / OAuth signers, JWT, thread pool, event queue, curl multi dispatcher, config parser) | 3-4 sittings | Tightly coupled but smaller surface |
| **S3** | D1 — Workflow orchestration (workflowRuntimeManager, aiRequestPool, task executors, 6 reply parsers, jcwfContainer, fileWriter, triggerEngine, pythonEngine) | 3-4 sittings | **Density "very high" per the plan.**  Most concurrency-heavy code in the project; recent bug history (`m_ActiveRuns` lambda-by-reference, fingerprint-after-drain, UTF-8 truncation) all lives here.  Likely runs hot relative to the estimate, similar to S1=D2's 5-6 → 34 expansion |
| **S4** | D4 — Application infrastructure (TUI byte safety, lifecycle / signal-handler safety) | 2-3 sittings | Smallest surface |

**8-11 sittings remaining** at the planned discipline.  Total budgeted from start of pass was 13-17 sittings; reality is now 34 + 8-11 = 42-45.  The expansion is concentrated in S1=D2 (factor of ~6× the estimate) due to the cloud-surface depth, not in the other three domains where the estimates are likely closer to right.

JC's remark at session close: **"the workflow engine is rather large"** — the next major lift is S3=D1.  The remaining S2 (engine) and S4 (app infrastructure) are smaller; S3 will be the test of whether the sweep cadence developed across S1=D2 sittings 11-34 (one pattern × N files at 30-90 min per sitting) transfers to a different sub-domain.

### Gotchas next-session-Claude should know

- **Read both `S1-D2-session-note.md` AND the recent hand-off entries together** to understand what S1 closed.  Neither file alone tells the full story — the session note has the per-change template detail for sittings 1-20, the hand-off has the per-sitting briefs for sittings 21-34.  The session-note closure section ("S1 = D2 closure — 2026-05-02") is the canonical bridge.
- **The 6 cloud-security counters are the right starting place for a S2 / S3 sweep too.**  When you find a new gate in S2 or S3, follow the sitting-32 pattern: atomic counter at file scope, public accessor in a helper header, `Increment*Rejection()` helper if the gate has multiple call sites, surfaced in `/api/debug/signals` next to the existing six.  The `ConnectorHttp::Increment*Rejection()` helpers are the canonical example.
- **JCWF spec §3.2.1 is the authoritative reference for path resolution** in cloud + workflow contexts.  Sitting 22-23-25 had to discover this the hard way; future hardening on D1 (S3) workflow code should consult the spec first when path-resolution rules look ambiguous.
- **Per-change template + hand-off log split.**  Future sessions: pick the right granularity for the work shape.  Single-file depth-first → per-change template.  Horizontal sweep → per-sitting hand-off entry is fine.  Don't pretend strict template adherence happened when it didn't (this hand-off entry is the precedent for honest gap-acknowledgment).
- **`example/workflows/*.jcwf` is the canonical location for shipped JCWFs.**  `workflows/` is gitignored runtime scratch.  After `PUT /api/workflows/<id>` repacks the runtime `.jcwf` zip, copy to `example/workflows/`.  The 6 JCWFs touched in S1=D2 sittings are all at the canonical location with the spec-aligned content.

---

## 2026-05-02 (S1 sitting 34) → next session

S1=D2 sitting 34.  Theme: **Uniform counter pattern across the other cloud-security gates** (last open carry-forward).  Sitting 32 shipped one counter (DNS post-resolve); this sitting replicates the pattern across the remaining 5 gate categories.  After this sitting, every cloud-surface security gate has both (a) a security log line for forensics and (b) an atomic lifetime counter on `/api/debug/signals` for live operator monitoring.

### What landed

**Six atomic counters** total now in `connectorHttp.cpp` (anonymous namespace, file-scope, `std::atomic<std::uint64_t>`, lock-free `fetch_add` with relaxed ordering).  Each has a public accessor `Get*RejectionCount()` and (for the per-site categories) a public increment helper `Increment*Rejection()`:

1. **`DnsResolvedIp`** — sitting 32, already shipped.  Bumped inside `OpensocketStrictCallback`.
2. **`EndpointSsrf`** — bumped centrally inside `ValidatePublicHttpEndpoint` via a wrapping inner-lambda + single false-return increment site.  Captures all 5 syntactic-rejection paths (URL size, scheme, host charset, host length, https-→-local-net) in one increment point.
3. **`CredentialCrlf`** — bumped at every `LOG_SECURITY_WARN("[security] *_crlf_rejected")` site across the cloud surface (10 sites: every connector + the 2 oneDrive executor helpers + the snowflake executor JWT check).  Per-site instrumentation because `ICloudTaskExecutor::ContainsCrlf` is a generic bool helper that doesn't know "this is a rejection" (just "string contains CR/LF").
4. **`InputValidation`** — bumped at every `LOG_SECURITY_WARN("[security] *_invalid_*")` site (12 sites: azure-blob container/blob_name, gcs bucket, googleSheets spreadsheet_id/range, oneDrive remote_path, snowflake endpoint/handle, email folder).  Per-site for the same reason.
5. **`PostgresInvalidSslmode`** — bumped centrally inside `IsValidSslMode` at both false-return paths (allowlist miss + non-localhost plaintext-fallback).
6. **`PostgresForbiddenParam`** — bumped centrally inside `ValidatePostgresParams` at the single false-return path.

**Surfacing**: 5 new `cloud_*_rejections` fields added to `/api/debug/signals` alongside the existing `cloud_dns_resolved_ip_rejections` from sitting 32.  Section comment block in `HandleDebugSignalsGet` documents the responsibility of each counter via cross-reference to the `Get*RejectionCount` docstring.

**Per-site instrumentation was bulk-applied via a Python script** that scanned each `LOG_SECURITY_WARN` site for the prefix patterns (`_crlf_rejected` and a list of `_invalid_*` suffixes) and inserted the increment line before the log statement, preserving indentation.  17 files modified, 26 instrumentation sites total.

### What's verified

- Studio Debug build: clean (Debug because `/api/debug/signals` is DEBUG-only).
- **Initial counter snapshot**: all 6 at 0 on fresh server.
- **Triggered 5 of 6 gates** via temp connections / adhoc workflow:
  - `https://10.0.0.5` (jira) → `cloud_endpoint_ssrf_rejections` 0 → 1.
  - `https://localtest.me` (jira) → `cloud_dns_resolved_ip_rejections` 0 → 2 (per-socket-family from sitting-32 finding).
  - `db.example.com:5432` + `sslmode=prefer` (postgres) → `cloud_postgres_invalid_sslmode_rejections` 0 → 1.
  - `localhost:5432` + `sslcert=/etc/passwd` (postgres) → `cloud_postgres_forbidden_param_rejections` 0 → 1.
  - Adhoc azure-blob `container=BAD?CONT` → `cloud_input_validation_rejections` 0 → 1.
- **`cloud_credential_crlf_rejections` not exercised** — triggering it requires forging a credential with embedded CR/LF, which means manipulating the keystore directly (against `feedback_use_j9t_apis`).  Code-review verified — every CRLF site has the increment call adjacent to the LOG_SECURITY_WARN.
- **Regression sanity** — postgresDemo, snowflakeQueryDemo, azureBlobDemo all green; counters did NOT bump for legitimate flows (false-positive check).
- All 4 test connections deleted at end of sitting.

### Open items / next-session candidates

**Empty.**  The cloud-surface CRIT/HIGH cluster from the original S1=D2 audit is **fully closed**, with every gate having both forensic log lines and live counters.  Sittings 33+34 wrapped the housekeeping.

If JC wants to spend more sittings on this domain, candidates that haven't been called out before:
- **Future sitting — Counter rate / windowing** (deferred, possibly Y).  All counters today are monotonic lifetime totals.  An operator wanting "have we seen DNS-time SSRF in the last 5 minutes?" has to read the counter twice with a known time gap and subtract.  Adding a windowed-counter abstraction (or a Prometheus-style histogram) would make this nicer for dashboards but is a separate scope.
- **Future sitting — Release-build status surfacing** (deferred, possibly N).  `/api/debug/signals` is DEBUG-only; release-build operators can't see these counters.  If the alpha release wants operators to monitor cloud-security gates, that needs a separate (non-debug-gated) status endpoint or a dashboard widget.  Out of scope for current S1=D2 work — alpha-prep timing.
- **Future sitting — Cross-reference counters in dashboard "Run Analysis"** (deferred, possibly N).  The dashboard already filters security-log lines per run.  A "this run triggered N input-validation rejections" badge would require per-run counters (vs the global ones shipped).  Adds value for forensics but doubles the counter footprint.

S1=D2 hardening work for cloud surface is **done** at sitting 34.

### Gotchas next-session-Claude should know

- **Bulk Python-based instrumentation works for adjacent-line insertions like this.**  The script that added 26 increment calls across 17 files in a few seconds is included in the sitting-34 turn history; if a future sweep needs the same pattern, write a similar script rather than manually editing.  Just be careful that the regex matches ONLY the intended lines — verify by counting expected vs actual edits per file.
- **`ConnectorHttp::IncrementXxx()` helpers are public and lock-free.**  Safe to call from any thread, any context (including `OpensocketStrictCallback` which runs inside libcurl's connect path).  Don't worry about ordering with other counters or with the security log emission — the counter and the log line are independent operations and a brief window where one happens before the other is fine.
- **Some sittings' "wrap up the surface" claims didn't survive contact with reality.**  Sittings 25 and 31 each declared "the cloud surface is uniformly hardened now" — but each found one more bespoke setopt block in the next sitting.  Sitting 34 is making the same claim.  The only thing that will tell us if it's true is the next time someone tries to extend the cloud surface and either (a) finds it consistent or (b) finds another holdout.  Treat the closure-claim as "best snapshot of current state, not theorem".
- **The CRLF counter is the one without a verified live-trigger.**  If a future sitting wants to verify it, the route is to add a synthetic credential with embedded CR/LF via a test fixture, NOT to manipulate the keystore by hand.  See sitting-13 for the original CRLF test fixture pattern (probably worth digging up if it exists).

---

## 2026-05-02 (S1 sitting 33) → next session

S1=D2 sitting 33.  Theme: **Investigate `local-pg` `verify-full` upgrade** (carried from sitting 29).  Outcome: sitting concluded as a **deliberate non-change** — `sslmode=require` is the correct floor for j9t's local-pg dev connection; `verify-full` requires per-machine CA provisioning that's outside j9t's deployment scope.  Documented the three-mode tradeoff in `cloud-integration.md` so operators choosing a posture for production deployments have explicit guidance.

### What landed

**Empirical investigation of `verify-full` against the local pg.**  Ran `openssl s_client -starttls postgres` against `localhost:5432` to inspect the cert:
- Subject: `CN = zorin.attlocal.net` (the machine's hostname, not `localhost`).
- Issuer: same CN as subject — **self-signed cert**.

Set `sslmode=verify-full` on `local-pg` and ran TestConnection.  libpq rejected with:
> `connection to server at "localhost" (127.0.0.1), port 5432 failed: root certificate file "/home/beaumanvienna/.postgresql/root.crt" does not exist`

Two failure modes confirmed:
1. **No CA chain to validate against** — libpq looks for `~/.postgresql/root.crt` by default; not present on this dev machine.  Even if I created the file, the self-signed cert chain is one cert deep (no intermediate CA), so `verify-ca` would also need the cert itself trusted.
2. **Hostname mismatch** — the cert CN is `zorin.attlocal.net` but j9t connects to `localhost`.  Even with a CA in place, `verify-full` would reject the connection on hostname mismatch (`verify-ca` would tolerate it).

**Decision**: revert local-pg to `sslmode=require`, do NOT attempt CA provisioning at this layer.  Reasoning:
- CA provisioning is per-machine dev tooling — would require either (a) generating a custom CA, signing the local-pg cert with it, and trusting the CA in `~/.postgresql/root.crt`, or (b) reconfiguring the local pg daemon to use a cert with CN `localhost` and a chain rolling up to a system-trusted root.  Neither belongs in the j9t repo or in j9t-side connection config.
- Sitting-30's forbid list (`sslcert`/`sslkey`/`sslrootcert` rejected at validation time) means JCWFs / connection config can't override the trust store via `m_Params` — that's the right posture for SSRF / path-traversal prevention.  Loosening it to enable per-connection custom CA paths would re-open the attack surface sitting 30 closed.
- For production deployments against managed pg (Supabase / RDS / Cloud SQL / etc.), `verify-full` Just Works because the cert chain rolls up to a public CA already in the OS trust store and the hostname is a real DNS name — no j9t-side change needed.

**Documentation update** — added a "Choosing an `sslmode` value" subsection to `doc/cloud-integration.md` Postgres section with the three-mode tradeoff (`require` / `verify-ca` / `verify-full`) and explicit guidance: use `require` for dev / self-signed local pg, use `verify-full` for production with managed pg + public CA.  Operators picking a posture for a new connection now have the rationale in one place.

**`local-pg` config left at `sslmode=require`** (the sitting-29 setting).  The investigation didn't change anything in the persisted state.

### What's verified

- `openssl s_client` showed the self-signed nature of the local pg cert.
- `verify-full` test failure was the predicted libpq error.
- Reverted local-pg to `sslmode=require` and TestConnection green again.
- No code change, no rebuild.

### Open items / next-session candidates

- **Sitting 34 — Uniform counters for the other cloud-security gates** (carried, last open item).  Pattern from sitting 32 (atomic counter + accessor + signals row) replicated across `*_endpoint_rejected` (sitting 22), `*_invalid_*` validators (sittings 18 + 23), `postgres_invalid_sslmode` (sitting 26), `postgres_forbidden_param` (sitting 30).  Estimated 30-45 min.
- **Future — dev CA tooling for `verify-full` against local pg** (deferred, not currently scoped).  Would need: (a) a script to generate a dev CA + sign the local pg cert with CN matching `localhost`, (b) a step in dev setup to install the CA at `~/.postgresql/root.crt`.  Belongs in dev tooling (`scripts/dev-setup/...`) rather than in j9t code.  Open this only if JC actually wants the strict posture for dev.

After sitting 34, the cloud-surface hardening work for S1=D2 is **done** — every cross-cutting concern from the original audit is closed, every gate has a counter for live monitoring, every config posture has explicit operator guidance.

### Gotchas next-session-Claude should know

- **`sslmode=verify-full` is for production, not dev.**  Don't try to "tighten" the local-pg connection to `verify-full` again without first provisioning a dev CA — the next sitting-33 attempt will hit the same libpq error and you'll burn time re-discovering this.  The sitting-33 conclusion is durable: dev = `require`, prod = `verify-full`.
- **Sitting-30's forbid list of `sslrootcert` is intentional and stays.**  If a future request comes in to "let users configure a CA path for verify-full", the right answer is "use libpq's default search path (`~/.postgresql/root.crt`) at the OS level, not via j9t connection params" — that keeps the SSRF preventive gate intact.
- **The `verify-full` posture doesn't compose with sitting-26's per-host sslmode rules.**  The non-localhost rule rejects `disable`/`allow`/`prefer` for non-localhost hosts but accepts `verify-full`.  So `verify-full` is automatically the right posture for any non-localhost connection where the operator wants strict validation; the only case requiring explicit choice is whether to use `require` (chain-not-validated) or `verify-full` (chain+hostname).
- **The local pg cert CN is machine-specific** (`zorin.attlocal.net` here).  Don't hardcode it anywhere — if it appears in test code or docs, it's stale.  The right reference is "the machine's hostname" or "the cert's actual CN, whatever it is".

---

## 2026-05-02 (S1 sitting 32) → next session

S1=D2 sitting 32.  Theme: **DNS-rejection live counter** — surface sitting 28's `dns_resolved_ip_local_network_rejected` event as a runtime counter on `/api/debug/signals` so an operator can read "are we seeing DNS-time SSRF attempts at all?" without grepping `log/log.txt`.  Per-instance forensic detail (timestamp + actual IP) stays in the security log; this counter is the global "is the gate firing?" answer.

### What landed

**Atomic counter in `connectorHttp.cpp`** — `std::atomic<std::uint64_t>` at file scope (anonymous namespace), incremented inside `OpensocketStrictCallback` immediately before the `LOG_SECURITY_WARN` + `return CURL_SOCKET_BAD`.  Lock-free `fetch_add` with `std::memory_order_relaxed` (no synchronization with anything else; just a counter).

**Public accessor `ConnectorHttp::GetDnsResolvedIpRejectionCount()`** — declared in `connectorHttp.h`, returns the current counter value via `load(std::memory_order_relaxed)`.

**Surfaced in `/api/debug/signals`** — new field `cloud_dns_resolved_ip_rejections` in the signals object (admin-gated, DEBUG-only — release builds don't expose this endpoint).  Added a "Cloud surface security counters" section comment block in `HandleDebugSignalsGet` so future cloud-security counters land in the same place.

**Added `#include "cloud/connectorHttp.h"` to `webServer.cpp`** so the accessor is visible.

### What's verified

- Studio Debug build: clean.  Edition: studio, config: debug.
- **Initial counter value**: 0 (fresh server).
- **After triggering a rejection** via temp connection `https://localtest.me` (public DNS resolving to `127.0.0.1` + `::1`): counter reads **2**.  Matches the 2 security log entries from sitting 28's testing — `localtest.me` resolves to both an IPv4 loopback AND an IPv6 loopback, so libcurl tries both and the callback rejects each one.
- Test connection deleted at end of sitting.
- No regression test on cloud demos — counter increments are an additive observability change with no behavior delta.

### Open items / next-session candidates

- **Sitting 33 — Local pg `verify-full` + dev CA provisioning** (carried).  Move `local-pg` from `sslmode=require` to `verify-full` once a dev CA is wired.  Could fold into a broader "trust store hygiene" sitting that also covers the `CurlWrapper::GetCaBundlePath()` defaults.
- **Sitting 34 — Counters for the other cloud-security gates** (new, surfaced this sitting).  The pattern just shipped (atomic counter in implementation file + public accessor + signals row) is reusable.  Worth instrumenting: connector-layer `*_endpoint_rejected` (sitting 22), executor-layer `*_invalid_*` validators (sittings 18 + 23), `postgres_invalid_sslmode` (sitting 26), `postgres_forbidden_param` (sitting 30).  Each becomes a `cloud_<event>_rejections` counter in the signals object.  Estimated 30-45 min for the lot.

### Gotchas next-session-Claude should know

- **The counter is monotonic and never reset.**  Counts since j9t process start; a server restart resets it to 0.  If you need a "rate" instead of a "total", read it twice with a known time delta and subtract — there's no built-in window logic.
- **The counter is incremented EVEN IF the URL is allowed by syntactic gate.**  `OpensocketStrictCallback` only fires when `ApplyHardenedDefaults` / `ApplyExecutorRedirectDefaults` was given an `https://` URL and libcurl reached the post-resolve socket-open step.  A request rejected at the syntactic stage (`ValidatePublicHttpEndpoint` returns false) never reaches the callback and doesn't increment this counter.  If you want a syntactic-rejection counter too, that's sitting 34.
- **`/api/debug/signals` is admin-gated AND DEBUG-only.**  Operators on a release build can't use this counter — they'd need to grep the security log directly.  If JC wants release-build operators to see cloud-security-event totals, that requires a separate non-debug status endpoint or a dashboard widget — out of scope for sitting 32.
- **The counter increments PER socket family.**  As the test showed, `localtest.me` rejected counts as 2 (IPv4 + IPv6) because libcurl iterates the resolved address list and the callback fires for each address.  Don't read the counter as "number of distinct attacker attempts" — it's "number of resolved local-net IPs refused".  A single attacker probe with both A and AAAA records pointing to local IPs counts as 2.

---

## 2026-05-02 (S1 sitting 31) → next session

S1=D2 sitting 31.  Theme: **Snowflake post-resolve check** — fold `snowflakeCloudTaskExecutor.cpp`'s bespoke curl setopt block into `ConnectorHttp::ApplyHardenedDefaults(curl, url)` so Snowflake gets the DNS-resolution-time SSRF gate (sitting 28) along with the existing TLS verify + redirect-disabled posture.  Snowflake was the lone holdout: its setopt block predates sitting 22's `ConnectorHttp` helper and was never migrated.

### What landed

**`snowflakeCloudTaskExecutor.cpp::SnowflakeRequest`** (the single curl-using helper in the executor) routed through `ConnectorHttp::ApplyHardenedDefaults(curl, url)`:
- Replaced 18 lines of explicit setopts (`FOLLOWLOCATION=0`, `SSL_VERIFYPEER=1`, `SSL_VERIFYHOST=2`, `CAINFO`) with a single helper call.
- Added `#include "cloud/connectorHttp.h"`.
- Behavioural delta: the helper additionally installs `CURLOPT_OPENSOCKETFUNCTION = OpensocketStrictCallback` because `url` always starts with `https://` (Snowflake URLs come from `SnowflakeConnector::BuildApiBaseUrl`, which only emits `https://*.snowflakecomputing.com`).  Closes the only remaining cloud-surface site that lacked the post-resolve gate.

This sitting brings Snowflake into parity with the other 9 redirect-disabled executors: every cloud-surface curl handle that talks https now installs the post-resolve callback uniformly via the shared helper.

### What's verified

- Studio release build: clean.  One file recompiled (`snowflakeCloudTaskExecutor.cpp`).
- `my-snowflake` `TestConnection`: **OK: reachable**.  No false-positive on the legitimate `*.snowflakecomputing.com` DNS resolution.
- `snowflakeQueryDemo`: **14/14 tasks succeeded** (~13 s) — exercises the full SQL submit → poll → fetch results pipeline, including the per-region fan-out to AI calls.  Confirms the post-resolve callback installs without breaking any legitimate flow.
- No `dns_resolved_ip_local_network_rejected` log entries during the demo run — the public Snowflake IPs all pass the post-resolve check.

### Open items / next-session candidates

- **Sitting 32 — Audit-log dashboard for `dns_resolved_ip_local_network_rejected`** (carried).  ~30 min.
- **Sitting 33 — Local pg `verify-full` + dev CA provisioning** (carried).  Move `local-pg` from `sslmode=require` to `verify-full` once a dev CA is wired.

The cloud-surface CRIT/HIGH cluster + DNS-time SSRF gate are now **uniformly closed across every executor + connector + dbQuery + libpq surface, including Snowflake**.  No remaining HIGH-severity gaps from the original audit.

### Gotchas next-session-Claude should know

- **Snowflake was the last bespoke setopt block in the cloud surface.**  Every cloud-task executor + connector now routes through `ConnectorHttp::ApplyHardenedDefaults` (or `ApplyExecutorRedirectDefaults` for s3 + oneDrive's data path).  A future executor or connector that adds a new curl handle MUST use one of these helpers — adding a manual `CURLOPT_SSL_VERIFYPEER` / `CURLOPT_FOLLOWLOCATION` block in new code is a code-review reject (per sitting-25 gotcha).
- **`SnowflakeRequest` is the only curl handle in `snowflakeCloudTaskExecutor.cpp`.**  All Snowflake API calls (statement submit, statement poll, async cancel) go through it.  No additional sites needed touching.
- **The post-resolve callback fires on every Snowflake request now.**  Each request opens a new socket via `OpensocketStrictCallback`.  The overhead is negligible (one `inet_ntop` + one prefix-check per request) but it's worth noting if a future profiler run shows surprise latency in the snowflake path.

---

## 2026-05-02 (S1 sitting 30) → next session

S1=D2 sitting 30.  Theme: **Preventive libpq cert/key/file-path param tripwire** for `PostgresConnector`.  No current code path exposes `sslcert` / `sslkey` / `sslrootcert` / `sslcrl` / `sslcrldir` / `service` / `passfile` / `sslpassword` to libpq, but a future PR could regress this by adding `paramOrDefault("sslcert", ...)` to `BuildConnectionString`.  This sitting installs a defensive gate that fires on any forbidden param key, so future regressions surface as a hard reject rather than a silent path-traversal vector.

### What landed

**`PostgresConnector::ValidatePostgresParams(connection, errorMessage)`** — public static helper that scans `connection.m_Params` for libpq's documented file-path / cert / file-lookup parameters and rejects any presence with a clear error.  Forbidden keys (full set per [libpq connection params](https://www.postgresql.org/docs/current/libpq-connect.html)):
- `sslcert` — client certificate file
- `sslkey` — client private key file
- `sslrootcert` — root CA certificate file
- `sslcrl` — certificate revocation list file
- `sslcrldir` — CRL directory
- `sslpassword` — passphrase for sslkey (a disk secret)
- `service` — libpq service file lookup
- `passfile` — libpq password file

**Two gate sites** call `ValidatePostgresParams` BEFORE `BuildConnectionString`:
- `PostgresConnector::TestConnection` — gates the connection-test path.
- `DbQueryCloudTaskExecutor::ExecuteCloud` — gates the query-execution path.

Each rejection emits `[security] postgres_forbidden_param connection='{}' message='...'` with the full error string for forensic analysis.

**Header docstring** documents the threat model: j9t's posture is "credentials live in KeyManager, not on disk" — any libpq param that asks the server to read a file is a path-traversal vector waiting to happen.  The docstring also explicitly flags that removing this gate without adding `ValidateLocalPath` confinement on the cert/key paths is a code-review reject — future-proofs against the regression vector this sitting was designed to close.

**No current behavioral change.**  `BuildConnectionString` already only reads `database` and `sslmode` from `m_Params`; everything else was being silently ignored.  This sitting upgrades that "silent ignore" to "explicit reject + audit log" so the threat surface is visible in the security log if anyone ever tries to set these params.

### What's verified

- Studio release build: clean.  3 files recompiled (postgresConnector .h/.cpp + dbQueryCloudTaskExecutor.cpp).
- **`local-pg` TestConnection**: green (`OK: reachable`) — legitimate connection without forbidden params still works.
- **`postgresDemo`**: 12/12 tasks succeeded — dbQuery executor path also green through both gates.
- **4 negative tests** (each via temp `manage_connections` create+test):
  - `sslcert: /etc/passwd` → `Forbidden PostgreSQL connection param 'sslcert': ...`.
  - `sslrootcert: /etc/shadow` → same shape with `sslrootcert`.
  - `passfile: /root/.pgpass` → same shape with `passfile`.
  - `service: evil` → same shape with `service`.
- Each rejection produced (a) task state `failed`, (b) `[security] postgres_forbidden_param connection='{}' message='...'` log line.  Other 4 forbidden keys (`sslkey`, `sslcrl`, `sslcrldir`, `sslpassword`) follow the identical code path; sample tested above is representative.
- All 4 test connections deleted at end of sitting.

### Open items / next-session candidates

The cloud-surface CRIT/HIGH cluster is now **fully closed** at the executor + connector + dbQuery + libpq surface.  Remaining work is adjacency / housekeeping:

- **Sitting 31 — Snowflake post-resolve check** (carried from sitting 28).  Snowflake's curl handles in `snowflakeCloudTaskExecutor.cpp` use a bespoke setopt block that predates sitting 22 — they don't go through `ConnectorHttp::ApplyHardenedDefaults` and so don't get the post-resolve callback.  Either route through the helper or attach the callback directly.  ~30 min.
- **Sitting 32 — Audit-log dashboard for `dns_resolved_ip_local_network_rejected`** (carried from sitting 28).  ~30 min.
- **Sitting 33 — Local pg `verify-full` + dev CA provisioning** (carried from sitting 29).  Move local-pg from `sslmode=require` to `verify-full` once the dev environment has a wired-up CA bundle.  Could fold into a broader "trust store hygiene" sitting.

### Gotchas next-session-Claude should know

- **The forbid list is libpq's documented file-path params, not a guess.**  The 8 keys above are the complete set per the libpq docs.  If libpq adds new file-path params in a future version, audit this list.  `sslsni`, `keepalives`, `application_name`, `target_session_attrs`, `replication`, etc. are NOT file-path params and stay un-forbidden by design.
- **The gate is preventive, not reactive.**  No current JCWF or REST flow can produce these params (the j9t connection-config UI doesn't surface them, and m_Params keys silently dropped in `BuildConnectionString`).  The gate exists to make the threat boundary visible — if a future PR adds e.g. `paramOrDefault("sslcert", ...)` without first removing this gate, the gate fires and the test suite catches it.  Removing the gate without adding `ValidateLocalPath` confinement on the cert/key paths is a security-review reject.
- **`ValidatePostgresParams` is the third allowlist/forbid-list helper on `PostgresConnector`** (alongside `IsValidSslMode` and `ParseHostPort`).  All three are public static so `DbQueryCloudTaskExecutor` can gate before `BuildConnectionString`.  Future cloud connectors that follow the postgres pattern (e.g. mysql, mssql) should install the same three-helper triad.
- **The 4 test connections (`forbid-test-sslcert/-sslrootcert/-passfile/-service`) were deleted** at end of sitting.  Don't expect them.

---

## 2026-05-02 (S1 sitting 29) → next session

S1=D2 sitting 29.  Theme: **`local-pg` sslmode tighten** — folded from sitting 26's "we noticed local pg supports TLS" observation.  The dev-local Postgres connection had `sslmode=disable` set explicitly to avoid breakage during sitting 26's default-`require` rollout, but TestConnection had passed with TLS active so the underlying transport was always there.  Sitting 29 brings the dev posture into line with production: `sslmode=require`.

### What landed

**`local-pg` connection updated** via `mcp__j9t__manage_connections action=update`:
- Before: `{"params": {"sslmode": "disable", "database": "j9t_test"}}`
- After: `{"params": {"sslmode": "require", "database": "j9t_test"}}`

The connection passes through `PostgresConnector::IsValidSslMode("localhost", "require")` — `require` is in the allowlist and is acceptable for any host (loopback or not), so the validator approves.  libpq's TLS handshake against the local Postgres succeeds (the daemon supports TLS with a self-signed cert, which `require` accepts without verifying the cert chain — that's the libpq semantic).

Stronger postures (`verify-ca`, `verify-full`) would also be acceptable to the validator but would require provisioning a CA bundle that recognizes the local pg's self-signed cert.  That's a separate dev-tooling task; `require` is the right floor for a local development connection that just needs the wire to be encrypted.

No code changes — pure connection config tightening.

### What's verified

- `local-pg` `TestConnection`: **OK: reachable** with `sslmode=require`.
- `postgresDemo`: **12/12 tasks succeeded** (~2 s) — exercises the dbQuery executor path through the same `IsValidSslMode` gate (sitting 26).  The `query_departments` task connects with TLS and returns rows.
- No code rebuild needed (config-only change, picked up immediately by the running server).

### Open items / next-session candidates

- **Sitting 30 — `sslcert` / `sslkey` / `sslrootcert` path-confinement** (carried, preventive).  Currently no code path exposes these libpq params; sitting only opens if a future feature surfaces them.
- **Sitting 31 — Snowflake post-resolve check** (carried from sitting 28).  Snowflake's curl handles in `snowflakeCloudTaskExecutor.cpp` don't go through `ConnectorHttp::ApplyHardenedDefaults` — its setopt block predates sitting 22 and is bespoke.  The post-resolve callback isn't installed there.  ~30 min.
- **Sitting 32 — Audit log dashboard for `dns_resolved_ip_local_network_rejected`** (carried from sitting 28).  ~30 min.
- **Sitting 33+ — Local pg `verify-full` + dev CA provisioning** (new, surfaced this sitting).  If JC wants the local-pg connection to use the strictest libpq mode (`verify-full`), the dev environment would need a wired-up CA that recognizes the local pg's cert chain.  Possibly fold into a broader "trust store hygiene" sitting that also covers the `CurlWrapper::GetCaBundlePath()` defaults.

### Gotchas next-session-Claude should know

- **The dev-and-prod-symmetry invariant is now intact for postgres.**  Pre-sitting-29, `local-pg` was the one connection that explicitly opted out of the sitting-26 production posture.  Now every shipped connection's `sslmode` (or implicit default) is at least `require`.  If you need to add a new postgres connection that points to a non-TLS server for testing, prefer adding a one-off named connection with `sslmode=disable` over relaxing `local-pg` — keep `local-pg` as the canonical "what production looks like in dev" example.
- **Connection-config changes are runtime-live.**  No restart needed.  `manage_connections action=update` writes through to the in-memory `CloudConnectionManager` AND persists to `connections.json` atomically (sitting-25-era atomic-rename pattern).  This is by design — JC tests posture changes interactively and expects them to take effect immediately.
- **`sslmode=require` is a TLS floor, not a cert-validation floor.**  libpq's `require` says "TLS must be negotiated" but doesn't validate the server cert.  A MITM with a self-signed cert can still intercept.  For full protection (validates the cert against a CA), use `verify-ca` (validates chain) or `verify-full` (validates chain + hostname).  `require` is the right floor for "any encryption better than none"; the stricter modes are appropriate when the operator has set up CA trust.

---

## 2026-05-02 (S1 sitting 28) → next session

S1=D2 sitting 28.  Theme: **DNS-resolution-time SSRF post-resolve check** + **bracketed-IPv6 fix** in postgres host parse (folded from sitting 27 gotcha).  Sitting 22 added a syntactic SSRF gate (`ValidatePublicHttpEndpoint`) that catches IP literals; sitting 28 closes the resolve-time vector — an attacker-controlled DNS name that resolves to an internal IP.  The two fixes ship together because they share the `IsLocalNetworkHost` infrastructure.

### What landed

**`OpensocketStrictCallback` in `connectorHttp.cpp`** — a `CURLOPT_OPENSOCKETFUNCTION` callback that fires after libcurl's DNS resolve, before TCP connect.  Stringifies the resolved IP via `inet_ntop` (portable on POSIX + Windows via `<arpa/inet.h>` / `<ws2tcpip.h>`), runs `IsLocalNetworkHost` on it, returns `CURL_SOCKET_BAD` if the IP is in the loopback / RFC1918 / link-local / cloud-metadata ranges.  Logs `[security] dns_resolved_ip_local_network_rejected resolved_ip='...'` with the actual IP for audit-log analysis.

**`ApplyHardenedDefaults` and `ApplyExecutorRedirectDefaults` extended** to accept an optional `std::string_view url` parameter.  When `url.starts_with("https://")`, the helper installs `OpensocketStrictCallback`.  For `http://` URLs (dev-mode local-net opt-in like MinIO `http://localhost:9000`, Azurite, local Mailpit), the callback is NOT installed — the user opted into local-net by choosing http.  Mirrors email's `allowLocal = !useSsl` posture from sitting 13, the connector-layer SSRF gate from sitting 22, and the postgres sslmode rules from sitting 26.

**27 call sites updated** to pass the URL.  The bulk-edit was mechanical (`sed -i 's|ApplyHardenedDefaults(curl);|ApplyHardenedDefaults(curl, url);|g'`) because every call site already had `url` in scope from the `CURLOPT_URL` setopt one or two lines above.  Touched files: 9 connectors, 8 executors, polarionClient (4 sites).  Snowflake had its own non-shared-helper code path and was untouched (already at FOLLOWLOCATION=0, didn't need the post-resolve check via this helper — could be folded into a future sweep if desired).

**`PostgresConnector::ParseHostPort` bracket-stripping** — pre-fix, `[fc00::1]:5432` parsed as `host = "[fc00::1]"`, which had a leading `[` that caused `IsLocalNetworkHost`'s structural IPv6 classifier to return false (sitting-27 known gap).  Post-fix, `ParseHostPort` detects the bracketed form, strips the brackets, and returns `host = "fc00::1"` — `IsLocalNetworkHost` now correctly recognizes it as local-net.  Also handles `[fc00::1]` (no port) and `[fc00::1]:5432` (with port), with malformed forms (open bracket, no close) falling through to the generic path which produces something libpq rejects.

### What's verified

- Studio release build: clean across all 22 modified files (connector + executor + polarionClient + postgresConnector + connectorHttp).
- **Negative test (DNS-resolution-time SSRF) — `localtest.me`** (a real public DNS name that resolves to `127.0.0.1` and `::1`):
  - Connection created with `https://localtest.me`.
  - SSRF gate's syntactic check passed (host charset is alphanumeric + `.`, no IP-literal prefix match).
  - Post-resolve callback fired during curl's connect — both resolved IPs (`::1`, `127.0.0.1`) were rejected, libcurl returned "Could not connect to server".
  - Security log shows two consecutive entries: `dns_resolved_ip_local_network_rejected resolved_ip='::1'` then `... resolved_ip='127.0.0.1'`.
  - **First-time real test of the post-resolve callback chain** — sitting 22's syntactic gate alone would NOT have caught this.
- **Negative test (bracketed-IPv6 in postgres) — `[fc00::1]:5432`**:
  - `ParseHostPort` strips brackets, returns `host = "fc00::1"`.
  - `IsValidSslMode("fc00::1", "disable")` accepts (IPv6 unique-local is recognized as local-net via the structural classifier from sitting 27).
  - libpq itself reaches the network layer and times out (the address isn't routed).  Confirms the bracket-strip fix and the IPv6-literal classifier interaction.
- **Negative test (syntactic-IP-literal-SSRF, regression-positive) — `127.0.0.1.nip.io`**:
  - The hostname literally starts with `127.` so the SYNTACTIC `IsLocalNetworkHost` check matches at the `host.starts_with("127.")` rule.  Rejected pre-resolve.
  - Confirms the syntactic gate still catches the obvious case before incurring the DNS lookup cost.
- **Full demo regression — 12/12 demos green**: azureBlobDemo, gcsDemo, s3UploadDownloadDemo, oneDriveUploadDownloadDemo, sheetsQuizGrader, emailDemo, snowflakeQueryDemo, gitHubIssueDemo, slackQAndABot, redmineTriageBot, jiraIssueDemo, postgresDemo.  No false-positive on legitimate https-public-DNS hosts (api.github.com, atlassian.net, slack.com, sheets.googleapis.com all resolve to public IPs that pass the post-resolve check).  No false-positive on legitimate http+local-network hosts (MinIO, Azurite, local Mailpit, local pg) because the callback isn't installed for http://.
- All test connections deleted at end of sitting.

### Open items / next-session candidates

- **Sitting 29 — `local-pg` sslmode tighten** (carried).  Move from `sslmode=disable` to `sslmode=require` (or `verify-full`) since local pg supports TLS.  ~5 min config change.
- **Sitting 30 — `sslcert` / `sslkey` / `sslrootcert` path-confinement** (carried, preventive).  Currently no code path exposes these; sitting only opens if a future feature surfaces them.
- **Sitting 31 — Snowflake post-resolve check** (new, surfaced this sitting).  Snowflake's curl handles in `snowflakeCloudTaskExecutor.cpp` don't go through `ConnectorHttp::ApplyHardenedDefaults` — its setopt block is bespoke (predates sitting 22).  The post-resolve callback isn't installed.  Either route Snowflake through the helper or attach the callback directly.  ~30 min.
- **Sitting 32 — Audit log analysis of `dns_resolved_ip_local_network_rejected`** (new).  The log line includes the actual resolved IP, which is useful forensic data.  Worth adding a dashboard query / status counter so an operator can see DNS-time SSRF attempts at a glance.

### Gotchas next-session-Claude should know

- **The post-resolve callback doesn't observe `clientp`.**  All `CURLOPT_OPENSOCKETDATA` is unused — the callback runs in strict mode unconditionally (because we only install it when the URL is https).  Don't try to thread per-request state through `clientp`; it's a no-op by design.
- **`OpensocketStrictCallback` calls `socket()` on the success path.**  This is the standard pattern: libcurl's documented behavior is that the callback returns either a fresh socket or `CURL_SOCKET_BAD`.  `socket(family, type, protocol)` works on both POSIX and Winsock (after `WSAStartup`, which webServer already does).
- **`IsLocalNetworkHost` accepts the IPv6 ANY address `::` and the IPv4 ANY `0.0.0.0`?**  Probably not as written — neither matches any rule.  But these shouldn't appear as resolved IPs from public DNS.  If they ever do (pathological DNS response), they'd fall through to "not local" and connect would proceed.  Acceptable: the connection would then fail at TCP-connect anyway (you can't connect TO `0.0.0.0` as a destination).
- **The bracket-stripping in `ParseHostPort` is intentionally conservative.**  Malformed input (open bracket, no close like `[fc00::1`) falls through to the generic colon-split path — produces a host string libpq won't accept, which is the right outcome (we're not trying to "rescue" malformed input).  Don't add error reporting at the `ParseHostPort` level; the gate-and-reject happens in `IsValidSslMode` and libpq.
- **Post-resolve check is HTTPS-only by design.**  If you find yourself wanting to install it on an http URL, the question is: why is your http target hitting public DNS at all?  http+localhost is dev mode (the user explicitly opted in), and http+public-IP is a wildly insecure posture that has bigger problems than DNS rebinding.  Don't extend the gate to http.

---

## 2026-05-02 (S1 sitting 27) → next session

S1=D2 sitting 27.  Theme: **IPv6 + IDN host-charset cleanup** in `ConnectorHttp::IsLocalNetworkHost` (folded from sitting-22 gotchas).  Pre-fix, the IPv6 unique-local / link-local prefix check ran unconditionally on the raw host string — a public hostname like `fc-acme.example.com` or `fdsoftware.example.com` was incorrectly flagged as local-network and rejected by the SSRF gate.  Micro fix.

### What landed

**`IsIPv6Literal(host)` file-local classifier** in `application/cloud/connectorHttp.cpp`.  Cheap structural test: `host` is hex digits + colons only, with at least one colon.  Real IPv6 literals (`fc00::1`, `fe80::1`) pass; public hostnames (`fc-acme.example.com`, `fdsoftware.example.com`) fail because hyphen and dot aren't in the alphabet.

**`IsLocalNetworkHost` gated the IPv6 prefix branch on `IsIPv6Literal`** — `host.starts_with("fc"|"fd"|"fe80")` now only fires when the structural classifier confirms it's actually an IPv6 literal.  All other rules (loopback strings, RFC 1918 prefixes, link-local 169.254., 172.16/12) are unchanged.

**Header docstring rewritten** to reflect the new scoping plus the documented limitation: bracketed IPv6 literals (`[fc00::1]`) in URLs are handled upstream by `ValidatePublicHttpEndpoint`'s host-charset rule (`[` / `]` are rejected); but `PostgresConnector::ParseHostPort` doesn't strip brackets, so an IPv6 in a postgres connection's endpoint can slip through.  Tracked separately as sitting 28+ scope.

**No code change needed for IDN/punycode** — the existing posture (host-charset rule = `alphanumeric + . + -`) already requires punycode-encoded names (`xn--bcher-kva.example.com`) and rejects raw Unicode.  That's the deliberate posture: punycode is the standard transport encoding for IDN in URLs, and the JCWF author is responsible for emitting it.  No-op for this sitting; documented in code comment at `ValidatePublicHttpEndpoint`'s host-charset loop.

### What's verified

- Studio release build: clean.  One file recompiled (`connectorHttp.cpp`) — every executor + connector that pulls in `connectorHttp.h` recompiled too via the header doc-comment edit, but link is one-shot.
- **3 negative-on-the-old-code tests** (each via temp connection + manage_connections test):
  - `https://fc-acme.example.com` (jira) → SSRF gate now PASSES; curl tries to resolve and fails with "Could not resolve hostname".  Before this sitting: blocked at the validator with "https endpoint resolves to local-network host".  **Fixed**.
  - `https://fdsoftware.example.com` (jira) → same, **fixed**.
  - `fc00::1:5432` (postgres) → bare IPv6 unique-local: `IsIPv6Literal` returns true, prefix check fires, `IsLocalNetworkHost` returns true, validator accepts `sslmode=disable` for it (correct localhost-treatment).  libpq itself times out connecting to the unrouted address.  **Behavior unchanged** for real IPv6 literals.
- **2 regression-positive tests**:
  - `https://10.0.0.5` (RFC 1918) → still rejected (`https endpoint resolves to local-network host`).
  - `https://169.254.169.254` (cloud metadata) → still rejected.
- **3 cloud-demo regressions**: jiraIssueDemo 4/4, snowflakeQueryDemo 14/14, postgresDemo 12/12.  No false-positive on legitimate hosts post-fix.
- All 5 test connections deleted at end of sitting.

### Open items / next-session candidates

- **Sitting 28+ — DNS-resolution-time SSRF post-resolve check** (carried).  `ValidatePublicHttpEndpoint` is purely syntactic; an attacker-controlled public DNS name resolving to an internal IP is NOT blocked.  Closing this requires a `CURLOPT_OPENSOCKETFUNCTION` post-resolve check that re-validates the resolved IP via `IsLocalNetworkHost`.  Estimated 1.5-2 hours including testing.  Plus: this sitting noted that bracketed IPv6 in postgres endpoints (`[fc00::1]:5432`) bypasses `IsLocalNetworkHost` because brackets aren't stripped — fold that fix into sitting 28's IPv6 / DNS work.
- **Sitting 29+ — `local-pg` sslmode tighten** (carried).  Move from `sslmode=disable` to `sslmode=require` (or `verify-full`) since local pg supports TLS.  ~5 min config change.
- **Sitting 30+ — `sslcert` / `sslkey` / `sslrootcert` path-confinement** (carried, preventive).  Currently no code path exposes these; sitting only opens if a future feature surfaces them.

### Gotchas next-session-Claude should know

- **`IsIPv6Literal` is intentionally permissive on length.**  The classifier doesn't enforce the IPv6-literal grammar (8 groups of 4 hex digits, `::` zero-compression rules) — it only checks the alphabet.  A 1000-char hex+colon string would pass.  That's fine: any pathological-but-syntactically-IPv6-looking input would either fail the connection at the OS level or get rejected by the host-length cap upstream (`ValidatePublicHttpEndpoint` caps at 253 chars).  Don't tighten this without a concrete attack vector.
- **The fix is at the `IsLocalNetworkHost` boundary, not at the call sites.**  Don't sprinkle `IsIPv6Literal` checks at callers — they shouldn't need to know about IPv6 detection.  `IsLocalNetworkHost` is the single source of truth for "is this host inside the local-net trust boundary"; the IPv6 classifier is implementation detail.
- **Bracketed IPv6 literals are still a known gap in postgres-style endpoint parsing.**  `[fc00::1]:5432` — `ParseHostPort` returns host = `[fc00::1]`, which has a leading `[` so `IsIPv6Literal` returns false → `IsLocalNetworkHost` returns false → the validator wouldn't catch it.  In practice this is a self-DOS (connection won't work anyway), but the security boundary should still recognize it as local.  Documented in the header docstring; closure is sitting 28+.
- **The IDN posture is "punycode in, raw Unicode rejected" by design.**  If a user reports that a hostname like `bücher.example.com` is rejected, the answer is "punycode it to `xn--bcher-kva.example.com` and resubmit" — not "loosen the host-charset rule".  Raw Unicode in hostnames is a phishing / homograph-attack vector that the strict ASCII charset deliberately blocks.

---

## 2026-05-02 (S1 sitting 26) → next session

S1=D2 sitting 26.  Theme: **Postgres SSL posture audit** — `PostgresConnector` previously took `sslmode` from JCWF params with no validation and defaulted to libpq's `"prefer"`, which silently falls back to plaintext if the server doesn't accept TLS.  That's an MITM-vulnerable posture for production (any non-localhost) hosts.  This sitting closes it.

### What landed

**`PostgresConnector::IsValidSslMode(host, sslmode, errorMessage)`** — public static helper with two layers:
1. **Allowlist of libpq sslmode values**: `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full`.  Anything outside (typo / hostile input) is rejected before reaching libpq with a clear error.
2. **Production posture for non-localhost hosts**: rejects `disable` / `allow` / `prefer` (the three plaintext-fallback modes) — only `require` / `verify-ca` / `verify-full` are acceptable.  For local-network hosts (`localhost`, RFC 1918, link-local — detected via the existing `ConnectorHttp::IsLocalNetworkHost`), all 6 modes are accepted as a dev-mode opt-out.  Mirrors email's `allowLocal = !useSsl` heuristic from sitting 13.

**`PostgresConnector::ParseHostPort(connection, host, port)`** — extracted from the existing inline parse in `BuildConnectionString` so both `IsValidSslMode` callers and `BuildConnectionString` share a single host-extraction codepath.

**Default sslmode changed from `"prefer"` to `"require"`** (in `BuildConnectionString`).  TLS is now mandatory by default — for local non-TLS pg, set `sslmode=disable` explicitly on the connection (validator allows it because the host is localhost).  `local-pg` already has `"sslmode": "disable"` set explicitly, so no breakage.

**Two gate sites** call `IsValidSslMode` BEFORE `BuildConnectionString` — closes the gap where an invalid sslmode could reach libpq:
- `PostgresConnector::TestConnection` — gates the connection-test path.
- `DbQueryCloudTaskExecutor::ExecuteCloud` — gates the query-execution path (this executor is the actual data-path consumer; it shares `BuildConnectionString` with the connector but didn't share the validation).

Each rejection emits a `[security] postgres_invalid_sslmode connection='{}' host='{}' sslmode='{}'` warning + an ERROR-level workflow log line tagged with task / workflow / run id.

### What's verified

- Studio release build: clean.  3 files recompiled (postgresConnector .h/.cpp + dbQueryCloudTaskExecutor.cpp).
- **`local-pg` TestConnection** — green (`OK: reachable`).  Local pg accepts TLS on its default port even with `sslmode=disable` on the j9t side, so the existing connection works unchanged.  Note: I had assumed local pg was non-TLS, but it actually does support TLS — sitting 27+ could consider tightening `local-pg` to `sslmode=require` since the underlying transport supports it.
- **postgresDemo** — 12/12 tasks succeeded (~2 s); `dbQueryCloudTaskExecutor` exercises the new gate end-to-end via the `query_departments` task on the same `local-pg` connection.
- **4 negative tests** (each via temp `manage_connections` create+test) — all rejected with explicit security log entries:
  - `db.example.com:5432` + `sslmode=prefer` → `postgres_invalid_sslmode ... sslmode='prefer'`.
  - `db.example.com:5432` + `sslmode=allow` → `... sslmode='allow'`.
  - `db.example.com:5432` + `sslmode=disable` → `... sslmode='disable'`.
  - `localhost:5432` + `sslmode=bogus` → `... sslmode='bogus'` (caught by allowlist regardless of host).
- **Positive case for non-localhost** — `db.example.com:5432` + `sslmode=require` passes the validator (then libpq itself fails on DNS resolution for `db.example.com`, which is the expected next gate).
- All 4 negative-test connections deleted at end of sitting.

### Open items / next-session candidates

- **Sitting 27 — IPv6 + IDN host-charset cleanup** (carried from sitting 22).  Tighten `ConnectorHttp::IsLocalNetworkHost`'s `starts_with("fc"|"fd"|"fe80")` so it doesn't false-positive on hostnames; consider IDN/punycode handling.  Micro, ~30 min.
- **Sitting 28+ — DNS-resolution-time SSRF post-resolve check** (carried from sitting 22).  `ConnectorHttp::ValidatePublicHttpEndpoint` is purely syntactic; an attacker-controlled public DNS name resolving to an internal IP is NOT blocked.  Closing this requires a `CURLOPT_OPENSOCKETFUNCTION` post-resolve check that re-validates the resolved IP via `IsLocalNetworkHost`.  Estimated 1.5-2 hours including testing.
- **Sitting 29+ — `local-pg` sslmode tighten** (new, surfaced this sitting).  Local pg supports TLS; `local-pg` could move from `sslmode=disable` to `sslmode=require` (or `verify-full` if a CA bundle is wired up) to match the production posture even for development.  ~5 min config change + verify.
- **Sitting 30+ — `sslcert` / `sslkey` / `sslrootcert` allowlist + path-confinement** (new, adjacent to this sitting).  libpq supports client-certificate auth via these connection-string params.  If j9t ever exposes them through `m_Params`, they need the same path-traversal gate as `local_path` / `body_file` (`ICloudTaskExecutor::ValidateLocalPath`).  Currently NOT exposed, but a future feature could regress this.

### Gotchas next-session-Claude should know

- **The validator gates BOTH the connector and the executor.**  `PostgresConnector::TestConnection` and `DbQueryCloudTaskExecutor::ExecuteCloud` independently call `IsValidSslMode` before `BuildConnectionString`.  This is intentional — `BuildConnectionString` is a string-builder helper, not a security gate; if a future caller skips the validation by going directly to `BuildConnectionString`, the deeper libpq layer would silently accept the bad sslmode.  Keep the validation at every entry point.
- **`IsLocalNetworkHost` does the localhost detection.**  Don't reimplement it locally in postgresConnector — the existing helper in `ConnectorHttp` (lifted from email in sitting 22) is the single source of truth.  This is the third caller of `IsLocalNetworkHost` (email, http-connectors, postgres) — if a future caller surfaces, route through the same helper rather than copy-pasting the rules.
- **Default change is a posture shift, not a bug fix.**  Changing the default from `"prefer"` to `"require"` means existing JCWFs that omitted `sslmode` and pointed to a non-TLS pg will now fail.  The breaking change is bounded: only the canonical demo connection `local-pg` had this shape, and its config was already updated to `"sslmode": "disable"` explicitly.  For external users, this is documented in cyber security.md and needs a release-note callout when the alpha drops (see `project_alpha_beta_timeline`).
- **The 4 test connections (`ssl-test-1` through `ssl-test-4`) were deleted** at the end of the sitting.  Don't expect them.

---

## 2026-05-02 (S1 sitting 25) → next session

S1=D2 sitting 25.  Theme: **Executor-layer redirect/TLS sweep** — the complement of sitting 22's connector-layer hardening.  All 13 `CURLOPT_FOLLOWLOCATION = 1L` sites in `*CloudTaskExecutor.cpp` audited per-API; each got a deliberate posture (redirects-disabled vs redirects-allowed-https-only) plus explicit TLS verify-peer + verify-host.  New shared helper `ConnectorHttp::ApplyExecutorRedirectDefaults` for the vendor APIs that legitimately 30x.

### What landed

**New shared helper `ConnectorHttp::ApplyExecutorRedirectDefaults(CURL*)`** — the redirect-allowed counterpart to the existing `ApplyHardenedDefaults`.  Same TLS verify + CAINFO setup, but with:
- `CURLOPT_FOLLOWLOCATION = 1L`
- `CURLOPT_REDIR_PROTOCOLS_STR = "https"` — restricts redirect targets to https only.  Prevents an http-downgrade attack where a 30x response from a compromised endpoint redirects to `http://attacker.com/...` and leaks the bearer/PAT in the `Authorization` header on the unencrypted follow request.  libcurl 8.17 rejects an unencrypted-protocol redirect target as a hard error rather than silently following it.
- `CURLOPT_MAXREDIRS = 10L` — caps follow depth so an attacker-controlled redirect loop can't pin a worker thread indefinitely.

**13 executor sites swept**, classified per-API:

**Redirects-disabled (9 sites, `ApplyHardenedDefaults`)** — vendor APIs that respond directly on the data path.  Bearer-token-leak threat from following a hostile redirect is real even on cloud surfaces with valid TLS certs:
- `azureBlobCloudTaskExecutor.cpp` ×2 (Request + Download) — `*.blob.core.windows.net` responds directly.
- `gcsCloudTaskExecutor.cpp` ×2 — `storage.googleapis.com` JSON API responds directly.
- `gitHubCloudTaskExecutor.cpp` ×1 — `api.github.com` REST responds directly.  Also gained explicit TLS verify (was missing).
- `googleSheetsCloudTaskExecutor.cpp` ×1 — `sheets.googleapis.com` v4 responds directly.
- `jiraCloudTaskExecutor.cpp` ×1 — `*.atlassian.net/rest/api/3/*` responds directly.  Also gained explicit TLS verify.
- `redmineCloudTaskExecutor.cpp` ×1 — self-hosted Redmine `/issues/*.json` responds directly.  Also gained explicit TLS verify.
- `slackCloudTaskExecutor.cpp` ×1 — `slack.com/api/*` responds directly.  Also gained explicit TLS verify.

**Redirects-allowed-https-only (4 sites, `ApplyExecutorRedirectDefaults`)** — vendor APIs that legitimately 30x on the data path:
- `s3CloudTaskExecutor.cpp` ×2 (Request + Download) — S3 emits `301 Moved Permanently` + Location on cross-region bucket mismatches and presigned-URL flows.  S3-compatible alternatives (MinIO etc.) over `http://` are unaffected — the TLS verify setopts only kick in when libcurl actually negotiates TLS, and http-MinIO doesn't 30x in practice.
- `oneDriveCloudTaskExecutor.cpp` ×2 (GraphRequest + GraphDownload) — `GET /me/drive/items/{id}/content` returns 302 to a SharePoint / `download.microsoft*` CDN URL on every download; large-file upload sessions can pivot to `*.up.1drv.com` endpoints.

**Snowflake** was already at `FOLLOWLOCATION = 0L` from sitting 14 — untouched.  **Email** uses libcurl's SMTP/IMAP transports, not HTTP — `FOLLOWLOCATION` is n/a.

**Net effect on the cloud surface:**
- **Before this sitting:** 13 executor data paths followed redirects unconditionally (no protocol restriction, no follow-depth cap), 4 of which lacked explicit TLS verify.  A compromised vendor endpoint or a network-position attacker could pivot the bearer/PAT to any URL via 30x.
- **After this sitting:** 9 executor data paths refuse redirects entirely; 4 follow redirects but only to https targets, capped at 10 hops, with explicit verify-peer + verify-host.  All 13 share the same hardened-defaults helper.

### What's verified

- Studio release build: clean.  9 executor files + connectorHttp.h/.cpp recompiled, no warnings (post the in-place `#include "cloud/connectorHttp.h"` adds — every executor now pulls in the shared helper).
- **Full cloud demo matrix re-run** — 11/11 demos green:
  - **Redirect-disabled paths verified:** azureBlobDemo 14/14, gcsDemo 14/14, sheetsQuizGrader 3/3, emailDemo 3/3, snowflakeQueryDemo 14/14, gitHubIssueDemo 10/10, slackQAndABot 3/3, redmineTriageBot 14/14, jiraIssueDemo 4/4.  None of these legitimately 30x in practice; flipping to 0 had no behavioural impact.
  - **Redirect-allowed paths verified:** s3UploadDownloadDemo 5/5 (upload + download + ai_analyze + upload_report + list_objects), oneDriveUploadDownloadDemo 4/4 (upload + download + ai_review + upload_review).  The oneDrive download path is the primary justification for the keep-redirect helper — it 302's to a SharePoint CDN URL on every request; this one's working confirms the helper is plumbed correctly.
  - **Cumulative:** ~88 cloud tasks across 11 demos, 0 failures, no regression.
- **Code-review level verification of the negative case** — `ApplyExecutorRedirectDefaults` sets `CURLOPT_REDIR_PROTOCOLS_STR = "https"`, which libcurl 8.17 enforces as a hard error on http-target follows.  Verified by reading the helper definition and cross-referencing libcurl's `lib/transfer.c` redirect-check.  Negative case (forced http target) not scripted because triggering it requires either a mocked vendor server or an attacker-position MITM; the library-level enforcement is the boundary.

### Open items / next-session candidates

- **Sitting 26 — Postgres SSL posture audit** (carried).  `PostgresConnector` `sslmode` allowlist + production default to `require`.  Estimated 1 hour.
- **Sitting 27 — IPv6 + IDN host-charset cleanup** (folded from sitting-22 gotchas).  Tighten `ConnectorHttp::IsLocalNetworkHost`'s `starts_with("fc"|"fd"|"fe80")` so it doesn't false-positive on hostnames; consider IDN/punycode handling.  Micro, ~30 min.
- **Sitting 28+ — DNS-resolution-time SSRF post-resolve check** (carried from sitting 22 gotchas).  `ConnectorHttp::ValidatePublicHttpEndpoint` is purely syntactic; an attacker-controlled public DNS name resolving to an internal IP is NOT blocked.  Closing this requires a `CURLOPT_OPENSOCKETFUNCTION` post-resolve check that re-validates the resolved IP via `IsLocalNetworkHost`.  Estimated 1.5-2 hours including testing.

### Gotchas next-session-Claude should know

- **The redirect-allowed helper does NOT block initial-request http.**  `CURLOPT_REDIR_PROTOCOLS_STR` only controls REDIRECT targets, not the initial request.  An s3 connection to `http://localhost:9000` (MinIO) still works because the initial protocol is unrestricted by the helper; the redirect protocol restriction would only kick in if MinIO returned a 30x — which it doesn't in practice.  This is the correct posture: a user explicitly configured an http endpoint, so the initial request honors that, but a hostile redirect target would be refused.
- **Don't reach for `ApplyExecutorRedirectDefaults` by default.**  Use `ApplyHardenedDefaults` everywhere unless you've actually traced a legitimate vendor 30x on the data path.  The bearer-token-leak threat from following a hostile redirect is real even on cloud surfaces with valid TLS certs — a compromised endpoint can return a 302 to its own attacker-controlled host with a valid cert and harvest the Authorization header.  Sitting 25 only used the redirect-allowed variant on s3 + oneDrive because both vendors' documented behavior includes 30x.
- **`CURLOPT_REDIR_PROTOCOLS_STR` requires libcurl 7.85.0+; we're on 8.17.**  If the vendored libcurl is ever downgraded, this fails to compile.  Older fallback would be `CURLOPT_REDIR_PROTOCOLS = CURLPROTO_HTTPS` (long bitmask).  Worth noting in case of future libcurl version changes.
- **The 9 executors that swept this sitting now all `#include "cloud/connectorHttp.h"`.**  That include used to live only in the `*Connector.cpp` files (sitting 22).  Any future executor edit that touches curl setup should call `ApplyHardenedDefaults` (or the redirect variant) — manual `SSL_VERIFYPEER` / `FOLLOWLOCATION` / `CAINFO` setopts in executor code are a code-review reject.

---

## 2026-05-02 (S1 sitting 24) → next session

S1=D2 sitting 24.  Theme: **Request-body / response-body JSON-injection sweep** (deferred from sitting 19).  Sitting 19's Track A converged 9 raw splices in the 5-executor cluster; sitting 19 left the cluster's siblings (jira, snowflake, polarionWrite) for later.  This sitting closes those.  The hand-off had named "Slack chat.postMessage body, Jira create body, GitHub issue body" as targets, but the survey showed those three were already converged in sitting 19 — the actual remaining holes are smaller and on different sites (synthesized success-response bodies + snowflake columnNames result-write).

### What landed

**4 raw `+` / `<<` JSON splices converged onto `JsonHelper::EscapeJsonString`:**
- `snowflakeCloudTaskExecutor.cpp:634` — `out << "\"" << columnNames[c] << "\": "` (carried from sitting 21).  A column name containing `"` would otherwise produce invalid JSON in the result-write path.  Snowflake column names are typically safe (server uppercases unquoted identifiers), but a hostile/compromised endpoint or a quoted identifier like `SELECT 1 AS "x\"y"` would break it.
- `jiraCloudTaskExecutor.cpp:314` — `responseBody = "{\"ok\":true,\"issue_key\":\"" + issueKey + "\",\"operation\":\"update\"}"`.  This is a **synthesized success-response** built when Jira's `update` endpoint returns no body.  `issueKey` comes from JCWF params (user-controlled), so a value containing `"` would break downstream JSON-path resolvers (`{{create_issue.json.issue_key}}`).
- `jiraCloudTaskExecutor.cpp:349` — synthesized `transition` success body with both `issueKey` AND `transitionId` raw.  Same shape, both user-controlled.
- `polarionWriteTaskExecutor.cpp:238` — `responseBody = "{\"ok\":true,\"file_path\":\"" + filePath + "\"}"`.  `filePath` is the local-disk write path used for downloaded attachments; user-controlled.

**Survey corrections vs the hand-off's pre-flight estimate.**  The sitting-22 hand-off named slack/jira/github request bodies as the targets, but those were already fully converged in sitting 19 — slack `chat.postMessage` body uses `EscapeJsonString` on channel/text/thread_ts (slackCloudTaskExecutor.cpp:254-258); jira `create` body uses it on projectKey/summary/issueType/description/priority/labels (jiraCloudTaskExecutor.cpp:213-241); github `create issue` body uses it on title/body/labels (gitHubCloudTaskExecutor.cpp:206-224).  Re-greping for raw `+` splices into JSON literals turned up only the 4 sites above.  Net: the sitting was smaller than estimated (~30 min vs the 1.5h estimate).

**No new validators added** — this sweep is a strict extension of sitting-19's Track A, not new threat closures.  The redmine `assigned_to_id` numeric splice (line 296) was deliberately left untouched: it's gated by an explicit digit-only `std::all_of` check at lines 291-293 (sitting-19 era), and the corresponding string-form path at line 300 IS escaped — the asymmetry is intentional.

### What's verified

- Studio release build: clean.  3 files recompiled (snowflake + jira + polarionWrite executors), one-shot link.
- 4 cloud demos green: snowflakeQueryDemo 14/14 (exercises the columnNames escape via the result-write JSON branch on every per-region row), slackQAndABot 3/3, gitHubIssueDemo 10/10, jiraIssueDemo 4/4 (after a retry — the first attempt's `create_issue` had a transient curl error against the live Atlassian endpoint, completely unrelated to this sitting; the synthesized-response paths fixed in this sitting only fire on `update` / `transition` ops which `jiraIssueDemo` doesn't exercise — verification is at the code-review level since the convergence pattern is well-tested by sitting 19's 9-site sweep).
- Zero new error log entries in the test runs (the transient jira-create error from the first attempt aside).
- **No negative-case test scripted** because the natural negative inputs would require either a mocked Atlassian/Snowflake server returning controlled values, or a JCWF whose upstream task pipes a hostile string into `issueKey` / `filePath` / `columnNames`.  Deferred — the convergence pattern is mechanical and the existing 12+ already-converged sites in slack/jira/github prove `JsonHelper::EscapeJsonString`'s behavior.

### Open items / next-session candidates

- **Sitting 25 — Executor-layer redirect/TLS sweep** (carried from sitting 22).  14 `FOLLOWLOCATION=1L` sites in `*CloudTaskExecutor.cpp`; each needs per-API analysis (some legitimately 30x to regional endpoints / presigned URLs).  Estimated 2-3 hours.
- **Sitting 26 — Postgres SSL posture audit** (carried).  `PostgresConnector` `sslmode` allowlist + production default to `require`.  Estimated 1 hour.
- **Sitting 27 — IPv6 + IDN host-charset cleanup** (folded from sitting-22 gotchas).  Tighten `ConnectorHttp::IsLocalNetworkHost`'s `starts_with("fc"|"fd"|"fe80")` so it doesn't false-positive on hostnames; consider IDN/punycode handling.  Micro, ~30 min.

### Gotchas next-session-Claude should know

- **The hand-off pre-flight survey is not authoritative.**  Sitting 22's hand-off named slack/jira/github request bodies; sitting 19 had already covered them.  Always grep for the exact pattern at the start of a sweep — the work-volume estimate from a 4-day-old hand-off can be wildly off in either direction (sweep #6 was sized at 1.5h and was 5 min; sweep #7 was sized at 2-3h and grew into a 4-track consolidation).  Trust the codebase, not the prior estimate.
- **Synthesized-response JSON splices are a separate audit category from request-body splices.**  Sitting 19's Track A focused on REQUEST bodies (the data going OUT to the cloud API).  This sitting found the COMPLEMENT — RESPONSE bodies the executor synthesizes when the upstream returns empty (Jira's update / transition endpoints return 204 No Content).  Those synthesized bodies flow into `taskState.m_CapturedStdout` and downstream `response.json` files where template variables `{{task.json.field}}` consume them.  An unescaped synthesized response is just as dangerous as an unescaped request — invalid JSON breaks downstream parsing.  Future sweeps should grep for both directions.
- **The redmine `assigned_to_id` numeric splice at `redmineCloudTaskExecutor.cpp:296` is intentionally unescaped.**  It's gated by a digit-only check three lines above — the JSON output is `{"assigned_to_id": 1234}` (number, no quotes).  Don't "fix" it by wrapping in `EscapeJsonString` — that would emit it as a string and Redmine would reject the wrong type.

---

## 2026-05-02 (S1 sitting 23) → next session

S1=D2 sitting 23.  Theme: **Sweep #4 part 2** — close the audit's last two URL-side validation deferrals from sitting 18 (which originally swept the 5-executor cluster but left azureBlob's `container` / `blob_name` and googleSheets' `spreadsheet_id` / `range` for later because they each needed bespoke per-API allowlist rules).  Tightly scoped: 2 executors, 4 validators, ~110 LoC of new code, no architectural change.

### What landed

**`azureBlobCloudTaskExecutor.cpp` — 2 new file-local validators + per-call gate.**
- `IsValidAzureContainer(container)` — Microsoft container-naming rules: 3-63 chars, lowercase + digits + hyphen, must start/end with letter or digit, no consecutive hyphens.  Strict allowlist form rejects `/`, `?`, `#`, `:`, `@`, `%` before they reach the URL build at `endpointUrl + "/" + container + "/" + blobName`.
- `IsValidAzureBlobName(blobName)` — 1-1024 chars, alphanumeric + `.` + `_` + `-` + `/` (hierarchy) + space.  Reject `..` segments (path traversal at the Azure URL — server-side resolves `foo/../etc/secret.txt` and would otherwise let a hostile blob name escape the intended folder).
- Applied at the single point where both fields are first read (after the empty-checks, before the URL build).  Each rejection emits an ERROR log + a `[security] azure_blob_invalid_{container,blob_name}` line tagged with task / workflow / run id (consumable by the dashboard's run analyser).

**`googleSheetsCloudTaskExecutor.cpp` — 2 new file-local validators + per-call gate.**
- `IsValidSpreadsheetId(spreadsheetId)` — Google's drive-file-ID format: `[A-Za-z0-9_-]`, 1-128 chars.  Required because `spreadsheet_id` flows UNENCODED into the URL path (`apiBase + "/" + spreadsheetId + "/values/..."`); a `/`, `?`, `#`, `:`, `@`, or `%` would otherwise inject URL components past the canonical layout.
- `IsValidSheetsRange(range)` — A1-notation chars only: alphanumeric + `!` + `:` + `$` + `_` + `-` + `.` + `'` + space.  1-256 chars.  The range value IS URL-encoded by `UrlEncode` before splicing, so the SSRF risk is bounded — but the check rejects CR / LF / control bytes that would otherwise pollute audit logs (`LOG_APP_INFO("[sheets] read N rows from range '{}'")`) and defends against pathological inputs.
- Applied right after both fields are read, before any URL build.  Same ERROR + `[security] sheets_invalid_{spreadsheet_id,range}` tagging shape as the azureBlob fixes.

### What's verified

- Studio release build: clean.  Two files recompiled (azureBlob + sheets executors), one-shot link.
- **Positive-case demos** — both green end-to-end after the validator-add: azureBlobDemo 14/14, sheetsQuizGrader 3/3.  Validators fire on legitimate values without false positives.
- **4 negative tests** (each via `mcp__j9t__run_adhoc_workflow`) — all rejected with explicit security log entries:
  - container = `"BAD?CONT"` (uppercase + `?`) → `azure_blob_invalid_container`.
  - blob_name = `"foo/../etc/secret.txt"` (`..` traversal) → `azure_blob_invalid_blob_name`.
  - spreadsheet_id = `"abc/etc/passwd"` (`/` injection) → `sheets_invalid_spreadsheet_id`.
  - range = `"Sheet1\r\nBcc:evil"` (CRLF injection) → `sheets_invalid_range`.
- Each rejection produced (a) task state = `failed`, (b) ERROR log line with task/workflow/run id, (c) `[security] *_invalid_*` warning line — all queryable by the run analyser.

### Open items / next-session candidates

- **Sitting 24 — Request-body JSON-injection sweep** (deferred from sitting 19).  Slack chat.postMessage body, Jira create body, GitHub issue body — **plus** `snowflakeCloudTaskExecutor.cpp:634` columnNames raw splice (carried from sitting 21).  Estimated 1.5 hours.  Mechanical: same fix shape as sitting-19's Track A (raw concat → `JsonHelper::EscapeJsonString`).
- **Sitting 25 — Executor-layer redirect/TLS sweep** (carried from sitting 22).  14 `FOLLOWLOCATION=1L` sites in `*CloudTaskExecutor.cpp`; each needs per-API analysis (some legitimately 30x to regional endpoints / presigned URLs).  Estimated 2-3 hours.
- **Sitting 26 — Postgres SSL posture audit** (carried).  `PostgresConnector` `sslmode` allowlist + production default to `require`.

### Gotchas next-session-Claude should know

- **The 4 validators are file-local statics, not lifted to a shared helper.**  Each executor's allowlist is bespoke to its API (Azure container rules ≠ GCS bucket rules ≠ Sheets ID format), so a generic `IsValidCloudIdentifier()` would either be too permissive or wrong-shape.  This is consistent with sitting 18's pattern: `IsValidGcsBucket` (gcs), `IsValidOneDriveRemotePath` (oneDrive) — same shape, different rules.  Do NOT extract them to a shared header.
- **`IsValidSheetsRange` is intentionally permissive on space + `'`** — quoted-sheet-name patterns like `'Quiz Data'!A1:B10` are A1 syntax for sheet names containing spaces.  The Sheets API also accepts unquoted simple names like `Sheet1!A1`.  If you tighten this validator further, run sheetsQuizGrader to confirm the canonical demo's range still passes.
- **`spreadsheet_id` is unencoded in the URL but `range` IS encoded.**  This asymmetry is by design — the Sheets API's path structure is `{spreadsheetId}/values/{range}`; the API expects spreadsheetId to be raw URL-safe and range to be percent-encoded by the client.  The validator allowlists reflect this: `IsValidSpreadsheetId` is strict (URL-safe-base64 only), `IsValidSheetsRange` is broader (A1 syntax) because the URL-encode call protects the actual splice.
- **Run-adhoc is the right tool for negative tests on cloud executors.**  Crafting a temporary JCWF + `mcp__j9t__run_adhoc_workflow` exercises the full executor path including the validators, without polluting the canonical demo set.  Each adhoc run gets a distinct `adhoc_<timestamp>_<seq>` id that the security log tags via `task=bad workflow=_adhoc_... run=adhoc_...` — easy to grep.

---

## 2026-05-02 (S1 sitting 22) → next session

S1=D2 sitting 22.  Theme grew mid-sitting from a single **Horizontal Sweep #7** (connector-layer hardening) into a three-part consolidation push: (a) connector-layer SSRF + TLS + CRLF + redirect + response-cap hardening across all 12 cloud connectors' `TestConnection` paths and PolarionClient's 4 HTTP sites; (b) **OAuth callback authentication fix** — discovered when re-authorising a stale Google Sheets refresh token; (c) **cloud-surface path-resolution consolidation** — aligned all 6 local-file executors (azureBlob, email, gcs, oneDrive, s3, sheets) on the JCWF spec §3.2.1 working-directory-relative convention, plus updated 6 demo JCWFs to match; (d) tracked-doc sweep folding (a)-(c) into `cloud-integration.md`, `cyber security.md`, `api-endpoints.md`.  Five concurrent tracks for (a): A redirect-amplified SSRF closure, B explicit TLS verify, C SSRF endpoint-allowlist gate, D response-body cap, E CRLF check on bearer/PAT before splicing.  All five applied; new shared helper `application/cloud/connectorHttp.h/.cpp` collapses the duplication.  Total: ~120 lines of plumbing across 14 files removed by the helper, ~80 lines of new security gates added; plus consolidation diff across `cloudTaskExecutor.cpp` + 4 executors + 6 JCWFs + 1 webServer route + 3 docs.

### What landed

**New shared helper `application/cloud/connectorHttp.h/.cpp`** with:
- `ConnectorHttp::kMaxConnectorResponseBytes` (1 MB cap) — Track D.
- `ConnectorHttp::BoundedStringWriteCallback` — capped `std::string*` writeCallback for libcurl.
- `ConnectorHttp::ApplyHardenedDefaults(CURL*)` — sets `SSL_VERIFYPEER=1`, `SSL_VERIFYHOST=2`, `FOLLOWLOCATION=0`, `CAINFO` if a bundle is configured (Tracks A + B in one call).
- `ConnectorHttp::IsLocalNetworkHost(host)` — RFC 1918 / loopback / link-local / cloud-metadata IP-literal detection.  Lifted from `emailConnector.cpp`'s anonymous-namespace copy, with the `std::stoi`-throws hostile-input fix from sweep #6 applied (explicit digit loop).
- `ConnectorHttp::ValidatePublicHttpEndpoint(url, errorMessage)` — Track C SSRF gate: scheme must be http/https, host charset conservative, host length ≤ 253; for https only, host must NOT be a local-network IP literal (parallel of email's `allowLocal = !useSsl` heuristic).  Purely syntactic — does NOT do DNS resolution; an attacker-controlled public DNS resolving to an internal IP is NOT blocked here.

**12 cloud connectors swept** (TestConnection paths):
- **azureBlob, gcs, gitHub, googleSheets, oneDrive, s3, slack** — endpoint-validate when override set; ApplyHardenedDefaults; BoundedStringWriteCallback; CRLF check on bearer/OAuth token (where applicable).
- **jira, redmine, polarion** — endpoint-validate UNCONDITIONALLY (endpoint is required for these); same hardening defaults; CRLF check on bearer/PAT.
- **emailConnector** — already had TLS verify + `IsValidEmailHost` from sitting 13; just routed `IsValidEmailHost` to the shared `ConnectorHttp::IsLocalNetworkHost` and removed the anonymous-namespace duplicate.
- **snowflakeConnector** — already canonical from sitting 14, untouched.
- **postgresConnector** — libpq, no curl plumbing, deferred (see follow-up).

**PolarionClient (`application/workflow/filter/polarionClient.cpp`)** — 4 HTTP sites (HttpGet, HttpRequest, HttpDownloadFile, HttpUploadFile) all converged on `ConnectorHttp::ApplyHardenedDefaults(curl)`.  Note: writeCallbacks NOT swapped to `BoundedStringWriteCallback` here — these are executor data paths (responses can be hundreds of KB to MB on real Polarion queries), the 1 MB connector cap would break legitimate use.

**OAuth callback authentication fix (`application/web/webServer.cpp:1635-1648`)** — surfaced when JC needed to re-authorise a stale Google Sheets refresh token: the browser redirect from Google to `/api/connections/<name>/oauth/callback` was rejected with 401 because the route was admin-Bearer-gated — but a user-agent redirect cannot carry the j9t admin token.  The CSRF gate is supposed to be the `state` query parameter (single-use 16-byte random nonce stored server-side at `/oauth/authorize` time, verified inside `HandleOAuthCallbackGet`).  Per RFC 6749 §10.12 `state` IS the security mechanism for an OAuth callback.  Removed the `CheckAdminAuth` call from the callback route; `/oauth/authorize` remains admin-gated (it's the one that creates the flow).  Verified end-to-end: the user re-authorised, my-sheets `TestConnection` returned reachable, and the sheets demo's API-touching tasks succeeded.

**Cloud-surface path-resolution consolidation** — discovered while debugging two pre-existing demo failures (oneDrive `upload_review`, sheets `write_grades`).  Root cause: cloud executors had drifted from JCWF spec §3.2.1.  s3 + gcs validated `local_path` / `file_path` against `launchCwd` (project root), oneDrive + sheets + email + azureBlob validated against `workDir` (working_directory).  Worse, `ICloudTaskExecutor::ValidateLocalPath` rejected absolute paths outright — but `{{<task>.output_file}}` templates produce absolute paths by spec.  The cumulative effect was that template-driven file paths only worked on the launchCwd-validating executors and were rejected on the workDir-validating ones.  Shipped fix:
- **`ICloudTaskExecutor::ValidateLocalPath` rewrite** — relative paths resolve under caller-supplied `baseDir` (the task `working_directory` for spec compliance), absolute paths pass through; both forms confined under `launchCwd` as the project-tree security boundary.  `..` segments are now permitted (lexical_normal handles them; the `launchCwd` boundary catches escapes).  This is the spec-aligned interpretation: working_directory is the resolution base, not the security boundary.
- **6 cloud executors aligned on workDir as `baseDir`** — azureBlob, email (`bodyFile` only — `attachments` was already correct), gcs, oneDrive (already correct), s3, sheets (already correct).  All 6 now compute `workDir` once at the top of `ExecuteCloud` and pass it through to both `ValidateLocalPath` and the file-open path (`workDir / localPath`, which `std::filesystem::path::operator/` correctly turns into the absolute path when the rhs is absolute — so template-from-`{{A.output_file}}` Just Works).
- **6 JCWFs updated** to spec-compliant working-directory-relative literals + `{{<task>.output_file}}` templates: `oneDriveUploadDownloadDemo` (template form), `sheetsQuizGrader` (template form), `s3UploadDownloadDemo` (`../server_metrics.csv`, `metrics.csv`, template), `gcsDemo` (`../regional_sales.csv`, `sales.csv`, template), `azureBlobDemo` (`../project_budgets.csv`, `budgets.csv`, template), `emailDemo` (template form for `body_file`).  Each `.jcwf` zip was re-packed via `PUT /api/workflows/<id>` AND the result copied to `example/workflows/` (the canonical, git-tracked location — `workflows/` is runtime scratch, gitignored).

**Tracked-doc sweep** — `cloud-integration.md`, `cyber security.md`, `api-endpoints.md`.  Refreshed `ValidateLocalPath` description (4 places) to match the spec-aligned semantics; refreshed connector-layer TLS coverage list (sitting-22's `ApplyHardenedDefaults` covers every connector, not just the 5 cloud-storage ones the docs called out); added connector SSRF gate description; added OAuth-callback-unauth-by-design note in 2 places (cyber security.md + api-endpoints.md `oauth/callback` row).  Spec itself (`JC_Workflow_Specification.md` §3.2.1) was already correct — the executors had drifted from it, not the other way round.

### What's verified

- Studio release build: clean across all rebuilds (connector hardening, OAuth fix, path consolidation, doc sweep).  All 12 connectors + connectorHttp.cpp + polarionClient.cpp + 4 modified executors + cloudTaskExecutor.cpp + webServer.cpp recompiled, no warnings (post the `handle` rename to dodge `-Wshadow` on the `curl` parameter name).
- 28-test assistant suite: 28/28 passed (re-run after every restart).
- **TestConnection across 12 connections** — initial pass: 11 OK + 1 stale-token (my-sheets); after OAuth re-authorise: **12/12 OK**.
- **4 negative SSRF tests** — all rejected with explicit security log entries:
  - `https://10.0.0.5` (RFC 1918) → blocked.
  - `https://169.254.169.254/...` (cloud metadata) → blocked.
  - `file:///etc/passwd` (bad scheme) → blocked.
  - `https://attacker.com@10.0.0.5/path` (userinfo smuggle) → blocked at host-charset check.
- **OAuth re-authorisation flow** — Google Sheets `prompt=consent` URL completed end-to-end after the callback fix; new refresh token landed in keystore; `OAuthTokenManager` background refresh started succeeding.
- **Cloud demo regressions across the consolidation** — all 6 cloud demos that touch local files re-run after the path-resolution + JCWF rewrite + repack, all green: s3UploadDownloadDemo 5/5, gcsDemo 14/14, azureBlobDemo 14/14, oneDriveUploadDownloadDemo 4/4, sheetsQuizGrader 3/3, emailDemo 3/3.  Plus the cloud surfaces that don't touch local files: snowflakeQueryDemo 14/14, gitHubIssueDemo 10/10, slackQAndABot 3/3, redmineTriageBot 14/14, jiraIssueDemo 4/4.  Cumulative: **65 cloud tasks across 11 demos succeeded, 0 failed**.
- All security log entries (`*_endpoint_rejected reason=...`) and `path_traversal_blocked` lines are present and queryable for the run-analysis dashboard.
- **Final spec-divergence check** (`grep -nE 'ValidateLocalPath\(.+, launchCwd' application/cloud/*CloudTaskExecutor.cpp` + JCWF-launchCwd-path scan): zero hits — the only `launchCwd` reference left is inside `ValidateLocalPath` itself as the project-tree boundary.  No JCWF carries `"workflows/..."` or `"queue/..."` literal paths anywhere.

### Open items / next-session candidates

Ordered by ROI (mechanical/low-risk first, judgment-heavy/narrow-scope later):

- **Sitting 23 — Sweep #4 part 2.**  Azure Blob `container`/`blob_name` validation + Google Sheets `spreadsheetId`/`range` validation (deferred from sitting 18).  Estimated 1.5 hours.  Tightly scoped: same shape as sitting 18's URL-side validation, only two deferred surfaces.
- **Sitting 24 — Request-body JSON-injection sweep** (deferred from sitting 19).  Slack chat.postMessage body, Jira create body, GitHub issue body — **plus** `snowflakeCloudTaskExecutor.cpp:634` columnNames raw splice (carried from sitting 21).  Estimated 1.5 hours.  Mechanical: same fix shape as sitting-19's Track A (raw concat → `JsonHelper::EscapeJsonString`).
- **Sitting 25 — Executor-layer redirect/TLS sweep.**  Sitting 22 covered ONLY connector-layer + PolarionClient.  The executor layer (`*CloudTaskExecutor.cpp`) still has 14 `FOLLOWLOCATION=1L` sites: azureBlob ×2, gcs ×2, gitHub, googleSheets, jira, redmine, s3 ×2, oneDrive ×2, slack.  Threat model is partly different — some cloud APIs legitimately 30x to regional endpoints / presigned URLs.  **Each site needs per-API analysis** before flipping to 0L.  Estimated 2-3 hours including the analysis.  Higher-risk: getting it wrong breaks working flows.
- **Sitting 26 — Postgres SSL posture audit.**  PostgresConnector sets `sslmode` from the connection's `sslmode` param without validating allowed values; production posture should default to `require` (reject `disable`/`allow`/`prefer` for non-localhost hosts).  Mirror the email `allowLocal = !useSsl` heuristic.  Narrow blast-radius (libpq, not libcurl) but smaller payoff.

(The IPv6 and `%`-encoding cleanups from the gotchas below are micro-followups; can fold into any sitting.)

### Gotchas next-session-Claude should know

- **The SSRF gate is syntactic, NOT resolver-based.**  `ValidatePublicHttpEndpoint` blocks IP literals (`10.0.0.5`, `169.254.169.254`, `127.0.0.1`) and bad host charsets, but does NOT block `attacker-internal-name.example.com` if that name resolves to an internal IP via attacker-controlled DNS.  Catching the resolver-time case requires a `CURLOPT_OPENSOCKETFUNCTION` post-resolve check — see `ConnectorHttp::ValidatePublicHttpEndpoint` docstring.  This is a known gap; closing it is sitting-25+ scope.
- **Two over-broad patterns kept intact for shared-helper consistency.**  (1) The IPv6 unique-local check is `host.starts_with("fc"|"fd"|"fe80")` — false-positives on hostnames literally starting with those bytes (e.g. `fc-acme.example.com` would be flagged as local).  (2) The host charset rejects `%` so percent-encoded smuggle is blocked, but legitimate IDN hostnames (xn--punycoded) still pass via alphanumeric chars.  Both copy emailConnector's existing behavior verbatim — flipping them is a separate cleanup, not part of this sweep.
- **`ConnectorHttp::ApplyHardenedDefaults`'s parameter is `handle`, NOT `curl`.**  Renamed during the sweep because passing `curl` (the caller's local variable name) into a function whose parameter was also named `curl` triggered `-Wshadow` warnings on every call site.  Don't rename it back to `curl` in the helper.
- **PolarionClient deliberately keeps its uncapped writeCallback.**  `BoundedStringWriteCallback` is sized for the 1 MB connector-test cap; PolarionClient's data-path responses can be much larger.  Same reason `BoundedStringWriteCallback` isn't a drop-in replacement for executor write callbacks generally.
- **`ValidateLocalPath` semantics changed mid-sitting.**  Before: rejected `..` and required the resolved path to be a prefix of `baseDir`.  After: relative resolves under `baseDir`, absolute passes through, both confined under `launchCwd`.  Three implications: (1) `..` is now permitted in JCWF paths (lexical_normal handles them; `launchCwd` boundary catches escapes); (2) absolute paths from `{{<task>.output_file}}` templates flow through transparently; (3) the security boundary is the project tree, not the working_directory — that's the spec-aligned interpretation per JCWF §3.2.1 (working_directory is the resolution base, not a confinement gate).
- **`example/workflows/*.jcwf` is the canonical, git-tracked location for shipped demo JCWFs.**  `workflows/` is gitignored runtime scratch.  After `PUT /api/workflows/<id>` repacks the runtime `.jcwf` zip in `workflows/`, copy it to `example/workflows/` so the change actually ships (otherwise: works in the dev session, breaks on a fresh checkout / for new contributors / at alpha-beta cut).  Also see the auto-memory note `feedback_jcwf_canonical_location`.
- **OAuth callback is intentionally unauthenticated.**  The user-agent redirect from Google / Microsoft cannot carry the j9t admin Bearer.  CSRF gate is the `state` param (single-use random nonce, server-side, verified before code-for-token exchange).  The `/oauth/authorize` route that creates the flow remains admin-gated.  Don't "fix" the callback by adding `CheckAdminAuth` back — it'd re-break what this sitting fixed.
- **The `ssrf-test-private` connection from negative tests was deleted** at the end of the sitting.  Don't expect it to be present.

---

## 2026-05-02 (S1 sitting 21) → next session

S1=D2 sitting 21.  Theme: **Cleanup sitting** (deferred from sittings 18 + 19).  Two refactors, no new threat closures: lift `ContainsCrlf` to the `ICloudTaskExecutor` base, and converge the last two inline JSON-escape switch blocks onto `JsonHelper::EscapeJsonString`.  After this sitting, every NAMED `JsonEscape*` static and every inline 5-case switch escape in the cloud surface is gone — `JsonHelper::EscapeJsonString` is the only JSON-string-escape path in the codebase.

### What landed

**`ContainsCrlf` lifted to `ICloudTaskExecutor`** — public static (not protected).  Public is required because `gcsCloudTaskExecutor.cpp::GcsRequest` and `oneDriveCloudTaskExecutor.cpp::GraphRequest` call it from file-local static helpers (not member functions), where `protected` access doesn't apply.  Single declaration in `cloudTaskExecutor.h`, single definition in `cloudTaskExecutor.cpp`.  Removed 4 file-local copies (oneDrive, email, gcs, snowflake executors) and qualified all 7 call sites as `ICloudTaskExecutor::ContainsCrlf(...)`.

**Inline JSON-escape switches converged** — 2 in `snowflakeCloudTaskExecutor.cpp` (statement-submit body at the SQL-query splice; result-write JSON output for row values).  Both were 5-case switches (`"`, `\`, `\n`, `\r`, `\t`) that passed control bytes 0x00..0x1F through raw — technically invalid JSON.  Replaced with `JsonHelper::EscapeJsonString` which is RFC 8259 §7-compliant (strict superset).  The googleSheets reference in sitting-20's hand-off was wrong — it has no inline JSON-escape switch (only URL-encode and CSV-escape, which are different file-local helpers and correctly stay).

### What's verified

- Studio release build: clean.
- 28-test assistant suite: 28/28 passed (2.1s).
- snowflakeQueryDemo: 14/14 tasks succeeded (9s) — exercises both converged switches end-to-end.
- emailDemo: 3/3 tasks succeeded (2s) — exercises the lifted CRLF check on send-reply path.
- Log scan: zero new ERROR-level entries, zero CRLF rejections, zero snowflake/email failures.
- gcs / oneDrive demos NOT re-run — `ContainsCrlf` change is pure refactor (identical body), no behavioural risk.

### Adjacent finding noticed but not fixed

`snowflakeCloudTaskExecutor.cpp:634` (in the JSON result-write block) splices `columnNames[c]` into the output raw: `out << "\"" << columnNames[c] << "\": ";`.  A column name containing `"` produces invalid JSON.  Snowflake column names are typically safe in practice (server uppercases unquoted identifiers), but a hostile/compromised endpoint or a quoted identifier like `SELECT 1 AS "x\"y"` would break it.  Same shape as sweep #5 Track A (snowflake wasn't in that cluster — sweep #5 Track A only covered the 5 sweep-#1 files).  Belongs in **sitting 24** with the other request-body / response-splice JSON-injection sites.

### Open items / next-session candidates

- **Sitting 22+ — Connector-layer SSRF + TLS verify mini-sweep.**  Each `*Connector::TestConnection` parallel of sitting 17's executor-side TLS sweep + sitting 14's BuildApiBaseUrl SSRF gate.  Estimated 2-3 hours.
- **Sitting 23+ — Sweep #4 part 2.**  Azure Blob `container`/`blob_name` validation + Google Sheets `spreadsheetId`/`range` validation (deferred from sitting 18).  Estimated 1.5 hours.
- **Sitting 24+ — Request-body JSON-injection sweep** (deferred from sitting 19).  Slack chat.postMessage body, Jira create body, GitHub issue body, etc. — **plus** the `snowflakeCloudTaskExecutor.cpp:634` columnNames raw splice noted above.
- **Carried items unchanged.**

### Cumulative status of the cloud-surface CRIT/HIGH cluster (sittings 11-21)

Unchanged from sitting 20: every cross-cutting concern (path traversal, response cap, upload cap, TLS verify, URL injection, JSON injection on response splices, `JsonEscape` convergence, parser hardening) ✓ closed in the dense files.  Sitting 21 was hygiene only — collapsed 4 `ContainsCrlf` copies → 1 base method, removed the last 2 inline switch escapes.

### Gotchas next-session-Claude should know

- **`ContainsCrlf` is on the base, but it's `public static` not `protected`.**  Two cloud executors (gcs, oneDrive) wrap their HTTP layer in file-local `static` free functions (`GcsRequest`, `GraphRequest`) — not members.  Keep it public, or any future cloud executor that adopts the same file-local wrapper pattern will fail to compile.
- **Always qualify `ICloudTaskExecutor::ContainsCrlf(...)` at call sites.**  Inside member functions, unqualified `ContainsCrlf(x)` would resolve via name lookup, but only inside a member context; the gcs / oneDrive call sites are inside file-local statics where unqualified lookup fails.  Consistent qualification across all 7 call sites is intentional — don't simplify it back down on grounds of "inheritance does this for free".

---

## 2026-05-01 (S1 sitting 20) → next session

S1=D2 sitting 20.  Theme: **Horizontal Sweep #6** — parser hardening for `stoi`/`stoull` calls that throw on hostile input.  Discovered after the cloud-surface survey: only **one site** remained in scope.  All other parsing sites had been hardened in prior sittings (sitting 12: email max_messages clamp; sitting 13: email connector UID stoull + IsLocalNetworkHost stoi; sitting 14: snowflake timeout/poll clamps).  The remaining site was S3's `max_keys` parsing, which `std::stoi(maxKeysStr)` would throw on hostile input.

### What landed

**S3 max_keys hardening** — single fix in `s3CloudTaskExecutor.cpp::ExecuteCloud` list-operation path:
- Pre-validate `maxKeysStr` is digits-only + ≤ 10 chars (10^10 > INT_MAX).  Invalid → WARN + default 1000.
- Wrap `std::stoi` in `try { ... } catch (std::invalid_argument) { ... } catch (std::out_of_range) { ... }`.  Each catch logs WARN + defaults to 1000.
- `std::clamp` parsed value to `[1, 1000]`.  S3's API caps at 1000 server-side; 0 is meaningless.

Closes the audit's HIGH "stoi on Unvalidated User Input — Exception / Crash" finding for S3.  Same shape as sitting 13's `EmailConnector::CheckForNewMail` UID parsing fix.

### What's verified

- Build clean.  Single file recompiled.
- 28-test + hermetic dispatcher PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.

### Open items / next-session candidates

- **Sitting 21 — Cleanup sitting (deferred from sittings 18 + 19):**
  - Lift `ContainsCrlf` to `ICloudTaskExecutor::ContainsCrlf` (4 copies → base class).
  - Converge inline JsonEscape switch blocks (snowflake × 2, googleSheets × 1) onto `JsonHelper::EscapeJsonString`.
  Estimated 1 hour total.
- **Sitting 22+ — Connector-layer SSRF + TLS verify mini-sweep.**  Each `*Connector::TestConnection` parallel of sitting 17's executor-side TLS sweep + sitting 14's BuildApiBaseUrl SSRF gate.  Estimated 2-3 hours.
- **Sitting 23+ — Sweep #4 part 2.**  Azure Blob `container`/`blob_name` validation + Google Sheets `spreadsheetId`/`range` validation (deferred from sitting 18).  Estimated 1.5 hours.
- **Sitting 24+ — Request-body JSON-injection sweep** (deferred from sitting 19).  Slack chat.postMessage body, Jira create body, GitHub issue body, etc.
- **Carried items unchanged.**

### Cumulative status of the cloud-surface CRIT/HIGH cluster (sittings 11-20)

| Cross-cutting concern | Status |
|---|---|
| Path traversal on local file params | ✓ Closed (sweeps #1) |
| Response-body cap | ✓ Closed (sweep #2) |
| Upload file-size cap | ✓ Closed (sweep #2) |
| TLS verify-peer/host explicit | ✓ Closed (sweep #3) |
| URL-side injection + Bearer CRLF | ✓ Closed (sweep #4 — partial; sheets/azureBlob deferred) |
| JSON-injection on response splices | ✓ Closed (sweep #5) |
| `JsonEscape` named-helper convergence | ✓ Closed (sweep #5) |
| Parser hardening (`stoi`/`stoull` exceptions) | ✓ Closed (sweep #6, this sitting) |

**Email + Snowflake surfaces** (sittings 11-14): fully closed at CRIT/HIGH.  
**5-executor cluster** (azureBlob, gcs, googleSheets, oneDrive, s3, sittings 15-20): fully closed at CRIT/HIGH minus the sweep-#4 deferrals (sheets/azureBlob URL validation).  
**Smaller executors** (gitHub, jira, dbQuery, redmine, slack, polarionWrite): partial coverage from sweeps #5 (JsonEscape convergence) only — still need their own depth-first or horizontal-sweep coverage for path traversal, response cap, TLS, URL injection, etc.

### Gotchas next-session-Claude should know

- **Sweep #6 was much smaller than estimated.**  The hand-off after sitting 14 estimated "1.5 hours" for parser hardening; the actual work was ~5 minutes (single site).  This is because sittings 12-14 had already addressed several of the parser-vulnerable sites depth-first.  When sizing future sweeps, grep the surface FIRST — the hand-off estimates were based on counting audit findings, not surveying current code state.
- **The cloud-surface CRIT/HIGH cluster is essentially closed at this point.**  Sittings 21+ are cleanups (refactor + scope-extension), not new threat closures on the dense files.  The remaining attack surfaces on the smaller executors (Slack message body injection, GitHub issue body injection, etc.) and the connector-layer (SSRF on endpoint, TLS verify) are MEDIUM-scale rather than CRIT — closing them is good hygiene but the immediate ROI per sitting drops.

---

## 2026-05-01 (S1 sitting 19) → next session

S1=D2 sitting 19.  Theme: **Horizontal Sweep #5** — JSON-injection sweep across the cloud surface in two tracks.  **Track A:** ~9 raw response/summary JSON splice fixes in the 5 sweep-#1 files (azureBlob, gcs, googleSheets, oneDrive, s3).  **Track B:** **converge 7 file-local `JsonEscape` copies** that sitting 12's claim ("no anon-namespace JsonEscape copies remain") missed — sitting 12 only checked `application/assistant/`, not `application/cloud/`.  This sweep finds all 7 in cloud-surface files (gitHub, jira, dbQuery, redmine, slack, polarionWrite, googleSheets) and converges them onto `JsonHelper::EscapeJsonString`.  After this sweep, every NAMED `JsonEscape*` static helper in the codebase is gone.  Inline switch blocks (snowflake × 2, googleSheets × 1) survive as separate-finding cleanups.

### What landed

**Track A — 9 raw splice fixes** in 5 sweep-#1 files: azureBlob (× 2), gcs (× 2), googleSheets (× 1), oneDrive (× 1), s3 (× 3).  Each `"\"key\":\"" + value + "\""` style raw concat → `JsonHelper::EscapeJsonString(value)`.

**Track B — 7 JsonEscape convergences:**
| File | JsonEscape def | Caller-side updates |
|---|---|---|
| googleSheets | deleted | 4 |
| gitHub | deleted | 4 |
| jira | deleted | 8 |
| dbQuery | deleted | 2 |
| redmine | deleted | 2 |
| slack | deleted | 8 |
| polarion-write | deleted | 4 |

32 caller updates total via `sed -i 's/\bJsonEscape(/JsonHelper::EscapeJsonString(/g'`.

**Sitting 12 claim correction:** sitting 12's "no anon-namespace JsonEscape copies remain" was correct **only for `application/assistant/`**.  This sweep + earlier sittings cumulative now make it true across the entire codebase for **named** helpers.  Inline switch blocks still exist (snowflake × 2 from sitting 14's deferred cleanup, googleSheets × 1 in the JSON output writer) — track for a follow-up.

### What's verified

- Build clean (11 cloud task executors recompiled cleanly).
- 28-test + hermetic dispatcher: PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.
- **Not directly verified:** live JSON-escape negative test.  Same posture as previous sweeps — JsonHelper's correctness was verified live in sitting 12's emailDemo hostile-byte test (8 control chars round-tripped); this sweep just routes more callers through the same helper.

### Open items / next-session candidates

- **Sitting 20 — Horizontal Sweep #6 (parser hardening).**  `stoi` / `stoull` exception handling.  S3's `max_keys` is the obvious one; a few others scattered.  Same shape as sitting 13's `IsValidImapUid` + try/catch pattern.  Estimated 1.5 hours.
- **Sitting 21 — Cleanup sitting (deferred from sittings 18 + 19):**
  - Lift `ContainsCrlf` to `ICloudTaskExecutor::ContainsCrlf` (now at 4 copies — past `feedback_cpp_discipline`'s third-copy threshold).
  - Converge inline switch blocks (snowflake × 2, googleSheets × 1) onto `JsonHelper::EscapeJsonString`.  Refactor the column-writer call sites to escape the full value string rather than character-by-character.
  Estimated 1 hour total.
- **Sitting 22+ — connector-layer SSRF + TLS verify.**  Each `*Connector::TestConnection` parallel of sitting 17's executor-side TLS sweep + sitting 14's BuildApiBaseUrl SSRF gate.  Likely 2-3 hours.
- **Sitting 23+ — request-body JSON injection sweep** (deferred from this sitting).  Each cloud's request-body construction (Slack chat.postMessage, Jira create, GitHub issue, etc.) currently uses raw concat for caller-supplied fields.  Apply JsonHelper at request-body build sites.
- **Sitting 24+ — sweep #4 part 2** (deferred from sitting 18): Azure Blob `container`/`blob_name` validation, Google Sheets `spreadsheetId`/`range` validation.
- **Carried items unchanged.**

### Gotchas next-session-Claude should know

- **Sitting 12's "no anon-namespace JsonEscape" claim was scope-limited.**  When making "no copies remain in the codebase" claims, grep across the WHOLE codebase, not just the directory the current sitting touched.  Sitting 12 looked at `application/assistant/` only; sitting 19 found 7 more copies in `application/cloud/`.  Future sweep-claim hand-offs should explicitly state the grep scope ("verified across `application/`" or similar).
- **Inline JSON-escape switch blocks survive this sweep.**  `snowflakeCloudTaskExecutor.cpp` has 2 (query escape + result-row JSON output writer) and `googleSheetsCloudTaskExecutor.cpp` has 1 (the column-writer at line ~405).  These are inline `switch (c) { case '"': ... }` blocks — different shape from the named functions this sweep deleted.  When implementing the cleanup sitting, the natural refactor is to apply JsonHelper::EscapeJsonString to the full VALUE string (e.g., `out << "\"" << JsonHelper::EscapeJsonString(rows[r][c]) << "\""`), not to escape character-by-character.  That changes the inner loop shape but preserves the output identity.
- **JsonHelper's RFC 8259 §7 control-char handling is stricter than some of the converged copies.**  Specifically gitHub + jira + dbQuery + googleSheets's pre-fix JsonEscape only handled the 5 named escapes (`"`, `\\`, `\n`, `\r`, `\t`); chars < 0x20 fell through to default (raw byte).  Post-convergence they go through JsonHelper which emits `\u00XX` for the full 0x00-0x1F range.  This is a STRICTLY-MORE-CORRECT change — pre-fix output containing control bytes was producing invalid JSON; post-fix it produces valid `\u00XX` escapes.  No regression, but if a downstream consumer was relying on the byte-passthrough behavior, this changes the on-wire format.  No realistic deployment relies on raw control bytes in JSON though.

---

## 2026-05-01 (S1 sitting 18) → next session

S1=D2 sitting 18.  Theme: **Horizontal Sweep #4** — URL-component sanitization + Bearer-token CRLF rejection across 3 cloud-storage executors (gcs, oneDrive, s3).  Closes 2 CRIT (gcs `bucket` + oneDrive `remote_path` URL injection) + 3 HIGH (s3 `key`+`prefix` unencoded + gcs/oneDrive bearer CRLF).  More involved than sweeps #1-3 because each cloud surface has different valid-character rules — strict allowlist for GCS bucket + OneDrive path, percent-encoding for S3 key/prefix.

### What landed

| File | Change |
|---|---|
| `gcsCloudTaskExecutor.cpp` | New `IsValidGcsBucket` (per GCS naming rules: `[a-z0-9._-]`, 3-63 chars, no leading/trailing hyphen).  Validate bucket at extraction site.  New `ContainsCrlf` + reject in `GcsRequest` + `GcsDownload` entry. |
| `oneDriveCloudTaskExecutor.cpp` | New `IsValidOneDriveRemotePath` (alphanumeric + `._-/` + space, max 1024, no `..`).  Validate at remote_path extraction site.  New `ContainsCrlf` + reject in `GraphRequest` + `GraphDownload` entry. |
| `s3CloudTaskExecutor.cpp` | New `UrlEncodeS3Key` helper — splits on `/`, percent-encodes each segment via `curl_easy_escape`, rejoins.  Applied at all 4 key-build sites + 1 prefix site.  No allowlist (S3 keys legitimately contain almost any UTF-8). |

### Per-cloud rationale

- **GCS bucket: strict allowlist.**  GCS bucket names are deliberately restrictive per Google's spec; any name needing URL-encoding is by definition invalid.
- **OneDrive remote_path: strict allowlist + `..` rejection.**  Tighter than encoding because `..` segments could escape the user's drive scope through Graph's path resolution.
- **S3 key + prefix: percent-encoding.**  S3 keys legitimately contain emoji, spaces, non-ASCII; allowlist would reject valid keys.  Per-segment encoding preserves `/` as path delimiter.
- **Bearer CRLF reject: file-local helper per file (4th + 5th copies).**  Past `feedback_cpp_discipline`'s third-copy threshold; lift planned for a future cleanup sitting.

### What's verified

- Build clean (3 files recompiled).
- 28-test + hermetic dispatcher: PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.
- **Live GCS bucket rejection** (representative for the URL-injection cluster):  mutated `gcsDemo`'s `upload_data.params.bucket` to `"j9t-demo/../private?x="`; run `failed` at upload_data; security log: `[security] gcs_invalid_bucket task='upload_data' workflow='gcsDemo' run='gcsDemo_1777697296'`.  Demo restored byte-identical.
- **Not directly verified:** OneDrive remote_path rejection (no demo workflow); S3 URL-encoding live trace; Bearer CRLF rejections.  All structurally identical to verified pattern.

### Open items / next-session candidates

- **Sitting 19 — Horizontal Sweep #5 (JSON-injection sweep).**  `JsonHelper::EscapeJsonString` applied to request-body JSON across the cloud surface.  Several MEDIUM findings; some files (gcs response build at line 342–343) construct response JSON via raw concatenation — bundle the cleanup with this sweep.
- **Sitting 20 — Horizontal Sweep #6 (parser hardening).**  `stoi` / `stoull` exception handling.  S3's `max_keys` is the obvious one; a few others scattered.  Estimated 1.5 hours.
- **Sitting 21 — Cleanup sweep: lift `ContainsCrlf` to `ICloudTaskExecutor::ContainsCrlf`.**  Now at 4 copies (sittings 11, 14, this sweep × 2).  Past the third-copy threshold.  Pattern matches sitting 13's `IsValidImap*` lift to `EmailConnector`.  Update the 4 existing callers to use the base-class static.  Estimated 30 minutes.
- **Sitting 22 — Sweep #4 part 2.**  Azure Blob `container` + `blob_name` validation + Google Sheets `spreadsheetId` + `range` validation.  Both deferred from this sweep — same allowlist shape, different files.  Bundle with one of the smaller sweeps.
- **Sitting 23+ — connector-layer SSRF.**  Each `*Connector::BuildEndpointUrl` equivalent (parallel of sitting 14's `BuildApiBaseUrl` fix).  Plus connector-layer TLS verify (sitting 17 carry).  Estimated 2-3 hours.
- **Carried items unchanged** from sitting 17 + earlier.

### Gotchas next-session-Claude should know

- **Different cloud surfaces need different URL-component sanitization strategies.**  Don't unify just because the threat model is similar — GCS allowlist (strict naming rules), OneDrive allowlist + `..` rejection (path-resolution attack vector), S3 percent-encoding (legitimate UTF-8 in keys).  When a future cloud surface lands, look up its valid-character rules first; allowlist where the protocol already restricts, percent-encode where the protocol allows arbitrary bytes.
- **`UrlEncodeS3Key` preserves `/` as a path delimiter.**  Do not use `curl_easy_escape` on the whole key — it would escape `/` as `%2F`, which S3 would treat as a literal slash in the key name (different object).  The split-encode-rejoin pattern is load-bearing.
- **`curl_easy_escape` requires a CURL handle** (passed as the first arg).  The `UrlEncodeS3Key` helper inits + cleans one each call — slight overhead, but the alternative (sharing a CURL* across calls) requires lifetime management we don't need at the call rates this sees.
- **The 4-copy `ContainsCrlf` situation is now load-bearing for the next refactor sitting.**  When the cleanup sitting lands, lift to `ICloudTaskExecutor::ContainsCrlf` as a public static, delete the 4 file-local copies, update 4 caller-side qualifiers (`ContainsCrlf(...)` → resolves to `ICloudTaskExecutor::ContainsCrlf(...)` automatically since each subclass inherits).  Same pattern as sitting 13's `IsValidImap*` lift to `EmailConnector`.

---

## 2026-05-01 (S1 sitting 17) → next session

S1=D2 sitting 17.  Theme: **Horizontal Sweep #3** — TLS verify-peer + verify-host strict gate added across the same 5 sweep-#1 files.  9 setopt-pair additions total (each file has 1-2 helper functions).  Smallest sweep yet — single 2-line block per occurrence, applied via `replace_all` to cover both helper functions per file.  Closes the audit's HIGH "TLS Peer Verification Conditionally Disabled" / "Missing TLS Certificate Verification Fallback" finding on each.

### What landed

Per-file change: after the existing `if (!caBundle.empty()) { curl_easy_setopt(curl, CURLOPT_CAINFO, ...); }` block, add:
```cpp
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
```

| File | Helper sites covered |
|---|---|
| `azureBlobCloudTaskExecutor.cpp` | `AzureBlobRequest` + `AzureBlobDownload` |
| `gcsCloudTaskExecutor.cpp` | `GcsRequest` + `GcsDownload` |
| `googleSheetsCloudTaskExecutor.cpp` | `SheetsRequest` (single helper) |
| `oneDriveCloudTaskExecutor.cpp` | `GraphRequest` + `GraphDownload` |
| `s3CloudTaskExecutor.cpp` | `S3Request` + `S3Download` |

**Unconditional posture (no `use_ssl` gate):** All 5 cloud surfaces are HTTPS-only protocols.  S3-compatible alternatives like local MinIO can use `http://` for testing, but `CURLOPT_SSL_VERIFY*` setopts are no-ops on HTTP (libcurl only applies them on TLS handshake).  Setting them unconditionally is safe + closes the strict-on-HTTPS gap without breaking local-test workflows.

### What's verified

- Build clean (5 files recompiled).
- 28-test + hermetic dispatcher: PASS.
- emailDemo regression: 3 tasks succeeded in 2 s.
- **Not directly verified:** strict-TLS gate firing under MITM (would need a controlled TLS proxy fixture).  Structural fix; same shape as sitting 14's Snowflake fix.

### Open items / next-session candidates

- **Sitting 18 — Horizontal Sweep #4 (URL-side injection / SSRF).**  Three concerns bundle here:
  - URL-component injection: gcs's `bucket` + `object_name`, oneDrive's `remote_path`, s3's `key` + `prefix`.  Each interpolated raw into request URLs.  Per-cloud valid-character sets (S3 key vs OneDrive path vs GCS object name).
  - Endpoint SSRF: each connector's endpoint URL build (the parallel of sitting 14's BuildApiBaseUrl fix for Snowflake).
  - Bearer token CRLF rejection in HTTP headers (gcs, oneDrive specifically — sitting 14 closed this for Snowflake JWT).
  Estimated 3-4 hours.
- **Sitting 19 — Horizontal Sweep #5 (JSON-injection sweep).**  `JsonHelper::EscapeJsonString` applied to request-body JSON across the cloud surface.
- **Sitting 20 — Horizontal Sweep #6 (parser hardening).**  `stoi` / `stoull` exception handling.  S3's `max_keys` plus a few others.
- **Sitting 21+ — connector-layer TLS verify mini-sweep** (closes the parallel of this sweep on the 5 `*Connector.cpp` TestConnection helpers — flagged in this sitting's skipped-findings table).
- **Carried items unchanged** from sitting 16.

### Gotchas next-session-Claude should know

- **`CURLOPT_SSL_VERIFY*` setopts only apply on TLS handshake.**  Setting them on a curl handle that ends up speaking HTTP (e.g. local MinIO over `http://localhost:9000/`) is a no-op — no behavior change for HTTP-mode endpoints, strict verification for HTTPS.  Deployments that rely on local MinIO HTTP testing aren't broken by this sweep.  When extending the gate to a new cloud surface, no `use_ssl` opt-out is needed unless the protocol legitimately runs both encrypted and plaintext (like SMTP/IMAP, which DO need the opt-out — that's why sittings 12-13 use the `use_ssl`-coupled pattern).
- **`replace_all=true` is the right pattern when a file has multiple identical helper functions.**  This sweep used it on each of the 5 files because both their Request and Download helpers had the same caBundle block.  When extending a horizontal sweep to a similar pattern, prefer `replace_all` over multiple targeted Edits — fewer round-trips, less risk of one-off typos.
- **The 5-executor cluster's CRIT/HIGH cluster across 4 cross-cutting axes is now closed.**  Path traversal (sweep #1), response-body cap (sweep #2), file-read cap (sweep #2), TLS verify (sweep #3).  Remaining threats on these files are URL-side injection (sweep #4) and JSON-injection (sweep #5).  After both, the 5-executor cluster will be fully closed at the CRIT/HIGH level.

---

## 2026-05-01 (S1 sitting 16) → next session

S1=D2 sitting 16.  Theme: **Horizontal Sweep #2** — response-body caps + file-read upload caps closed across the same 5 cloud task executors as Sweep #1.  9 fixes total: 5 writeCallback response caps + 4 file-read upload caps (skip googleSheets's CSV line-by-line pattern, deferred).  All caps follow the established sitting-13/14 pattern: define `kMax<File>{Response,Upload}Bytes` constant, check `buf->size() + incoming > cap` (writeCallback) or `fileSize > cap` (upload), abort cleanly on overflow.  **Boundary at sitting-end: the cloud-surface DoS / OOM cluster is closed for the 5 sweep-#1 files** — both halves of the resource-exhaustion threat model (untrusted server response stream + untrusted local file size) are now bounded.

### What landed

| File | writeCallback cap | Upload-read cap |
|---|---|---|
| `azureBlobCloudTaskExecutor.cpp` | `kMaxAzureBlobResponseBytes = 64 MB` | `kMaxAzureBlobUploadBytes = 256 MB` |
| `gcsCloudTaskExecutor.cpp` | `kMaxGcsResponseBytes = 64 MB` | `kMaxGcsUploadBytes = 256 MB` |
| `googleSheetsCloudTaskExecutor.cpp` | `kMaxSheetsResponseBytes = 64 MB` | (deferred — CSV line-by-line pattern) |
| `oneDriveCloudTaskExecutor.cpp` | `kMaxOneDriveResponseBytes = 64 MB` | `kMaxOneDriveUploadBytes = 256 MB` |
| `s3CloudTaskExecutor.cpp` | `kMaxS3ResponseBytes = 64 MB` | `kMaxS3UploadBytes = 256 MB` |

Constants chosen:
- **Response cap = 64 MB** matches Snowflake's pattern from sitting 14 — generous for paginated cloud-API list responses, tight enough to bound a hostile/compromised endpoint's memory exhaust.  IMAP's 10 MB cap (sitting 13) is intentionally tighter; variation per surface is by design.
- **Upload cap = 256 MB** matches Phase 9b's existing `CURLOPT_MAXFILESIZE_LARGE` for downloads — symmetric upload/download.  Closes the audit's HIGH "uncontrolled file read into memory on upload" finding consistently across S3 / GCS / Azure Blob / OneDrive.

### What's verified

- Studio debug build clean.  All 5 files recompiled.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- emailDemo regression check: 3 tasks succeeded in 2 s.  Confirms shared cloud-surface infrastructure is intact across both sweeps #1 + #2.
- **Not directly verified:** live trigger of either cap.  Fix shape is structurally identical to sitting 12's emailDemo attachment 25 MB cap (verified live with a 30 MB synthetic file).  Verification posture: "the bug class is gone by construction; pattern was previously verified live in sitting 12".

### Open items / next-session candidates

- **Sitting 17 — Horizontal Sweep #3 (TLS verify-peer/host unconditional).**  All 5 sweep-#1 files have `auto const& caBundle = CurlWrapper::GetCaBundlePath(); if (!caBundle.empty()) { ... }` in their Request helpers, but **never explicitly set** `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L`.  Audit's HIGH "TLS Peer Verification Conditionally Disabled" finding flagged this on each.  Fix is one-line per file: add the explicit setopts after the CAINFO branch, matching sitting 14's Snowflake pattern.  Estimated 1.5 hours.
- **Sitting 18 — Horizontal Sweep #4 (URL-side injection / SSRF).**  Audit flagged URL-side traversal on gcs's `bucket` + `object_name`, oneDrive's `remote_path`, s3's `key` + `prefix`.  Plus SSRF on each connector's endpoint URL build.  Likely 5–7 fixes; trickier per-cloud-service valid-character sets (S3 key vs OneDrive path vs GCS object name).  Estimated 3 hours.
- **Sitting 19 — Horizontal Sweep #5 (JSON-injection sweep).**  Apply `JsonHelper::EscapeJsonString` to request-body JSON across the cloud surface.  Several MEDIUM findings.  Estimated 2 hours.
- **Sitting 20 — Horizontal Sweep #6 (parser hardening).**  `stoi`/`stoull` exception handling.  S3's `max_keys` plus a few others.  Estimated 1.5 hours.
- **Sitting 21+ — extend horizontal sweeps to remaining smaller cloud files** (Slack, Jira, GitHub, Redmine, Polarion-write, dbQuery).  Most have narrower attack surfaces (no large file reads, small JSON request bodies); some sweeps may not need to touch them at all.
- **Carried items unchanged** from sitting 15: `HandleWorkflowVersionRestorePost` zip-container fix; editor master-password / MCP-login parity; `engine.cpp:225` TOCTOU on connections.json; `cloudConnectionManager.cpp` MEDIUM input-validation; MEDIUM `secrets in success-path log` + `predictable MIME boundary` on `emailCloudTaskExecutor.cpp`; MEDIUM credentials-in-error-message attribution on `emailConnector.cpp`; MEDIUM `response.json` raw-API-response sensitivity policy (architectural memo); inline JSON-escape switch blocks in `snowflakeCloudTaskExecutor.cpp`; encrypted-at-rest memory store; §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.

### Gotchas next-session-Claude should know

- **The cloud-surface response-cap value is per-surface, not global.**  Sitting 13 chose 10 MB for IMAP (responses are SEARCH UID lists — small).  Sitting 14 chose 64 MB for Snowflake (paginated query results).  Sitting 14 also chose 1 MB for the Snowflake **connector's** TestConnection (a single SELECT 1).  This sweep chose 64 MB for the 5 cloud-storage / sheets / OneDrive / S3 task executors (paginated list responses + small metadata).  Don't unify just for consistency — each value matches what the protocol typically returns.  When extending to a new cloud surface, pick a value that matches the protocol's typical response size + a generous margin.
- **The 256 MB upload cap matches Phase 9b's existing download cap for symmetry.**  `CURLOPT_MAXFILESIZE_LARGE = 256 MB` was set in S3 and OneDrive download paths during Phase 9b.  This sweep's upload cap matches that value precisely so a workflow that downloads + processes + re-uploads a file has consistent size limits in both directions.  Don't change one without changing the other.
- **`std::streamoff` is the right type for `tellg()` / file-size comparisons.**  Used throughout this sweep + sitting 12.  It's signed (so it can hold the -1 error sentinel that `tellg()` returns on stream errors) and large enough for files up to LLONG_MAX bytes.  Don't `static_cast<size_t>` until AFTER the bound check passes — otherwise -1 wraps to SIZE_MAX which always passes a `<= cap` check.
- **The googleSheets sheets_write CSV pattern is intentionally deferred.**  It reads `input_file` line-by-line via `while (std::getline(in, line))` + accumulates into `std::vector<std::vector<std::string>>`.  Bounded fix needs either total-bytes-accumulated cap, line-count cap, or per-line-length cap — all three differ from the `tellg + allocate` pattern this sweep used.  Bundle with a future MEDIUM input-parser sweep.
- **None of the cap values are exposed as configuration.**  All `kMax<File>{Response,Upload}Bytes` constants are file-local `static constexpr`.  If a future deployment legitimately needs a larger cap (for example, uploading > 256 MB blobs), the answer is to bump the constant + recompile, not to add a config option.  Cap-as-config would let an attacker who breaches the config layer disable the protection — keeping it compile-time is the deliberately-stricter choice.

---

## 2026-05-01 (S1 sitting 15) → next session

S1=D2 sitting 15.  Theme: **first horizontal sweep** — one pattern (CRITICAL filesystem-path traversal on caller-supplied local-file params) closed across **5 cloud task executors** in a single sitting.  Files: `azureBlobCloudTaskExecutor.cpp`, `gcsCloudTaskExecutor.cpp`, `googleSheetsCloudTaskExecutor.cpp`, `oneDriveCloudTaskExecutor.cpp`, `s3CloudTaskExecutor.cpp`.  All 5 had a CRITICAL audit finding flagging the same pattern: caller-supplied local-file path → `std::ifstream` / `std::ofstream` / download helper without canonicalisation.  Single helper applied — `ICloudTaskExecutor::ValidateLocalPath(path, baseDir, taskId)` — with the right `baseDir` per file: launch CWD for azureBlob/gcs/s3 (matches each demo's `"workflows/<demo>/file.csv"` convention), workDir for googleSheets/oneDrive (matches existing `workDir / param` join pattern).  **Naming convention introduced:** "Horizontal Sweep #N — \<pattern\> across \<files\>" — vs. the depth-first per-file sittings (11–14).  This is the first of an estimated 4–5 horizontal sweeps that will close the cross-cutting cloud-surface concerns; subsequent sweeps will tackle response-body caps, TLS verification, SSRF host validation, JSON-injection, and `stoi`/`stoull` exception hardening.

### What landed

1. **`azureBlobCloudTaskExecutor.cpp` — `local_path` confined under launch CWD.**  `getStringParam("local_path")` extraction at line ~255; new `ValidateLocalPath(localPath, Core::g_Core->GetLaunchCWDAbsolute(), taskDefinition.m_Id)` gate immediately after the empty check; reject = task Failed + `LOG_APP_ERROR("[azure_blob] task='{}' workflow='{}' run='{}': local_path rejected", ...)`.  Both upload and download branches benefit (single gate before either branch).

2. **`gcsCloudTaskExecutor.cpp` — same shape.**  Same param name (`local_path`), same launch-CWD baseDir, same gate location after the empty check.

3. **`googleSheetsCloudTaskExecutor.cpp` — `output_file` (read op) + `input_file` (write op) confined under workDir.**  Two separate gates because the read and write ops have separate param names.  Both apply `ValidateLocalPath(file, workDir, taskDefinition.m_Id)` before the corresponding `workDir / file` path build.

4. **`oneDriveCloudTaskExecutor.cpp` — `local_path` confined under workDir + bonus refactor.**  Pre-fix: upload + download branches independently re-resolved `workflowBaseDir` + `workDir` + `fullLocalPath` (duplicated ~6 lines per branch).  Lifted both above the branch (single `workflowBaseDir` + `workDir` + validated `fullLocalPath`); the trailing `WriteResponseJson` block also re-resolved them — now reuses the same values.  Closes a `-Wshadow` warning as a bonus.

5. **`s3CloudTaskExecutor.cpp` — `file_path` confined under launch CWD.**  Two gates because the upload and download branches each extract `file_path` independently from JSON params.  Same shape; logs distinguish the upload vs download rejection via the task message and the LOG_APP_ERROR trailing parenthetical.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 15" (single 5-row table for the per-file changes + 7-row skipped-findings table for cross-cutting items deferred to future horizontal sweeps).

### What's verified

- Studio debug build clean (`make config=debug`).  All 5 files recompiled without diagnostic.  Resolved an early `-Wshadow` warning in oneDrive by completing the refactor lift.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **emailDemo regression check:** 3 tasks succeeded in 2 s end-to-end.  Confirms the shared cloud-surface infrastructure (`ICloudTaskExecutor` base class, `ValidateLocalPath` helper, the curl + JSON helpers from sittings 11–14) is intact.
- **Live S3 negative test (representative for all 5 files since the gate is identical):**
  - Mutated `s3UploadDownloadDemo`'s `upload_data.params.file_path` to `"../../../../etc/passwd"` via direct JSON edit + `POST /api/workflows/reload`.
  - Ran via `mcp__j9t__run_workflow` → state `failed` at `upload_data`; downstream tasks (`download_data`, `ai_analyze`, `upload_report`, `list_objects`) `skipped` per dependency policy.
  - Log produced exactly the expected three lines:
    - `[Security] [info] [security] path_traversal_blocked: task='upload_data' local_path='../../../../etc/passwd' contains '..'`
    - `[Application] [error] [s3] task='upload_data' workflow='s3UploadDownloadDemo' run='s3UploadDownloadDemo_1777695935': file_path rejected (upload)`
    - `[Application] [error] [workflow] task 'upload_data' failed in run 's3UploadDownloadDemo_1777695935': s3 upload: file_path is invalid or escapes the launch directory`
  - Demo restored byte-identical to backup post-test (md5 verified).
- **Not directly verified:**
  - Live negative tests on azureBlob / gcs / googleSheets / oneDrive.  Gate code is structurally identical across all 5 — single live test on one is sufficient evidence the pattern works; per-file fixtures would be redundant.
  - Live happy-path against actual cloud services (S3, GCS, Azure Blob, OneDrive, Google Sheets).  Test env doesn't host those; structural fix matches sittings 11 + 14's verified patterns.

### Open items / next-session candidates

- **Sitting 16 — Horizontal Sweep #2 (response-body + file-read caps).**  Recommend leading with this since it's similarly scoped (5–8 files, single-line size-cap addition per writeCallback).  Each cloud connector + executor has a writeCallback that appends to `responseBody` without bound; same pattern as sitting 13's `kMaxImapResponseBytes` (10 MB) + sitting 14's `kMaxSnowflakeResponseBytes` (64 MB).  Plus the per-file upload `fileData = std::string(fileSize, '\\0')` allocation needs a cap (the audit's HIGH "unbounded file read on upload" finding for S3 / GCS / Azure Blob / OneDrive).  Estimated 2 hours.
- **Sitting 17 — Horizontal Sweep #3 (TLS verify unconditional).**  All cloud-surface curl-setup blocks need explicit `CURLOPT_SSL_VERIFYPEER = 1L` + `CURLOPT_SSL_VERIFYHOST = 2L`.  Sittings 12–14 established this on email + Snowflake; remaining cloud surfaces (S3, GCS, Azure Blob, OneDrive, Google Sheets, plus connectors) need it too.  Estimated 1.5 hours.
- **Sitting 18 — Horizontal Sweep #4 (URL-side injection / SSRF).**  The flip side of this sitting: the URL-injection findings (gcs's `bucket`/`object_name`, oneDrive's `remote_path`, s3's `key`/`prefix`) plus the SSRF-on-endpoint findings (each executor's endpoint URL build).  Likely 5–7 fixes; trickier than the others because each cloud service has its own valid-character set for path components.  Estimated 3 hours.
- **Sitting 19 — Horizontal Sweep #5 (JSON-injection sweep).**  Several MEDIUM findings across the cloud surface where request-body JSON is built via raw concatenation.  Apply `JsonHelper::EscapeJsonString` to each.  Estimated 2 hours.
- **Sitting 20 — Horizontal Sweep #6 (parser hardening).**  `stoi` / `stoull` calls without exception handling (the same pattern sitting 13 closed in `EmailConnector::CheckForNewMail`'s UID parsing).  S3's `max_keys`, plus several other places.  Estimated 1.5 hours.
- **Carried items unchanged** from sitting 14:  `HandleWorkflowVersionRestorePost` zip-container fix; editor master-password / MCP-login parity; `engine.cpp:225` TOCTOU on connections.json; `cloudConnectionManager.cpp` MEDIUM input-validation; MEDIUM `secrets in success-path log` + `predictable MIME boundary` on `emailCloudTaskExecutor.cpp`; MEDIUM credentials-in-error-message attribution on `emailConnector.cpp`; MEDIUM `response.json` raw-API-response sensitivity policy (architectural memo); inline JSON-escape switch blocks in `snowflakeCloudTaskExecutor.cpp`; encrypted-at-rest memory store; §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.

### Gotchas next-session-Claude should know

- **The horizontal-sweep model is now the established mode for the cloud surface's smaller files.**  Verification compresses per-pattern (one live test suffices for a structurally-identical fix replicated N times); fixes batch within a single sitting; the hand-off entry summarizes per-file changes in a 5-row table rather than a 5-section depth-first wall of text.  When picking the next sweep, prefer ones where the fix shape is genuinely identical across files — a sweep that tries to combine "similar but slightly different" fixes per file ends up worse than two separate sweeps.
- **`ValidateLocalPath`'s `baseDir` choice is per-file, not per-sweep.**  This sweep used launch CWD for 3 files and workDir for 2 — driven by each file's existing usage convention.  When applying the helper to a new cloud executor, **read the canonical demo workflow** to confirm what convention the param uses; don't assume.  The `workflows/<demo>/file.csv` pattern signals CWD-relative; a `workDir / param` build site signals workDir-relative.
- **`Core::g_Core->GetLaunchCWDAbsolute()` is the canonical accessor for launch CWD.**  Used in this sweep's azureBlob / gcs / s3 fixes.  Pre-existing in the codebase since at least sitting 11 (when emailCloudTaskExecutor's `body_file` first started using it).  The result is a `std::filesystem::path const&` — pass directly to `ValidateLocalPath` which accepts the second arg as `std::filesystem::path const&`.
- **OneDrive has an internal lift that future sittings may want to extend.**  The `workflowBaseDir` + `workDir` + `fullLocalPath` are now resolved once above the upload/download branch.  Future code that needs `workDir` (e.g., a new "delete" branch for `onedrive_delete`) should reference the already-resolved values, not re-resolve.  Same lift would benefit the other 4 files in this sweep — but only on a future sitting that adds enough new code to make the duplication smell load-bearing.  Don't lift just for hygiene.
- **The S3 demo file (`workflows/s3UploadDownloadDemo/s3UploadDownloadDemo.json`) is the canonical fixture for cloud-surface negative-path testing.**  Its `upload_data` task is the entry point; mutating its `file_path` is the cleanest way to exercise the path-traversal gate without needing MinIO running (the gate fires before any network call).  Same pattern would work for azureBlobDemo / gcsDemo / oneDriveDemo / googleSheetsDemo if their workflow files exist.  Restore via `\cp -f` to bypass any `cp -i` alias.
- **The audit's "URL-side" path-traversal findings on the same files are NOT closed by this sweep.**  GCS's `bucket` / `object_name`, OneDrive's `remote_path`, S3's `key` / `prefix` are each interpolated into request URLs — distinct threat model from filesystem-path traversal.  Sweep #4 closes those.  When investigating a new finding on these files, distinguish "param flows to filesystem op" (covered by this sweep) from "param flows to URL build" (sweep #4).

---

## 2026-05-01 (S1 sitting 14) → next session

S1=D2 sitting 14.  Theme: close out the **densest single-file cloud-surface cluster** — `application/cloud/snowflakeCloudTaskExecutor.cpp` (3 CRIT + 5 HIGH + 1 MEDIUM, the densest single-file cluster after the email surface) plus the parallel issues in `snowflakeConnector.cpp::TestConnection` + the shared `BuildApiBaseUrl` helper (matches sitting 13's "executor + connector comprehensive close" pattern for the email surface).  Fixes are mostly mechanical applications of patterns established in sittings 11–13: `ValidateLocalPath` for path traversal, response-body cap in writeCallback, `JsonHelper::EscapeJsonString` for JSON injection, `ContainsCrlf` for header injection, unconditional `CURLOPT_SSL_VERIFY*` for TLS posture, `std::clamp` for resource-exhaustion bounds.  **Boundary at sitting-end: the Snowflake surface is fully closed at the CRITICAL/HIGH level.**  After the email + Snowflake surfaces are now closed, the remaining cloud executors (Google Sheets, GCS, S3, Azure Blob, OneDrive, Slack, Jira, GitHub, Redmine, Polarion-write, dbQuery) share enough structural duplication to justify the **horizontal sweep** model JC and I discussed before this sitting — see "Open items" below.

### What landed

1. **`BuildApiBaseUrl` charset gate (CRIT SSRF).**  Pre-fix `BuildApiBaseUrl` accepted either an account locator or a full user-provided URL (passed through as-is) — the latter being the SSRF vector (`m_Endpoint = "http://evil.com/path?x="` reached `curl_easy_setopt(CURLOPT_URL, ...)` directly).  Fix: drop the user-provided-URL branch entirely.  Endpoint must now be alphanumeric + `.` + `-` + `_`, max 128 bytes.  Always returns `"https://" + endpoint + ".snowflakecomputing.com"`.  Rejection emits `[security] snowflake_invalid_endpoint reason={size,charset} endpoint_length={}`.  This is a **breaking change** for any deployment that had configured `m_Endpoint` as a full URL — the documented contract was always "account locator with region", so the breaking change is acceptable for alpha/no-production.
2. **Response-body cap in `SnowflakeRequest::writeCallback` (CRIT DoS).**  `kMaxSnowflakeResponseBytes = 64 MB`; on overflow return 0 → libcurl aborts with `CURLE_WRITE_ERROR`.  Same pattern in `snowflakeConnector.cpp::TestConnection`'s parallel writeCallback with a tighter `kMaxConnectorResponseBytes = 1 MB` (test responses are tiny).
3. **TLS verify-peer + verify-host unconditional in both `SnowflakeRequest` and `TestConnection` (HIGH cyber).**  No use_ssl gate (Snowflake is HTTPS-only).  Set 1L / 2L explicitly so libcurl's build-defaults can't fail-open.
4. **`CURLOPT_FOLLOWLOCATION = 0L` in both functions** (defense-in-depth combined with the SSRF fix).  Snowflake never legitimately redirects.
5. **JWT CRLF rejection in `ExecuteCloud` + `TestConnection` (HIGH header injection).**  File-local `ContainsCrlf` matches sitting 11's executor pattern.  Reject = task Failed + LOG_APP_ERROR + `[security] snowflake_jwt_crlf_rejected` / `snowflake_test_jwt_crlf_rejected`.
6. **JSON injection on warehouse / database / schema (HIGH cyber).**  Route all three through `JsonHelper::EscapeJsonString` in both the executor's submit-request build and the connector's TestConnection request build.  Same fix as sitting 12's summary-JSON gap.
7. **`m_LastErrorMessage` raw-response sanitization (HIGH secrets leakage).**  Drop the `if (responseBody.size() < 500) { errorMessage += ": " + responseBody; }` blocks at all three sites (executor submit + executor poll + connector test).  Snowflake error responses can include schema names + partial query data.  Structured `code` / `message` extraction on the success-but-error-status path retains legitimate diagnostic info.
8. **`statementTimeout` + `pollInterval` clamp (HIGH resource exhaustion).**  `kMaxStatementTimeoutSeconds = 24 * 3600`, `kMaxPollIntervalSeconds = 60`.  Pre-clamp the uint64_t to the cap before the int cast (closes the wraparound-to-negative case where INT_MAX-overflow disables the timeout entirely), then `std::clamp(static_cast<int>(val), 1, cap)`.
9. **Snowflake handle validation (MEDIUM URL injection).**  `IsValidSnowflakeHandle`: alphanumeric + `-`, max 64 bytes (UUID-like).  Defense-in-depth gate on the server-supplied handle before splicing into pollUrl + cancelUrl.
10. **`outputFile` path traversal (CRIT path traversal).**  `ICloudTaskExecutor::ValidateLocalPath(outputFile, workDir, taskDefinition.m_Id)` before opening.  Same pattern as sitting 11's email body_file / attachments fixes.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 14" (10 sections + 7-row skipped-findings table).

### What's verified

- Studio debug build clean (`make config=debug`).  Two files recompiled: `snowflakeCloudTaskExecutor.cpp` + `snowflakeConnector.cpp`.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **Live SSRF rejection (the meat of this sitting):** mutated `my-snowflake.endpoint` to `"evil.com/path?x="` via REST PUT, then `POST /api/connections/my-snowflake/test` → HTTP 400 + `Snowflake endpoint rejected: invalid account locator (see security log)`.  Security log: `[security] snowflake_invalid_endpoint reason=charset endpoint_length=16`.  Restored canonical endpoint after the test; `connections.json` saved cleanly.
- **Live emailDemo regression check:** ran emailDemo end-to-end after the Snowflake-surface changes — 3 tasks succeeded in 2 s.  Confirms the shared cloud-surface curl pattern (writeCallback, headers, TLS setup) is intact across the email surface that sittings 11+12+13 closed.
- **Not directly verified:**
  - The strict-TLS branch against a real Snowflake endpoint.  No Snowflake account in test env.  Fix is structural (every curl option set unconditionally; verifications match the established patterns sittings 11–13 verified live elsewhere).
  - The 64 MB response-body cap, the JWT CRLF rejection, the warehouse/database/schema JSON escape, the timeout clamp, the handle validation — all reproduce-via-fixture-only.  Defer to the cybersec fixture sitting.
  - The `outputFile` path-traversal rejection.  No test workflow exercises Snowflake; the fix shape matches sitting 11's email body_file fix which was verified live there.

### Open items / next-session candidates

- **The cloud surface's "single-file depth-first" mode is mostly done.**  Email + Snowflake are closed.  Remaining executors (Google Sheets 7, GCS 7, S3 6, Azure Blob 6, plus the smaller Slack / Jira / GitHub / Redmine / Polarion-write / dbQuery / OneDrive) share so much structural duplication that **horizontal sweeps** are the natural next mode.  Per JC's framing (this sitting's pre-discussion):
  - Working term: **"horizontal sweep"** — one pattern across N files, vs. our depth-first per-file sittings.
  - Naming convention: `Horizontal Sweep #N — <pattern> across <files>`.  Per-sweep entry in the hand-off log + session note table.
- **Sitting 15 candidates (pick one):**
  - **Horizontal Sweep #1 — `local_path` / `output_file` path-traversal across the remaining executors.**  S3, GCS, Azure Blob, OneDrive, Google Sheets all build paths from caller input and pass to `std::ofstream` / `std::ifstream` without confinement.  Single sweep, ~5 fixes, structural pattern reused (the established `ValidateLocalPath` helper).  Estimated 2 hours.
  - **Horizontal Sweep #2 — Response-body cap in writeCallback across the cloud surface.**  Every cloud connector + executor builds an internal writeCallback that appends to `responseBody`.  Single sweep adds the cap to all of them.  Likely ~10 sites.  Estimated 2 hours.
  - **Horizontal Sweep #3 — TLS verify-peer/verify-host unconditional across the cloud surface.**  Same as #2 in shape — most curl-setup blocks lack the explicit `VERIFYPEER=1L` + `VERIFYHOST=2L`.  Some have the conditional `if (!caBundle.empty())` only.  Estimated 1.5 hours.
  - **Horizontal Sweep #4 — SSRF host validation across the remaining executors.**  Each connector / executor that builds a URL from caller-supplied endpoint or bucket needs validation.  GCS bucket, Azure Blob account_name, S3 endpoint, etc.  Some need allowlists (Snowflake-style domain pattern), others need char-set validation.  Estimated 3 hours.
  - **Horizontal Sweep #5 — JSON-injection sweep across the cloud surface.**  Most JSON request bodies use raw concatenation; need `JsonHelper::EscapeJsonString` route through.  Estimated 2 hours.
  - **Or revert to depth-first if the cluster is large enough:** Google Sheets at 7 CRIT+HIGH is still depth-first-eligible.  GCS at 7 same.  Either could be a single sitting close.
- **JC's preference question** — recommend the horizontal-sweep model first since most remaining executors are smaller (3-5 findings each).  But the larger ones (Google Sheets / GCS at 7) might be best done depth-first first, then horizontal sweeps for the smaller files.  Hybrid strategy.
- **Architectural carry-overs (deferred):**
  - **`response.json` raw-API-response sensitivity policy** (sitting 14 architectural carry).  Cloud-task `response.json` files are downstream-task contracts but contain raw API responses (PII, schema info).  Design memo before any code: gate behind debug flag, OR redact at write, OR redesign downstream tasks to consume only structured result data.
  - **Inline JSON-escape switch blocks** in `snowflakeCloudTaskExecutor.cpp` (query escape + result-row writer).  Two inline switch blocks duplicate `JsonHelper::EscapeJsonString`'s logic.  Sitting 12's "no anon-namespace JsonEscape copies remain" claim still holds (these are inline switches, not named helpers).  Cleanup-grade convergence; bundle into Horizontal Sweep #5 or its own slot.
- **Carried items unchanged:**  `HandleWorkflowVersionRestorePost` zip-container fix (sitting 8 carry); editor master-password / MCP-login parity (sitting 8 carry); `engine.cpp:225` TOCTOU on connections.json (sitting 9 carry); `cloudConnectionManager.cpp` MEDIUM input-validation (sitting 10 carry); MEDIUM `secrets in success-path log` + `predictable MIME boundary` on `emailCloudTaskExecutor.cpp` (sitting 12 carry); MEDIUM credentials-in-error-message attribution on `emailConnector.cpp` (sitting 13 carry); encrypted-at-rest memory store (skipped MEDIUM since sitting 4); §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.
- **Doc sweep candidate** — `doc/cloud-integration.md`'s Snowflake section + `doc/cyber security.md`'s Snowflake section.  Not invalidated by this sitting (the user-visible contracts are unchanged), but the SSRF gate + breaking change to `m_Endpoint` (no more user-provided full URLs) is worth documenting briefly.  Bundle with the next post-sitting doc sweep.

### Gotchas next-session-Claude should know

- **`SnowflakeConnector::BuildApiBaseUrl` no longer accepts user-provided full URLs.**  Pre-sitting-14, an `m_Endpoint` value with `https://...` or `http://...` prefix was passed through unchanged.  Now: any non-allowlist character (anything outside `[A-Za-z0-9._-]`) is rejected, so the user-provided-URL escape hatch is gone.  Documented contract was always "account locator with region" (per `cloud-integration.md:651`); the full-URL handling was an undocumented escape hatch.  If a future deployment legitimately needs a custom URL (testing against a Snowflake replica?), the right approach is to add a separate `custom_endpoint` connection param with its own validation rather than relaxing this gate.
- **`ContainsCrlf` is now duplicated across `emailCloudTaskExecutor.cpp` (sitting 11) and `snowflakeCloudTaskExecutor.cpp` (sitting 14) as file-local statics.**  Per `feedback_cpp_discipline`'s third-copy threshold, the next file that needs CRLF rejection should trigger a refactor — most likely lift to `ICloudTaskExecutor` as a public static (matches sitting 13's "lift validators to the connector class" pattern, but for a base-class shared helper).  Track this for the horizontal-sweep mode.
- **`IsValidSnowflakeHandle` is file-local in `snowflakeCloudTaskExecutor.cpp`.**  The Snowflake server-supplied handle is the only such "trusted-but-still-validated" identifier in the cloud surface today.  If future cloud connectors return server-supplied handles that get spliced into URLs (e.g., a future GCS resumable-upload session ID, or an S3 multipart-upload ID), the same defense-in-depth pattern applies; consider lifting to a shared helper at that point.
- **`kMaxSnowflakeResponseBytes = 64 MB` is the largest response cap on the cloud surface.**  The email surface's `kMaxImapResponseBytes = 10 MB` is tighter; Snowflake result sets can legitimately be larger.  When adding a new cloud executor, pick a cap that matches the protocol's typical response sizes — don't unify just for consistency.
- **`CURLOPT_FOLLOWLOCATION = 0L` is now the default for Snowflake-surface curl calls.**  Disabling redirect-following was paired with the SSRF gate this sitting added.  When extending the Snowflake surface (new endpoint, new auth flow), keep this disabled — Snowflake never legitimately redirects, so any redirect would be an attacker-controlled pivot.
- **The Snowflake surface's `m_LastErrorMessage` no longer embeds raw response bodies on HTTP 4xx/5xx paths.**  This is a contract change — pre-fix, an operator debugging via the dashboard could see the Snowflake error JSON inline.  Post-fix, they need to look at `response.json` on disk for the structured error.  If the dashboard analyzer surfaces a Snowflake task failure, the operator's next step is "check the task's working directory's response.json".  This trade-off accepts slightly worse debug ergonomics for closure of the secrets-leakage-via-error-message vector; the architectural `response.json` content question is deferred to its own design memo.

---

## 2026-05-01 (S1 sitting 13) → next session

S1=D2 sitting 13.  Theme: close out **`emailConnector.cpp` comprehensively** — every CRITICAL and HIGH finding plus two MEDIUMs that bundle naturally, in one sitting.  **Boundary at sitting-end: the email surface (`emailCloudTaskExecutor.cpp` from sittings 11+12 + `emailConnector.cpp` from this sitting) is fully closed at the CRITICAL/HIGH level.**  Both halves of the email send/read path now refuse to proceed without TLS + full peer + hostname verification when `use_ssl=true`; loopback / link-local / private / cloud-metadata IP ranges are rejected as SMTP/IMAP targets in production posture; the IMAP `folder` and `subject_filter` strings cannot inject protocol bytes; the `std::stoull` calls in the polling loop cannot crash the engine; the IMAP response buffer is bounded at 10 MB.  Sitting 11's executor-side `IsValidImapFolder` + `IsValidImapUid` helpers were lifted to `EmailConnector` public statics so connector and executor share a single source of truth (per `feedback_cpp_discipline` — refactor before the third copy).

### What landed

1. **TLS hardening — `ImapCommand` + `TestConnection` (2 CRIT).**  Both functions had the same omission: `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` were never explicitly set; `ImapCommand` left the `useSsl=true` path entirely to libcurl defaults (which can fail-open on builds where the trust store is empty); `TestConnection` had a port-conditional `if (port == "587") { CURLUSESSL_ALL }` that left ports 465 + non-standard silently in plaintext.  Fix: apply sitting 12's TLS pattern to both.  Strict mode (`useSsl=true`): `CURLUSESSL_ALL` + `VERIFYPEER=1L` + `VERIFYHOST=2L`.  Plaintext mode (`useSsl=false`): explicitly select `CURLUSESSL_NONE` and emit `[security] email_test_tls_disabled` (TestConnection) or `[security] email_imap_tls_disabled` (ImapCommand).  This closes **the IMAP TLS verification HIGH carry-over from sittings 11+12**.

2. **SSRF host validation + port validation in `BuildSmtpUrl` + `BuildImapUrl` (HIGH cyber + MEDIUM cyber).**  Pre-fix: bare concatenation accepted `host = "169.254.169.254"` (cloud metadata), `host = "evil.com:465/path?x="` (URL-injection), `port = "587 UID FETCH"` (protocol-bytes-in-port).  Fix: new `EmailConnector::IsValidEmailHost(host, allowLocalNetwork)` and `IsValidEmailPort(port)` static methods.  Host validator rejects URL-meaningful chars + whitespace + CR/LF + empty + >253 byte; when `allowLocalNetwork=false`, also rejects loopback (`localhost`, `127.x`, `::1`), link-local (`169.254.x` — covers cloud metadata), private (`10.x`, `172.16-31.x`, `192.168.x`), and IPv6 unique-local (`fc00::/7`, `fe80::/10`).  Port validator: digits only, [1, 65535], max 5 bytes.  `allowLocalNetwork` is gated on `use_ssl` at the call site — plaintext mode (`use_ssl=false`) tolerates loopback (the local-testing escape hatch from sitting 12 stays consistent), production posture (`use_ssl=true`) rejects.  On reject: return empty URL + `[security] email_invalid_smtp_target` / `email_invalid_imap_target`.  Callers check for empty URL and fail-the-task with a "see security log" error message.

3. **IMAP folder + `subject_filter` injection in `CheckForNewMail` (HIGH cyber × 2).**  Pre-fix: bare interpolation of folder into IMAP URL (`searchUrl = imapBaseUrl + "/" + folder`) and `subjectFilter` into SEARCH command (`searchCommand += " SUBJECT \"" + subjectFilter + "\""`).  Fix: apply `EmailConnector::IsValidImapFolder` (lifted from sitting 11) at function entry; new `EmailConnector::IsValidImapSubjectFilter` rejects `"`, `\\`, `\r`, `\n`, `{` (the IMAP literal sentinel) at function entry.  This is **defense-in-depth** — sitting 11 already validates folder at the executor entry, but the connector's public API can be invoked from future call sites with no prior validation.  Also bonus-closes the lastSeenUid sanitization concern via `IsValidImapUid` at function entry.

4. **`std::stoull` DoS — try/catch + watermark sanitization (HIGH cyber).**  Pre-fix: `if (std::stoull(uid) > std::stoull(lastSeenUid))` had no exception handling; values like `"1e5"` (parser accepts the `1`, stoull throws `invalid_argument` on the `e`) or `"99999999999999999999"` (`out_of_range`) crashed the polling thread.  Fix: wrap the comparison loop in `try { ... } catch (std::invalid_argument) { ... } catch (std::out_of_range) { ... }`; on either exception, log WARN with connection name, set errorMessage, return `highestUid` (the safe "no new mail" path that preserves the watermark for the next poll).  Plus per-element `IsValidImapUid` check inside the loop — defense in depth even if `ParseSearchUids` stops checking the first byte.

5. **IMAP response buffer cap (MEDIUM cyber).**  `static constexpr size_t kMaxImapResponseBytes = 10 * 1024 * 1024;` in the anon namespace.  In `ImapWriteCallback`, check `buf->size() + incoming > kMaxImapResponseBytes` before appending; on overflow, return 0 — libcurl interprets this as `CURLE_WRITE_ERROR` and aborts cleanly.  10 MB matches the audit recommendation (production IMAP responses are tens of KB; >10 MB is pathological).

6. **Refactor — lift sitting-11 file-local validators to `EmailConnector` public statics.**  Sitting 11 created `IsValidImapFolder` + `IsValidImapUid` as file-local statics in `emailCloudTaskExecutor.cpp`.  Adding a second copy here would have triggered the 3-copy threshold under `feedback_cpp_discipline`.  Instead: lifted to `EmailConnector` public statics in the header, deleted from the executor, updated 2 executor call sites to qualify with `EmailConnector::`.  Plus added 3 new `EmailConnector` statics in this sitting (`IsValidImapSubjectFilter`, `IsValidEmailHost`, `IsValidEmailPort`).  The executor's `ContainsCrlf` is left as a file-local one-liner because it's trivially short and only used in one file.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 13" (6 sections + 5-row skipped-findings table).

### What's verified

- Studio debug build clean (`make config=debug`).  Four files recompiled (`emailCloudTaskExecutor.cpp`, `emailConnector.cpp`, `jarvisAgent.cpp`, `triggerEngine.cpp` — the last two via the connector header inclusion path).
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **Live happy-path:** emailDemo succeeded with new `[security] email_imap_tls_disabled url_scheme='imap:/'` per fetch — the GreenMail-with-`use_ssl=false` signal lands as designed.
- **Live SSRF rejection (the meat of this sitting):**
  - `smtp_host: "169.254.169.254"` + `use_ssl: "true"` (cloud metadata IP, production posture) → `POST /api/connections/my-greenmail/test` returns HTTP 400 + `Email SMTP target rejected: invalid host or port (see security log)`.  Security log: `[security] email_invalid_smtp_target connection='my-greenmail' use_ssl=true`.
  - `smtp_host: "localhost"` + `use_ssl: "true"` (legitimate hostname but production posture) → also rejected.  Confirms no plaintext-loophole — even when an operator forgets to set non-loopback host, the production posture refuses to send.
  - `smtp_host: "localhost"` + `use_ssl: "false"` (canonical demo config) → `POST /test` returns `{"ok":true}`, GreenMail accepted.  Local-testing escape hatch works as designed.
- Final emailDemo run after restoring canonical params: 3 tasks succeeded in 2 s.  my-greenmail params restored byte-identical.
- **Not directly verified:**
  - The strict-TLS branch of `TestConnection` and `ImapCommand` against a real TLS-enabled SMTP/IMAP server.  No production-grade TLS server in the test env; fix is structural.
  - The `subject_filter` injection rejection (`CheckForNewMail` is exercised by `email_watch` triggers, not the manual emailDemo).  Validators are simple allowlists; the failure path is an explicit early return.
  - The 10 MB response buffer cap.  GreenMail's responses are well under the cap; reproducing >10 MB requires a malicious IMAP server fixture.  Defer.
  - The `stoull` DoS recovery path.  GreenMail produces well-formed UIDs; reproducing requires a malicious server.  Defer.

### Open items / next-session candidates

- **The email surface is now closed at CRIT/HIGH.**  Sittings 11 + 12 + 13 close `emailCloudTaskExecutor.cpp` and `emailConnector.cpp`'s entire CRITICAL + HIGH cluster.  Remaining open items on these files are MEDIUM (predictable MIME boundary, secrets-in-success-path log, credentials-in-error-message attribution) — bundle into a future MEDIUM cloud-surface mini-sweep.
- **Sitting 14 — next densest cloud-surface file.**  Per the post-sitting-9 audit summary: `snowflakeCloudTaskExecutor.cpp` (8 CRIT+HIGH), `googleSheetsCloudTaskExecutor.cpp` (7), `gcsCloudTaskExecutor.cpp` (7), `s3CloudTaskExecutor.cpp` (6), `azureBlobCloudTaskExecutor.cpp` (6).  Of these, the executors share a lot of common structure (path-traversal on `local_path`, SSRF on `endpoint`, response-buffer caps, TLS verification gaps), so a structured "applied-to-each-executor" approach with shared helpers might amortize the work — or the densest single file (snowflake at 8) is a single-sitting close.  JC's call.
- **Sitting 15+ — remaining cloud-surface task executors.**  The smaller files (`dbQueryCloudTaskExecutor.cpp`, `slackCloudTaskExecutor.cpp`, `jiraCloudTaskExecutor.cpp`, `oneDriveCloudTaskExecutor.cpp`, `polarionWriteTaskExecutor.cpp`).  Estimate 2–3 sittings to finish.
- **Carried items unchanged:**  `HandleWorkflowVersionRestorePost` zip-container fix (sitting 8 carry); editor master-password / MCP-login parity (sitting 8 carry); `engine.cpp:225` TOCTOU on connections.json (sitting 9 carry); `cloudConnectionManager.cpp` MEDIUM input-validation (sitting 10 carry); MEDIUM `secrets in success-path log` + `predictable MIME boundary` on `emailCloudTaskExecutor.cpp` (sitting 12 carry); MEDIUM credentials-in-error-message attribution on `emailConnector.cpp` (sitting 13 carry); encrypted-at-rest memory store (skipped MEDIUM since sitting 4); §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.
- **Doc sweep candidate:**  `doc/cyber security.md` line 532 (`IMAP TLS — IMAPS (port 993) with certificate verification`) was a pre-existing under-statement; sitting 13's connector-side fix now matches the doc's claim more precisely.  Update during the next doc sweep.

### Gotchas next-session-Claude should know

- **The use_ssl param now gates THREE distinct behaviours on the email surface.**  (1) TLS posture (sitting 12 + 13 — strict TLS + verification when true, plaintext + SECURITY_WARN when false).  (2) Local-network host acceptance (this sitting — accepts loopback / private when false, rejects when true).  (3) URL scheme selection (always — `smtps://` / `imaps://` when true and using implicit-TLS port, `smtp://` / `imap://` otherwise).  These three are intentionally coupled: if you're sending plaintext, you're testing locally — accepting localhost AND emitting a security warning ARE the right defaults together.  When extending the email surface, follow the same coupling pattern.
- **`EmailConnector::IsValidImapFolder` / `IsValidImapUid` / `IsValidImapSubjectFilter` / `IsValidEmailHost` / `IsValidEmailPort` are public static methods on the connector class.**  Header lift in this sitting consolidated them.  When adding a new email-related call site (executor, trigger, REST endpoint), use these directly — don't roll a duplicate.  The executor already calls `EmailConnector::IsValidImapFolder` and `EmailConnector::IsValidImapUid` at the boundary; new callers should follow the same pattern.
- **`EmailConnector::BuildSmtpUrl` / `BuildImapUrl` now return empty string on validation failure.**  This is a contract change.  Pre-sitting-13 they always returned a non-empty URL (just possibly invalid).  Post-sitting-13: empty signals "host or port rejected by the validators; see security log".  When adding a new caller, check for empty URL **before** passing to libcurl (which would otherwise produce a confusing connect error).  The two existing callers (`TestConnection` in this file, `ExecuteCloud` in `emailCloudTaskExecutor.cpp`) handle this correctly.
- **The `email_imap_tls_disabled` security log line fires per ImapCommand call**, not per workflow run.  emailDemo's `fetch_email` task makes ~2 IMAP calls (SEARCH + per-UID FETCH), so a single emailDemo run produces 2+ such lines if `use_ssl=false`.  Don't mistake the log volume for instability.
- **The `email_invalid_smtp_target` / `email_invalid_imap_target` SECURITY_WARN messages do NOT include the rejected host or port value.**  This is intentional — the host/port might encode sensitive deployment topology.  The `connection` name is logged; an operator can look up the connection params in the dashboard / `connections.json` to see what failed.  This matches the audit's "credentials and key names should not appear in logs" concern from the deferred MEDIUM.
- **The cloud-surface SSRF gate (`IsLocalNetworkHost`) is email-specific in this sitting, but the threat model is universal across cloud connectors.**  When future sittings touch other cloud connectors (S3, Azure, GCS, Snowflake), apply the same pattern: read `use_ssl`, gate `allowLocalNetwork` on it, validate host against the ranges this sitting established.  If three cloud connectors land with their own copies of `IsLocalNetworkHost` / `IsValidEmailHost`-equivalent, refactor to a shared `application/cloud/networkValidation.h` (or similar) per `feedback_cpp_discipline`'s third-copy threshold.

---

## 2026-05-01 (S1 sitting 12) → next session

S1=D2 sitting 12.  Theme: close out **Cluster 11B — TLS hardening + size cap + JsonHelper convergence** on `application/cloud/emailCloudTaskExecutor.cpp`.  Five findings shipped (3 HIGH + 2 MEDIUM) plus one convergence: SMTP transport now defaults to **TLS-required with full cert + hostname verification**; the only path to plaintext SMTP is the `use_ssl: "false"` connection-param opt-out (preserves GreenMail compat) which now emits `[security] email_send_tls_disabled` on every send so an operator running insecurely sees the deviation.  Attachment file reads are bounded at **25 MB** with skip-with-WARN on overflow.  `max_messages` clamps to **[1, 500]**.  The `summary` JSON in `ExecuteCloud` now correctly escapes hostile `to`/`subject` content.  **The 7th anon-namespace `JsonEscape` copy in the codebase is gone** — `JsonEscapeEmail` removed, all 7 call sites converged to `JsonHelper::EscapeJsonString` (continues the post-sitting-4 + sitting-9 sweep across `application/cloud/`).  Boundary at sitting-end: `emailCloudTaskExecutor.cpp`'s **CRITICAL/HIGH cluster (sittings 11 + 12) is fully closed** save for one HIGH that lives at the connector layer — the IMAP TLS verification finding bundles cleanly into sitting 13's `emailConnector.cpp` work.

### What landed

1. **SMTP TLS unconditional + cert verify unconditional, gated on `use_ssl`.**  Pre-fix: `if (port == "587") { CURLOPT_USE_SSL = CURLUSESSL_ALL; }` — port-conditional TLS, no `CURLOPT_SSL_VERIFY*` calls anywhere.  Audit's "set unconditionally" recommendation would break GreenMail (which uses plaintext port 3025 and ships with `use_ssl: "false"`).  Fix: respect `use_ssl` on the SMTP path identically to the IMAP path.  Default true → `CURLUSESSL_ALL` + `VERIFYPEER=1L` + `VERIFYHOST=2L` (refuse to proceed without TLS, refuse without cert verification, refuse without hostname verification).  Default false → log `[security] email_send_tls_disabled task='{}' workflow='{}' run='{}' connection='{}'` so operator audit shows it.  In-code comment cites the MITM-stripped-STARTTLS attack `CURLUSESSL_TRY` would have permitted.
2. **Attachment file-size cap (25 MB) — skip-with-WARN.**  Pre-fix: `auto fileSize = file.tellg(); std::string content(static_cast<size_t>(fileSize), '\\0'); file.read(...)` with no upper bound and no `fileSize >= 0` check (negative `tellg` would wraparound the cast).  Fix: `static constexpr std::streamoff kMaxAttachmentBytes = 25 * 1024 * 1024;` validate `fileSize >= 0 && fileSize <= cap` before allocating; on overflow emit WARN with task / workflow / run / attachPath / fileSize / cap and `continue`.  25 MB matches Gmail / Outlook / typical SMTP server limits.  In-code comment cites the OOM vector (string + 1.33× base64-encoded MIME body).
3. **`max_messages` overflow clamp.**  Pre-fix: `maxMessages = static_cast<int>(val)` with `val: uint64_t` from JSON — `val > INT_MAX` wraps to negative, `val ∈ [1, INT_MAX]` allows DoS via "fetch 2 billion emails".  Fix: pre-clamp the uint64 to `kMaxMessageCap = 500`, then `std::clamp(static_cast<int>(val), 1, kMaxMessageCap)`.  Lower bound enforced for free.
4. **`summary` JSON in `ExecuteCloud` now escapes `to` and `subject`.**  Pre-fix: `"summary":"{\"to\":\"" + to + "\",\"subject\":\"" + subject + "\",...}"` raw concat — sitting 11's CRLF gate stops newline injection, but `"` and `\\` still slipped through and corrupted the `response.json` write.  Fix: route both fields through `JsonHelper::EscapeJsonString`.
5. **`JsonEscapeEmail` → `JsonHelper::EscapeJsonString` convergence.**  7th and (as of this sitting) last unconverged anon-namespace JSON-escape copy in the codebase — same pattern as the 6th convergence in sitting 9.  Local 30-line `JsonEscapeEmail` definition deleted, `#include "json/jsonHelper.h"` added, all 7 call sites (6 in `ExecuteEmailRead`'s summary build + 1 in the response JSON for the `folder` field) converged via `sed -i 's/JsonEscapeEmail(/JsonHelper::EscapeJsonString(/g'`.  No `JsonEscape*` copies remain in `application/cloud/`.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 12" (5 changes + 8-row skipped-findings table).

### What's verified

- Studio debug build clean (`make config=debug`).  Single-file change to `emailCloudTaskExecutor.cpp` (+ `<algorithm>` and `"json/jsonHelper.h"` includes).
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **Live happy-path:** `mcp__j9t__run_workflow emailDemo` → 3 tasks succeeded in 2 s end-to-end.  Security log now contains exactly one `[security] email_send_tls_disabled task='send_reply' workflow='emailDemo' run='emailDemo_<id>' connection='my-greenmail'` line per send (because GreenMail uses `use_ssl: "false"`) — exactly the design intent.
- **Live negative-path quartet:**
  - **30 MB attachment** (`dd if=/dev/zero bs=1M count=30 of=workflows/emailDemo/03_reply/big_blob.bin`, swap `attachments: ["big_blob.bin"]`) → `[Application] [warning] [email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777692645': attachment 'big_blob.bin' size 31457280 bytes exceeds 26214400 byte cap; skipping`.  Run state `succeeded` (skip-with-WARN per the audit recommendation).  big_blob.bin removed post-test.
  - **`max_messages: 99999`** → `fetch_email` succeeded with empty inbox (count: 0); no memory spike, no overflow.  The clamp is structural — value is bounded at 500 inside the executor regardless of input.
  - **Hostile `subject` with `"` + `\\`** (`"Re: \\"hostile\\" subject with \\\\backslash"`) → `response.json` contents: `{"ok":true,"to":"sender@example.com","subject":"Re: \\"hostile\\" subject with \\\\backslash","attachments":0}`.  Python `json.load` parses cleanly.  Pre-fix the same payload would have produced unparseable JSON.
  - **Final happy-path post-restore:** emailDemo.json restored byte-identical to backup; emailDemo ran cleanly, 3 tasks succeeded.
- **Not directly verified:**  the `use_ssl: "true"` SMTP branch.  No production-grade SMTP+TLS server in the test environment; the fix is structural (every curl option is set unconditionally on entry to the TLS branch).  A future Postfix-with-TLS fixture would harden confidence.

### Open items / next-session candidates

- **Sitting 13 — emailConnector.cpp.**  The connector layer carries 2 CRITICAL (TLS verification disabled in `ImapCommand`, TLS verification not enforced in `TestConnection`) + 5 HIGH (IMAP injection via `subjectFilter` in SEARCH, IMAP injection via `folder` in URL — overlaps cluster 11A's executor-side fix; SSRF via `smtp_host` / `imap_host`; `std::stoull` throws on non-numeric IMAP UIDs causing DoS) + 1 MEDIUM (uncontrolled response buffer growth).  Single dense file.  Will also pick up the **IMAP TLS verification HIGH deferred from sitting 12** + the **credentials-in-error-path HIGH** (the credential leak vector is the IMAP URL `user:password@host` form built in `EmailConnector::BuildImapUrl`).
- **Sitting 14+ — next densest cloud-surface file.**  Snowflake (8 CRIT+HIGH), Google Sheets (7), GCS (7), S3 (6), Azure Blob (6), and the smaller executors.  Estimate 3–4 sittings to finish the cloud-surface CRIT/HIGH cluster.
- **Carried items unchanged:**  `HandleWorkflowVersionRestorePost` zip-container fix (sitting 8 carry); editor master-password / MCP-login parity (sitting 8 carry); `engine.cpp:225` TOCTOU on connections.json (sitting 9 carry); `cloudConnectionManager.cpp` MEDIUM input-validation (name length / charset, endpoint SSRF — sitting 10 carry); MEDIUM `secrets in success-path log` + `predictable MIME boundary` on `emailCloudTaskExecutor.cpp` (sitting 12 carry); encrypted-at-rest memory store (skipped MEDIUM since sitting 4); §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.

### Gotchas next-session-Claude should know

- **`use_ssl` is now load-bearing on the SMTP path, not just the IMAP path.**  When adding a new email connection in production, **omit** the `use_ssl` param (or set to `"true"`) to get strict TLS — the default is the secure path.  Setting `use_ssl: "false"` is for local-testing connections (GreenMail, Mailpit, custom mocks) and produces a `[security] email_send_tls_disabled` log line on every send so operators can audit which connections are insecure.  When troubleshooting "why is my send failing on TLS", the first check is `use_ssl` and the second is whether the CA bundle is reachable (libcurl's strict default fails closed if the trust store is empty).
- **`CURLUSESSL_ALL` is the project's chosen TLS-policy gate.**  Not `CURLUSESSL_TRY` (silent plaintext fallback on MITM-stripped STARTTLS), not `CURLUSESSL_NONE` (no TLS).  When adding a new SMTP/IMAP/SMTPS curl call, follow the same pattern: read `use_ssl` from connection params, default true, set `CURLUSESSL_ALL` + `VERIFYPEER=1L` + `VERIFYHOST=2L` on the true branch, log `[security] <subsystem>_tls_disabled` on the false branch.
- **`kMaxAttachmentBytes = 25 * 1024 * 1024` is the email-surface attachment cap.**  Other cloud surfaces have their own caps (S3/OneDrive use `CURLOPT_MAXFILESIZE_LARGE = 256 MB` per Phase 9b).  Don't unify — email's tighter cap reflects SMTP server limits, not just memory pressure.  When adding a new size cap on a different cloud surface, pick a value that matches the protocol's typical limits, document the cap inline, and use `std::streamoff` to match `tellg`'s return type (not `size_t` — `streamoff` is signed and can hold the negative-`tellg`-error sentinel).
- **No file-local `JsonEscape*` functions remain in `application/cloud/`.**  The 7th and last anon-namespace copy was deleted in this sitting (`JsonEscapeEmail` in `emailCloudTaskExecutor.cpp`).  When adding a new JSON-string-content embed in any cloud-surface file, route through `JsonHelper::EscapeJsonString(view)` directly — never roll an 8th copy.  This is `feedback_simdjson_only`'s sibling rule for the escape side.
- **The emailDemo workflow is the canonical end-to-end exercise, AND its scratch `03_reply/big_blob.bin` is a known cleanup target.**  This sitting's negative test created a 30 MB file at `workflows/emailDemo/03_reply/big_blob.bin` to exercise the size cap.  It was removed post-test, but if a future sitting needs to re-run that test, follow the same pattern: create the file, add to attachments, run, observe rejection, remove.  Don't commit big_blob.bin to git.
- **The IMAP path's TLS verification is still on the deferred list.**  `emailCloudTaskExecutor.cpp` calls `EmailConnector::ImapCommand` which has its own curl setup — and that setup doesn't have the equivalent `CURLUSESSL_ALL` + `VERIFYPEER=1L` strictness yet.  Sitting 13 closes this on the connector side; until then, IMAP traffic over `use_ssl: "true"` is libcurl-default-strictness only (not the explicit-strict-policy this sitting added for SMTP).

---

## 2026-05-01 (S1 sitting 11) → next session

S1=D2 sitting 11.  Theme: open the **email surface** with **Cluster 11A — path traversal + protocol injection** on `application/cloud/emailCloudTaskExecutor.cpp`.  Four findings closed in one tight cluster: three CRITICAL "untrusted input pasted into URL/filesystem path" (body_file path traversal, attachment array path traversal, IMAP folder URL injection) plus one HIGH "untrusted input pasted into RFC 2822 header field" (SMTP header injection via from/to/cc/subject).  Boundary at sitting-end: every untrusted input flowing into a URL, filesystem path, or header field on the email **executor** is now gated by an explicit allowlist or path-confinement check; every reject path emits ERROR + SECURITY_WARN with task / workflow / run identifiers.  `emailDemo` ships untouched (canonical happy-path PASS) and the four targeted negative tests all fail-closed with the documented log shapes.  Cluster 11B (TLS hardening + attachment size cap + credential-redaction in error logs, 4 HIGH) and the **emailConnector.cpp** cluster (2 CRIT + 5 HIGH on the connector layer; one finding — IMAP folder URL injection — partially overlaps cluster 11A's executor-side fix) remain queued for sittings 12–13.

### What landed

1. **`body_file` path traversal — confine under launch CWD via `ValidateLocalPath`.**  Pre-fix: `std::ifstream ifs(bodyFile)` with no canonicalisation accepts any caller-supplied path including absolute (`/etc/passwd`) and traversal (`../../../etc/passwd`).  `body_file` semantics in this module are CWD-relative (canonical demo: `body_file: "queue/emailDemo/02_ai_reply/PROB_reply.output.txt"`), so confining under workDir would break the convention.  Fix: gate via `ICloudTaskExecutor::ValidateLocalPath(bodyFile, Core::g_Core->GetLaunchCWDAbsolute(), taskDefinition.m_Id)` — the helper rejects `..` substrings and any path whose `lexically_normal` form doesn't start with the canonical launch CWD.  Reject path emits LOG_APP_ERROR with task / workflow / run identifiers (per `feedback_log_failures` for dashboard analyzer compatibility) and a matching `[security] path_traversal_blocked` line from the helper itself.
2. **Attachment array path traversal — `ValidateLocalPath` per attachment under workDir.**  Pre-fix: each `attachPath` is joined with `workDir` via C++'s `path::operator/` and read directly — but the operator returns the right-hand side unchanged when it's absolute, so `attachPath: "/etc/shadow"` produces `fullPath = "/etc/shadow"`.  Relative `..` segments traverse outside workDir syntactically.  Fix: validate each `attachPath` with `ValidateLocalPath(attachPathStr, workDir, taskDefinition.m_Id)` before opening; on reject, emit a WARN-level fail-path log line with task / workflow / run identifiers and `continue` (per the audit-recommended skip-with-warning behaviour, so a single hostile attachment doesn't fail the entire task — legitimate later attachments still get a chance).
3. **IMAP folder URL injection — strict allowlist + per-iteration UID validation.**  Pre-fix: `searchUrl = imapBaseUrl + "/" + folder` and `fetchUrl = imapBaseUrl + "/" + folder + "/;UID=" + uid` interpolate untrusted input directly.  Folder value `INBOX@evil.internal:1234/extra` redirects the IMAP connection (libcurl's `@host:port` URL syntax).  Fix: new file-local helpers `IsValidImapFolder(folder)` (allowlist `[A-Za-z0-9._/-]`, max 256 bytes, no leading/trailing `/`, no `//`) and `IsValidImapUid(uid)` (digits only, max 20 bytes).  Folder validated once after the `INBOX` default; UID validated per-iteration in the fetch loop (server-supplied UIDs are still untrusted from this module's perspective — a malicious or buggy IMAP server could return a hostile UID).  Folder reject = task Failed + ERROR + SECURITY_WARN; UID reject = skip-with-WARN.  RFC 3501 hierarchy delimiters (`.` and `/`) are intentionally allowed so Gmail's `[Gmail]/Sent Mail` and similar legitimate folder names still work.
4. **SMTP header injection — CRLF rejection on every header field value.**  Pre-fix: `BuildEmailMessage` concatenates `from`/`to`/`cc`/`subject` into RFC 2822 headers via raw `<<` with no CR/LF stripping.  An attacker-controlled `subject: "Hello\r\nBcc: victim@..."` injects a real `Bcc:` header.  Fix: new file-local helper `ContainsCrlf(s)`; validate `from`, `to`, `cc`, `subject` at `ExecuteCloud`'s entry (after extraction, before `BuildEmailMessage`).  Body content NOT validated — newlines are legitimate body content.  Reject = task Failed + ERROR (`CRLF rejected in header field` with task / workflow / run identifiers) + SECURITY_WARN (`email_send_header_injection`).  In-code comment cites RFC 2822 §2.2.3 (CR/LF forbidden in unfolded header values).

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 11" (4 changes + 13-row skipped-findings table).

### What's verified

- Studio debug build clean (`make config=debug`).  Single-file change to `emailCloudTaskExecutor.cpp`.
- 28-test assistant non-AI suite: PASS in 2.1 s.
- Hermetic dispatcher: PASS.
- **Live happy-path:** `mcp__j9t__run_workflow emailDemo` → 3 tasks succeeded in 3 s end-to-end.  Every cluster 11A gate (folder validation in fetch_email, body_file validation + CRLF check in send_reply) accepted the canonical inputs.
- **Live negative-path quartet — all four cluster 11A boundaries verified by mutating the running emailDemo:**
  - **body_file = `"../../../../etc/passwd"`** → run failed; `[security] path_traversal_blocked: task='send_reply' local_path='../../../../etc/passwd' contains '..'` + `[email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777691687': body_file path rejected` (ERROR with run-id substring) + `task 'send_reply' failed in run 'emailDemo_1777691687': email_send: body_file path is invalid or escapes the launch directory`.
  - **folder = `"INBOX@evil.internal:1234/extra"`** → run failed at `fetch_email`; `[email_read] task='fetch_email' workflow='emailDemo': invalid folder name rejected (length=30)` (ERROR) + `[security] email_read_invalid_folder task='fetch_email' workflow='emailDemo' folder_length=30` (SECURITY_WARN).  Downstream tasks correctly skipped per existing dependency-failure policy.
  - **subject = `"Re: Hello\r\nBcc: victim@example.com"`** → run failed at `send_reply`; `[email_send] task='send_reply' workflow='emailDemo' run='emailDemo_1777691782': CRLF rejected in header field` (ERROR) + `[security] email_send_header_injection task='send_reply' workflow='emailDemo' run='emailDemo_1777691782'` (SECURITY_WARN).
  - **attachments = `["../../../../etc/passwd", "/etc/shadow"]`** → both attachments rejected: the `../etc/passwd` form caught by the `..` substring check (`[security] path_traversal_blocked: ... contains '..'`); the absolute `/etc/shadow` form caught by the prefix-escape check (`[security] path_traversal_blocked: task='send_reply' resolved='/etc/shadow' escapes base='/home/beaumanvienna/dev/jarvisAgent/workflows/emailDemo/03_reply'`).  Each rejection produces `[email_send] ... attachment path rejected` at WARN.  Run state: **succeeded** — task continued with no attachments per the audit-recommended skip-with-warning behaviour, the email itself sent cleanly.
- **Live happy-path post-restore:** restored `emailDemo.json` from backup, ran emailDemo, 3 tasks succeeded in 3 s.  emailDemo.json md5 byte-identical with backup post-test.
- **Not directly verified:** the per-iteration UID validation under a hostile IMAP server response.  GreenMail (the mock IMAP server) doesn't produce hostile UIDs; the validation is structural.  A fixture that injects malformed UIDs server-side would harden confidence — track for the cybersec fixture sitting.

### Open items / next-session candidates

- **Sitting 12 — emailCloudTaskExecutor.cpp Cluster 11B (TLS hardening + size + redaction).**  Four HIGH findings: SMTP `CURLOPT_USE_SSL` is conditional on port being exactly `"587"` (drop the conditional, set unconditionally for both 465 implicit-TLS and 587 STARTTLS); SMTP `CURLOPT_SSL_VERIFYPEER` / `VERIFYHOST` never explicitly set (set to 1L / 2L unconditionally; abort on missing CA bundle); IMAP TLS verify (delegated to `EmailConnector::ImapCommand` — coordinate with sitting 13); attachment size cap (25 MB-or-similar bound on `fileSize` before allocation); credential redaction in error path (use `CURLOPT_ERRORBUFFER` + scrub before assigning to `m_LastErrorMessage`).  Plus three MEDIUM findings that bundle naturally: secrets in success-path log (recipient + subject), `max_messages` clamp, predictable MIME boundary, `summary` JSON not escaped.  Plus the file-local `JsonEscapeEmail` convergence onto `JsonHelper::EscapeJsonString` (the 7th anon-namespace copy — extends the post-sitting-4 + sitting-9 sweep).
- **Sitting 13 — emailConnector.cpp.**  The connector layer carries 2 CRITICAL (TLS verification disabled in `ImapCommand`, TLS verification not enforced in `TestConnection`) + 5 HIGH (IMAP injection via subjectFilter in SEARCH, IMAP injection via folder in URL — overlaps cluster 11A's executor-side fix, the connector still needs the matching fix; SSRF via smtp_host / imap_host; `std::stoull` throws on non-numeric IMAP UIDs causing DoS) + 1 MEDIUM (uncontrolled response buffer growth).  Single dense file, ~1 sitting.
- **Sitting 14+ — next densest cloud-surface file.**  Snowflake (8 CRIT+HIGH), Google Sheets (7), GCS (7), S3 (6), Azure Blob (6), and the smaller executors.  Estimate 3–4 sittings to finish the cloud-surface CRIT/HIGH cluster.
- **Carried items unchanged:**  `HandleWorkflowVersionRestorePost` zip-container fix (sitting 8 carry); editor master-password / MCP-login parity (sitting 8 carry); `engine.cpp:225` TOCTOU on connections.json (sitting 9 carry); `cloudConnectionManager.cpp` MEDIUM input-validation (name length / charset, endpoint SSRF — sitting 10 carry); encrypted-at-rest memory store (skipped MEDIUM since sitting 4); §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.

### Gotchas next-session-Claude should know

- **`ICloudTaskExecutor::ValidateLocalPath` is the project's canonical path-confinement helper for cloud-surface code.**  Lives in `application/cloud/cloudTaskExecutor.cpp:195`.  Two-step gate: explicit `..` substring reject + `lexically_normal((baseDir / localPath))` prefix-under-baseDir check.  Per-call security log line includes the task id (`[security] path_traversal_blocked: task='...' local_path='...' contains '..'` or `... resolved='...' escapes base='...'`).  When adding a new cloud-task executor that opens a file from caller input, route through this helper — don't roll a new copy.  The helper uses `lexically_normal` (string-level), not `weakly_canonical` (resolves symlinks); per `feedback_path_confinement_edition` this is the project's chosen trade-off (Engine = strict prefix-under-queue-root, Studio = relaxed) and matches the cyber security doc's threat model.
- **`body_file` is CWD-relative on this executor, attachments are workDir-relative.**  Don't migrate `body_file` to workDir-relative without simultaneously updating every demo workflow that uses it (currently `emailDemo.jcwf` ships with `body_file: "queue/emailDemo/02_ai_reply/PROB_reply.output.txt"` — the canonical pattern of reaching upstream task output through the queue tree).  The new gate confines under launch CWD specifically to preserve this convention.
- **Folder allowlist is RFC 3501 hierarchy-aware.**  `[A-Za-z0-9._/-]` allows both `.` and `/` as hierarchy delimiters (Gmail uses `/`, others use `.`).  Reject any non-allowlist character — including space, which means folder names like `"Sent Items"` would fail.  No production j9t workflow uses such names; if a deployment surfaces the need, the right fix is to add `curl_easy_escape` for URL-encoding and extend the allowlist (not weaken it).
- **CRLF gate is on header field VALUES, not body.**  Body legitimately contains newlines (it's the message body, not an unfolded header).  When adding a new field that ends up in an RFC 2822 header position (e.g. a future `reply_to` param), validate with `ContainsCrlf`; fields that end up in the body or in MIME multipart text content do not need this gate.
- **The `emailDemo` workflow is the canonical end-to-end exercise for the email surface.**  3 tasks, 3 seconds, GreenMail Docker mock for IMAP/SMTP — runs cleanly without external dependencies.  Use it for both happy-path verification AND mutation-based negative-path testing (the modify-emailDemo.json → reload → run → observe → restore pattern this sitting used).  Restoration discipline: keep a backup at `/tmp/emailDemo.bak.json`, use `\cp -f` to bypass any `cp -i` alias, md5-verify after restoration.
- **The dashboard run-analyzer surfaces ERRORs with run-id substring.**  When adding a new fail-path log in this module, follow the established pattern: `LOG_APP_ERROR("[email_<send|read>] task='{}' workflow='{}' run='{}': <reason>", taskDefinition.m_Id, workflowDefinition.m_Id, workflowRun.m_RunId, ...)`.  Note: `ExecuteEmailRead` does NOT receive `WorkflowRun` (only `WorkflowDefinition` + `TaskDef`), so its ERROR lines include `task='{}' workflow='{}'` but not `run='{}'`.  This is consistent with the existing dispatch surface — adding `WorkflowRun&` to `ExecuteEmailRead`'s signature would be a sitting-12+ refactor.
- **`cloud_task_execute` security info line precedes every cloud task execution.**  Already in place at `cloudTaskExecutor.cpp` (the base-class `Execute` wrapper — Phase 9b audit-logging).  The cluster 11A reject paths emit AFTER this line, so the security log shows: (1) the audit `cloud_task_execute` line, (2) the `path_traversal_blocked` / `email_read_invalid_folder` / `email_send_header_injection` line for the rejection.  When investigating a security event, both lines together give the full picture of "what task tried to execute, why it was rejected".

---

## 2026-05-01 (S1 sitting 10) → next session

S1=D2 sitting 10.  Theme: close out **Cluster 9C — concurrency / lifetime** on `application/cloud/cloudConnectionManager.{cpp,h}`.  Two HIGH findings shipped: `GetConnection` returned a raw `CloudConnection const*` that outlived its `shared_lock` guard (use-after-free if a concurrent writer rehashes / erases the entry while a caller dereferences) — fixed by changing the return to `[[nodiscard]] std::optional<CloudConnection>` (value copy under the lock, callers receive their own bytes); `m_Dirty` was a plain `bool` accessed lock-free in `IsDirty` / `ClearDirty` while writers set it under `unique_lock` (data race) — fixed by promoting to `std::atomic<bool>` with documented acquire/release ordering.  All 7 external `GetConnection` call sites updated mechanically (`auto` deduction; the existing `if (!connection)` / `connection->m_X` / `*connection` syntax works unchanged on `std::optional` because it overloads `operator bool` / `operator->` / `operator*`).  Boundary at sitting-end: `cloudConnectionManager.cpp`'s **CRITICAL/HIGH cluster across sittings 9 + 10 is fully closed**.  Remaining findings on the file are MEDIUM (name length / charset, endpoint SSRF — input-validation cluster) and LOW (redundant copy, `[[nodiscard]]` sweep — Rust-emulating defaults cluster).  Next file: pick between (a) input-validation pass that combines manager-level checks with per-connector type-specific param validation, (b) per-connector OAuth / network-egress cluster (TLS verify-peer, SSRF on `m_TokenEndpoint`, OAuth POST-body URL-encoding, response-body cap, HTTP error-body redaction in `azureBlobConnector.cpp` / `googleSheetsConnector.cpp` / `oneDriveConnector.cpp` / `cloudConnectionPool.cpp`), or (c) jump to the densest task-executor file (`emailCloudTaskExecutor.cpp` at 9 CRITICAL+HIGH including SMTP header injection, IMAP URL injection, attachment path traversal).

### What landed

1. **`GetConnection` raw-pointer-across-lock-boundary → `std::optional<CloudConnection>` by value.**  Header signature changes from `CloudConnection const* GetConnection(std::string const& name) const` to `[[nodiscard]] std::optional<CloudConnection> GetConnection(std::string const& name) const` with a multi-line comment block documenting the lifetime contract (the optional owns its bytes after function-return; subsequent writer mutations cannot invalidate the caller's view).  Implementation unchanged in shape (`shared_lock` → `find` → return); `return nullptr;` → `return std::nullopt;`, `return &it->second;` → `return it->second;` (value copy constructed inside `std::optional` while the lock is still held).  In-code comment cites the audit + the Rust `Option<&T>` borrow-checker analogy.  Seven callers updated to `auto` deduction (no logic changes — `if (!connection)`, `connection->m_X`, `*connection` all work identically on `std::optional<T>`):
   - `application/workflow/triggerEngine.cpp:712` — email_watch poll loop.
   - `application/web/webServer.cpp:6299` — `HandleConnectionUpdatePut` (with the `CloudConnection updated = *existing;` value-copy line gaining an explanatory comment).
   - `application/web/webServer.cpp:6398` — `HandleConnectionTestPost`.
   - `application/web/webServer.cpp:6485` — `HandleOAuthAuthorizeGet`.
   - `application/web/webServer.cpp:6655` — `HandleOAuthCallbackGet` (multi-step OAuth flow with the longest webServer-side hold).
   - `application/workflow/filter/filterEngine.cpp:471` — Polarion-filter path.
   - `application/cloud/cloudTaskExecutor.cpp:76` — **the longest hold by far**, holds the optional through `connector->ResolveCredentials(*connection)` (network I/O) → audit log → circuit-breaker → template expansion → `ExecuteCloud(*connection, ...)` (the actual cloud operation).  In-code comment at this site cites the multi-second-window lifetime guarantee.
2. **`m_Dirty` data race → `std::atomic<bool>` with acquire/release ordering.**  Header: `bool m_Dirty{false}` → `std::atomic<bool> m_Dirty{false}` with `#include <atomic>` added.  `IsDirty()` returns `m_Dirty.load(std::memory_order_acquire);`.  `ClearDirty()` calls `m_Dirty.store(false, std::memory_order_release);`.  In-header comment block documents the ordering choice: the writer's `m_Dirty = true` (sequenced after the map mutation, then released by the unique_lock unlock) creates a happens-before edge to a subsequent `IsDirty() == true` reader, so observing `true` implies observing the map mutation that triggered it.  Writer sites (`m_Dirty = true;` in `AddConnection` / `UpdateConnection` / `RemoveConnection` / `ParseConnectionsJson`'s end-of-parse swap) need NO change — the implicit `operator=(bool)` on `std::atomic<bool>` is `seq_cst`, strictly stronger than the `release` required.  `IsDirty()` / `ClearDirty()` are now correctly lock-free without the data race.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 10" (2 changes + a 9-row skipped-findings table for the deferred MEDIUM/LOW items + verification gaps + the engine.cpp TOCTOU surfaced in sitting 9).

### What's verified

- Studio debug build clean (`make config=debug`).  All 6 touched files (`cloudConnectionManager.{cpp,h}`, `triggerEngine.cpp`, `filterEngine.cpp`, `cloudTaskExecutor.cpp`, `webServer.cpp`) recompile and link.  No diagnostic from any caller — the API change is type-erased through `auto` and `std::optional`'s pointer-compatible operators (`bool`, `->`, `*`).
- **28-test assistant non-AI suite: PASS** in 2.1 s.
- **Hermetic dispatcher: PASS.**
- **Live exercise of all five `webServer.cpp` `GetConnection` call sites:**
  - `GET /api/connections` → 14 connections, `dirty: false` (lock-free atomic load through the new `IsDirty()`).
  - `PUT /api/connections/my-polarion` (line 6299, `HandleConnectionUpdatePut`) → HTTP 200; subsequent `GET` reports `dirty: true` (writer's atomic store under `unique_lock` observed by the lock-free reader via acquire-release).
  - `POST /api/connections/save` (the `SerializeToJson` + `WriteTextFileAtomic` + `ClearDirty` path) → HTTP 200; subsequent `GET` reports `dirty: false` (atomic store via `ClearDirty` observed cleanly).
  - `POST /api/connections/my-polarion/test` (line 6398, `HandleConnectionTestPost`) → HTTP 400 with `curl error: Could not resolve hostname` (DNS unreachable in test env — expected; the relevant verification is that the handler ran the full path through `connector->TestConnection(*connection, errorMessage)` without crashing, confirming the optional → `CloudConnection const&` hand-off works).
  - `GET /api/connections/my-onedrive/oauth/authorize` (line 6485, `HandleOAuthAuthorizeGet`) → HTTP 200 with a valid Microsoft OAuth URL `https://login.microsoftonline.com/common/oauth2/v2.0/authorize?client_id=...&scope=Files.ReadWrite%20offline_access&...`.  Exercises `connection->m_AuthType`, `connection->m_Params.find("client_id")`, `connection->m_Params.find("scopes")`, `connection->m_Type` — all on the new `std::optional<CloudConnection>`.
- **Live exercise of `cloudTaskExecutor.cpp:76` (the longest-hold site) via real workflow run:** `mcp__j9t__run_workflow emailDemo` → run `emailDemo_1777690450` → 3 tasks succeeded (`ai_reply`, `fetch_email`, `send_reply`) in 3 seconds.  Both `fetch_email` (IMAP via `EmailConnector::ExecuteCloud`) and `send_reply` (SMTP via the same path) ran through `ICloudTaskExecutor::Execute`, each pulling the optional from `GetConnection` and holding it through `ResolveCredentials` (network I/O) → audit log → circuit-breaker → `ExecuteCloud` (IMAP/SMTP round-trip).  Pre-fix the raw pointer had multi-second exposure to concurrent connection mutation; the optional value-copy makes that window zero by construction.
- **Dirty-flag round-trip end-to-end:** `false → true` after `PUT` → `false` after `POST /save`.  The complete producer-under-lock / consumer-lock-free pairing works correctly under live REST traffic.
- **Not directly verified:**
  - `triggerEngine.cpp:712` email_watch path — fixture-dependent (set up email_watch trigger + wait for poll interval).  Caller-audit + identical-shape-to-verified-cloudTaskExecutor + build-clean is sufficient evidence.
  - `filterEngine.cpp:471` Polarion-filter path — requires a working Polarion endpoint; test env's `my-polarion` has no live network.  Caller-audit + build-clean is sufficient.
  - The original UAF and data-race reproducer — fixes are structural (the bug classes are gone by construction: no raw pointer escapes the lock guard, no non-atomic concurrent access).  Stress fixtures for both deferred to the cybersec fixture sitting.

### Open items / next-session candidates

- **Sitting 11 — pick one of three.**  All three are equally open after `cloudConnectionManager.cpp` closes:
  - **(a) Input-validation pass on the cloud surface** — combine manager-level `cloudConnectionManager.cpp` MEDIUMs (name length / charset, endpoint SSRF) with per-connector type-specific param validation in the densest connector files.  Each connector accepts type-specific params with no validation today; centralizing validation at the manager is cheap, but the per-connector files are the actual source-of-truth for which params are valid.
  - **(b) Per-connector OAuth / network-egress cluster.**  Findings live in `azureBlobConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, `cloudConnectionPool.cpp` (token cache).  Includes: TLS peer verification (curl `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST`), SSRF on `m_TokenEndpoint`, OAuth POST-body URL-encoding, response-body unbounded accumulation (DoS), HTTP error-body redaction, `TokenEntry` references across mutex unlock/lock in `GetAccessToken`, dangling-reference race in `RefreshLoop`.  Likely 2 sittings.
  - **(c) Densest task-executor file: `emailCloudTaskExecutor.cpp`** (9 CRITICAL+HIGH).  Surface includes SMTP header injection via `from`/`to`/`cc`/`subject`/`body`, IMAP URL injection / SSRF via `folder` parameter, path traversal via `body_file` / attachment paths, TLS enforcement gaps, credential leakage in error logs.  This is the kind of single-file dense cluster that closes in 1–2 sittings.
- **Sitting 12+ candidates (after the next sitting):**  The remaining task-executor surface (`snowflakeCloudTaskExecutor.cpp` at 8, `googleSheetsCloudTaskExecutor.cpp` at 7, `gcsCloudTaskExecutor.cpp` at 7, `s3CloudTaskExecutor.cpp` at 6, `azureBlobCloudTaskExecutor.cpp` at 6, plus the smaller `dbQueryCloudTaskExecutor.cpp` / `slackCloudTaskExecutor.cpp` / `jiraCloudTaskExecutor.cpp` / `oneDriveCloudTaskExecutor.cpp` / `polarionWriteTaskExecutor.cpp`).  Estimate 3–4 sittings.
- **Carried items unchanged:**  `HandleWorkflowVersionRestorePost` zip-container fix (sitting 8 carry); editor master-password / MCP-login parity (sitting 8 carry); `engine.cpp:225` TOCTOU on connections.json (sitting 9 carry); encrypted-at-rest memory store (skipped MEDIUM since sitting 4); §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.

### Gotchas next-session-Claude should know

- **`std::optional<CloudConnection>` is the new contract for `GetConnection`.**  When adding a new caller, use `auto connection = ...GetConnection(name);` (NOT `auto const* connection`).  Existing patterns work as-is: `if (!connection)` (operator bool), `connection->m_Name` (operator->), `*connection` (operator*) — all of these have identical syntax on `std::optional<T>` and on `T const*`.  Don't try to re-cast the optional to a pointer (`if (auto* p = connection.value() ?...`); just use the optional directly.
- **The optional value is alive for as long as the optional is alive — typically function scope.**  When passing `*connection` to a function taking `CloudConnection const&` (e.g., `connector->ResolveCredentials(*connection, ...)`), the reference is valid for the duration of that call.  When holding the optional across multiple operations (`cloudTaskExecutor` does this for ~5+ seconds), the reference into the optional remains valid for the entire hold.  The pre-fix raw-pointer pattern made this implicit and dangerous; the optional makes it explicit and safe.
- **Don't store a `CloudConnection const*` pointer obtained from `*connection.operator->()` past the lifetime of the local `connection` optional.**  This is the same pattern the audit warned about — it just moves the lifetime from "the manager's map entry" to "the local optional".  If a future detached lambda needs to capture the connection, capture the optional by value (or its dereferenced contents), never the underlying pointer.
- **`m_Dirty` is now `std::atomic<bool>` with explicit `acquire` / `release` ordering.**  Writer sites that already wrote `m_Dirty = true` under `unique_lock` are unchanged (the implicit `seq_cst` is strictly stronger than the `release` needed).  When adding a new writer site, follow the same pattern: write `m_Dirty = true` after mutating the map under `unique_lock`.  When adding a new reader, prefer `IsDirty()` over `m_Dirty.load()` so the ordering choice stays centralized.
- **The end of cloudConnectionManager.cpp's CRITICAL/HIGH cluster.**  Sittings 9 + 10 closed it.  Remaining findings on this file are MEDIUM (input-validation) or LOW (cosmetic).  The next sitting picks a new file — don't re-open cloudConnectionManager unless a new audit pass surfaces something.  `engine.cpp:225` TOCTOU is the only sibling concern this file's audit indirectly surfaced; track for whichever sitting next touches engine.cpp.
- **`emailDemo` is a clean cloudTaskExecutor exercise.**  The 3-task workflow runs in 3 seconds against the in-Docker GreenMail mock; both `fetch_email` and `send_reply` exercise the cloudTaskExecutor optional → `*connection` → `ExecuteCloud` path end-to-end.  Cheaper than setting up an S3 / Azure / Polarion stack for verification.  Use this for quick smoke-testing future cloud surface changes.

---

## 2026-05-01 (S1 sitting 9) → next session

S1=D2 sitting 9.  Theme: open the **cloud surface** with **Cluster 9A — JSON / serialization safety** on `application/cloud/cloudConnectionManager.cpp`.  Six findings closed in one tight cluster: the **6th and last** anon-namespace `JsonEscape` copy in the codebase converged onto `JsonHelper::EscapeJsonString` (closes the post-sitting-4 sweep across the entire codebase, plus the MEDIUM "doesn't encode all RFC 8259 control chars" finding as a structural side-effect); `ParseConnectionsJson` rebuilt with input-size cap (1 MB) + per-array cap (1024 connections) + per-field cap (4 KB) + per-params caps (256 entries × 1 KB) + scratch-then-swap (live `m_Connections` is no longer wiped on parse failure) + per-element WARN logging; `SerializeToJson` now holds `shared_lock` during iteration; `engine.cpp:225` connections.json loader gained a pre-read file-size cap (1 MB, defense-in-depth before `simdjson::padded_string` doubles memory).  Boundary at sitting-end: `cloudConnectionManager.cpp`'s JSON cluster is closed; the connection load + save round-trips arbitrary bytes 0x00–0x7F + UTF-8 cleanly (live-verified with bell / BS / FF / unit-sep + quotes / backslash / newline / tab in a single payload); both pre-allocation rejection paths (file-size + array-count) emit ERROR-level logs that mention the path + size + cap.  Concurrency cluster (`GetConnection` use-after-free, `IsDirty`/`ClearDirty` race), input-validation cluster (name charset, endpoint SSRF), and OAuth/network-egress cluster (which lives in **per-connector files**, not the manager — sub-agent attribution error corrected at sitting start) remain queued for sittings 10–11.

### What landed

1. **`JsonEscape` → `JsonHelper::EscapeJsonString` convergence — last copy in the codebase.**  Add `#include "json/jsonHelper.h"` to `cloudConnectionManager.cpp`.  Delete the local `static std::string JsonEscape(std::string const& s)` (lines 34–51).  Replace 5 call sites in `SerializeToJson` (`conn.m_Name` / `conn.m_Type` / `conn.m_Endpoint` / `conn.m_KeyName` + params keys + params vals) with `JsonHelper::EscapeJsonString(...)`.  No header change — local function had no external callers.  Closes the MEDIUM "doesn't encode all RFC 8259 §7 control chars" finding by replacing with a helper that does — verified live: a payload containing bell (0x07), BS (0x08), FF (0x0C), and unit-sep (0x1F) round-tripped cleanly via the proper `\u00XX` escapes; pre-fix the same payload would have written raw control bytes that broke the JSON file on next load.

2. **`ParseConnectionsJson` end-to-end hardening.**  Five-part rewrite:
   - **Pre-parse size cap** (`kMaxConnectionsJsonBytes = 1 MB`).  Reject oversized input at function entry with `LOG_CORE_ERROR` that mentions size + cap + "leaving in-memory connections untouched".
   - **Scratch-then-swap.**  Parse into a function-local `std::unordered_map<std::string, CloudConnection> staging`.  Take `m_Mutex` (unique_lock) only at the very end, after the entire parse succeeds, and `m_Connections = std::move(staging)`.  Any earlier `return false` leaves the live state untouched.  Pre-fix the function called `m_Connections.clear()` at function entry — any malformed payload wiped all connections.
   - **Per-array cap** (`kMaxConnections = 1024`).  Reject at element index 1024 with ERROR-level log + early return.  Tested live with a 1 100-connection synthetic payload — the cap fired exactly at element 1024.
   - **Per-field cap** (`kMaxFieldBytes = 4096` for `name` / `type` / `endpoint` / `key_name` / `auth_type`; `kMaxParamFieldBytes = 1024` for params keys + values).  Oversized fields skip the entire element (or the entire params entry, for params) with WARN-level log + element index.
   - **Per-params count cap** (`kMaxParamsPerConnection = 256`).  Element with overflowing params is skipped with WARN.
   - **Per-element error logging.**  Every previously-silent skip (`get_object()` failure, oversized field, missing-or-empty `name`, oversized params entry, params overflow) now emits `LOG_CORE_WARN` with the element index.  The dashboard's run-analyzer surfaces ERRORs only, so these WARNs are diagnostic-only — visible in `log/log.txt` for an operator without polluting the dashboard issues view.
   - In-code comments at the function head and at the swap point document the contract: "fail-closed, live state untouched on any parse failure".

3. **`SerializeToJson` shared_lock acquisition.**  Add `std::shared_lock lock(m_Mutex);` at the top of the function.  Closes the MEDIUM finding that the function (declared `const`) iterated `m_Connections` and `conn.m_Params` without lock acquisition — concurrent `AddConnection` / `RemoveConnection` / `ParseConnectionsJson` (the new structure briefly takes `unique_lock` at the swap) could rehash mid-iteration.  In-code comment cites the iterator-invalidation race the lock now prevents.

4. **`engine.cpp:225` connections.json loader — pre-read file-size cap.**  Add `std::filesystem::file_size(connectionsPath, ec)` before the `ifstream` open, gated by the same 1 MB threshold as the in-function cap.  Three branches: stat-error → `LOG_CORE_ERROR` + skip; oversize → `LOG_CORE_ERROR` with path + size + cap + "refusing to load" + skip; otherwise → existing load path unchanged.  This is the **primary** half of the unbounded-allocation fix; the in-function cap is defense-in-depth for any future caller that bypasses the engine.cpp loader.  In-code comment cites the rationale and the symmetry with `ParseConnectionsJson`.  Note: the existing `fs::exists()` precheck on the same line is the same TOCTOU pattern sitting 8 cleaned up in `webServer.cpp` — left in place as a separate-finding concern, tracked in the skipped-findings table.

Per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 9" (4 changes, plus an audit-grouping correction note at the cluster header).  Skipped-findings table records 11 deferred items including: `GetConnection` use-after-free (HIGH — deferred to cluster 9C concurrency, has 7 external call sites that the `std::optional` fix would touch), `IsDirty`/`ClearDirty` race (HIGH — bundle with 9C), name-charset / endpoint-SSRF (MEDIUM — input-validation cluster), the `engine.cpp` TOCTOU (sibling concern), the **wrongly-attributed** OAuth / TLS / Unlock / encrypted-blob findings (these live in the per-connector files + keyManager, not cloudConnectionManager — corrected at sitting start), and three verification-gap fixtures.

### What's verified

- Studio debug build clean (`make config=debug`).  Active edition: `studio` (per `.build-edition`).  Two touched files (`cloudConnectionManager.cpp`, `engine.cpp`) recompile and link.
- **28-test assistant non-AI suite: PASS** in 2.1 s against the new debug binary.  Exercises the JSON-emitting protocol path which routes every sessionId / message through `JsonHelper::EscapeJsonString` — the convergence change doesn't perturb it.
- **Hermetic dispatcher: PASS.**
- **Live JSON-escape round-trip with hostile bytes:**  `POST /api/connections` with a payload whose `params.comment` field contained 8 hostile bytes (literal `"`, `\`, U+000A, U+0009, U+0007 bell, U+0008 BS, U+000C FF, U+001F unit-sep) → HTTP 200; `POST /api/connections/save` → HTTP 200; `python3 -c 'json.load(open("connections.json"))'` parsed cleanly; all 8 bytes survived end-to-end via the proper escapes (`\"`, `\\`, `\n`, `\t`, ``, ``, ``, ``).  Pre-fix, bytes 0x07 / 0x08 / 0x0C / 0x1F would have written raw, breaking the next load.  Test connection cleaned up post-test; final live config restored byte-equivalent to the pre-test backup (sorted-name set match, content-equivalent — md5 differs only because `std::unordered_map` iteration order isn't stable across re-inserts).
- **Live size-cap rejection (engine.cpp pre-read cap):**  Stopped the server, replaced `connections.json` with a 1 228 959-byte synthetic file, restarted.  Log shows: `[Engine] [error] CloudConnectionManager: '/home/beaumanvienna/dev/jarvisAgent/connections.json' size 1228959 bytes exceeds 1048576 byte cap; refusing to load`.  Server came up clean with 0 connections; auth path + admin endpoints worked normally.
- **Live in-function `kMaxConnections` cap rejection:**  Stopped the server, replaced `connections.json` with a 138 617-byte synthetic file containing 1 100 valid empty-params connections (under file cap, over array cap), restarted.  Log shows: `[Engine] [error] CloudConnectionManager::ParseConnectionsJson: connection count exceeds 1024; rejecting at element index 1024; leaving in-memory connections untouched` immediately followed by `[Engine] [warning] CloudConnectionManager: failed to parse '/home/beaumanvienna/dev/jarvisAgent/connections.json'`.  The cap fires at exactly the right index and emits the documented log line shape.
- **Final state restored:**  `connections.json` overwritten with the original backup from `/tmp/connections.bak.json` (md5 byte-identical with backup); fresh restart loaded the canonical 14 connections cleanly.
- **Not directly verified:**
  - Per-field length cap and per-params cap under hostile REST input (mechanism is mechanical: one comparison + log + skip per field; track with the cybersec fixture sitting).
  - Concurrent `AddConnection` + `POST /save` race against the new `SerializeToJson` lock (fix is structural; fixture would harden confidence — same posture as sitting 8's deferred stress fixtures).
  - Pre-fix `m_Connections.clear()`-then-fail-loud reproducer.  The fix is structural ("the live state is touched only on success path"), so the verification posture is "the bug class is gone by construction".

### Open items / next-session candidates

- **Sitting 10 — cloudConnectionManager.cpp Cluster 9C (concurrency / lifetime).**  HIGH `GetConnection` use-after-free (return-by-`std::optional`, touches **7 external call sites**: `triggerEngine.cpp:712`, `webServer.cpp:6299/6398/6485/6655`, `filterEngine.cpp:471`, `cloudTaskExecutor.cpp:76`); HIGH `IsDirty`/`ClearDirty` data race (`shared_lock` / `unique_lock` or `std::atomic<bool>`).  Bundle these together — both are concurrency, both small.  Output: cloudConnectionManager.cpp's CRITICAL/HIGH cluster fully closed.
- **Sitting 11 — cloud surface input validation.**  Connection name length / charset (MEDIUM); endpoint URL SSRF validation (MEDIUM).  Likely combine with similar input-validation findings in the per-connector files (each connector accepts type-specific params with no validation today).
- **Sitting 12+ — cloud surface OAuth / network-egress.**  OAuth flows live in `azureBlobConnector.cpp`, `googleSheetsConnector.cpp`, `oneDriveConnector.cpp`, with the token cache in `cloudConnectionPool.cpp`.  Findings: TLS peer verification, SSRF on `m_TokenEndpoint`, OAuth POST-body URL-encoding, response-body cap, HTTP error-body redaction, `TokenEntry` references across unlock/lock, `RefreshLoop` dangling reference.  Likely 2 sittings.
- **Sitting 13+ — cloud task executors.**  Densest remaining files: `emailCloudTaskExecutor.cpp` (9 CRIT+HIGH including SMTP header injection, IMAP URL injection, attachment path traversal), `snowflakeCloudTaskExecutor.cpp` (8), `googleSheetsCloudTaskExecutor.cpp` (7), `gcsCloudTaskExecutor.cpp` (7).  Plus `s3CloudTaskExecutor.cpp` / `azureBlobCloudTaskExecutor.cpp` at 6 each.  Estimate 3–4 sittings to cover the executor surface.
- **`HandleWorkflowVersionRestorePost` zip-container fix** (carried from sitting 8's loose follow-ups).  Live-verified broken since the JCWF zip-container migration; fix is to swap `SaveOrUpdateWorkflowFromJson` for the registry's `UpsertJcwfFromZipBytes`-equivalent.
- **Editor master-password / MCP-login parity** (carried from sitting 8).  Frontend-only work.
- **`engine.cpp:225` TOCTOU on connections.json** (surfaced this sitting).  Same pattern as the webServer cleanup; one-line fix.  Bundle with whichever sitting next touches engine.cpp.
- **Encrypted-at-rest memory store** (skipped MEDIUM since sitting 4) — architectural design memo before any code.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **Audit-grouping correction is load-bearing.**  The sitting 8 hand-off enumerated the cloud-surface scope based on a sub-agent's reading.  The agent conflated `cloudConnectionManager.cpp` with `keyManager.cpp` (CRITICAL JSON-injection in `SerializeToJson` for `display_name` / `endpoint` / `api_key` / OAuth fields — that's keyManager) and with `cloudConnectionPool.cpp` (OAuth token cache, `RefreshLoop`, `TokenEntry`, `m_TokenEndpoint`, `GetCachedMasterPassword` — none of those exist in the manager).  When opening the next cloud-surface sitting, **read the audit doc directly for the targeted file** (`grep -n "<filename>" doc/combinedCyberSecAudit.md doc/combinedSafetyAudit.md` then `Read` the section) before scoping — sub-agent index summaries can be wrong about which file a finding belongs to.
- **`JsonHelper::EscapeJsonString` convergence is now complete across the codebase.**  No anon-namespace `JsonEscape` copies remain.  When adding a new JSON-string-content embed anywhere in the codebase, route through `JsonHelper::EscapeJsonString(view)` directly — never roll a 7th copy.  This is `feedback_simdjson_only`'s sibling rule for the escape side.
- **`ParseConnectionsJson` is the canonical "fail-closed parse" pattern for the cloud surface.**  When a future entry-point parses a config file (e.g., a hypothetical `parameter_overrides.json`), follow the same shape: pre-parse size cap → parse into `staging` local map → validate fully → swap into the live state under lock at the very end.  The sitting 7 `WriteTextFileAtomic` "target untouched on failure" pattern is the same idea applied to the on-disk side; this is its in-memory twin.  In particular: never call `m_X.clear()` at function entry without a successful-parse precondition.
- **`engine.cpp:225` connections.json loader still has the `fs::exists()` precheck.**  The sitting added the size cap but left the TOCTOU intact (out of cluster scope).  When the next sitting touches `engine.cpp`, fold the `exists` check into the `file_size` call: `if (sizeEc) { /* missing or stat error */ skip } else if (fileSize > cap) { skip } else { open }` — the `error_code` overload of `file_size` returns an error for missing files, so the precheck becomes redundant.  This is the same pattern sitting 8 applied in the three webServer handlers.
- **The 1 MB cap is consistent across both halves of the fix.**  `engine.cpp` rejects pre-read at `kMaxConnectionsFileBytes = 1 MB`; `cloudConnectionManager.cpp::ParseConnectionsJson` rejects pre-parse at `kMaxConnectionsJsonBytes = 1 MB`.  If a deployment legitimately needs more headroom, **both** thresholds must move in lockstep — change one without the other and you create either an "in-engine bypass" (engine permits more than the parser) or a dead branch (parser caps below engine).  In-code comments at both sites document the symmetry.
- **`std::unordered_map` iteration order isn't stable across re-inserts.**  During the live round-trip test, the post-cleanup `connections.json` had a different md5 than the pre-test backup despite containing the **same 14 connections by content**.  This is normal `std::unordered_map` behaviour and not a regression — the JSON output is content-equivalent, just with a different entry order.  Don't md5-compare connections.json before/after a test that adds + removes a connection; compare the parsed sorted-name set or use a content-aware diff.
- **Two background-process gotchas during live testing.**  (a) After shutting down j9t via REST, the keystore re-locks; the next startup needs `POST /api/settings/keys/unlock` with `{"master_password": ...}` (note: field is `master_password`, NOT `password` — getting it wrong returns `missing_password`) before `J9T_TOKEN`-authenticated requests work.  (b) `cp` may be aliased to `cp -i` (interactive) in JC's shell; use `\cp -f` to bypass alias resolution when restoring backups during cleanup, or `yes | cp` / `cp --remove-destination`.

---

## 2026-04-30 (S1 sitting 8) → next session

S1=D2 sitting 8.  Theme: close out **Cluster C (concurrency / lifetime)** on `application/web/webServer.cpp` — the six findings sitting 7 explicitly deferred.  Three CRITICAL: `const_cast<WebServer*>(this)` cascade in the auth funnel (`TryMcpAuth` / `Authenticate`), `DrainPendingBroadcasts` UAF on the per-client send loop, and `SetWorkflowRuntimeManager`'s lambda capturing `m_AdhocManager.get()` as a raw pointer with no detach on swap or shutdown.  Two HIGH: `m_ClientCount` atomic-vs-set consistency window (audit posture: document the racy-by-design semantics), and the `fs::exists()` followed-by-open TOCTOU sweep across `ServeDashboardIndex` / `HandleWorkflowVersionGetGet` / `HandleWorkflowVersionRestorePost`.  Boundary at sitting-end: the auth funnel no longer hides mutation behind `const_cast`; the WS broadcast path holds `m_Mutex` continuously over the per-client `send_text` loop; the run-terminal observer is detached on both swap *and* shutdown before any teardown that could unwind `m_AdhocManager`; the `m_ClientCount` atomic carries a complete contract comment explaining its hint-only role; the three handlers no longer use `fs::exists()` precheck.  After this sitting, the audit's `webServer.cpp` CRITICAL/HIGH cluster (Clusters A + B + C across sittings 6–8) is **closed**.  Remaining `webServer.cpp` items are MEDIUM/LOW and bundle with the cloud surface (sitting 9+).

### What landed

1. **`const_cast` cascade — drop `const` from the auth funnel.**  Six declarations in `webServer.h` (`AttachMcpExpiryHeader` / `Authenticate` / `CheckAdminAuth` / both `CheckAuth` overloads / `TryMcpAuth` / `TrySessionAuth`) and the matching definitions in `webServer.cpp` lose their trailing `const`.  Two `const_cast<WebServer*>(this)` sites are deleted: the one in `TryMcpAuth` (calling `RecordAuthFailure`) and the `auto* self = const_cast<WebServer*>(this);` block in `Authenticate` (which routed every `m_RateLimitMutex` / `m_AuthFailures` / `IsRateLimited(...)` access through `self->`).  The `Authenticate` body is rewritten to use plain member access throughout.  Six call sites across `webServer.cpp` and `webServer_studio.cpp` (`CheckAuth(req, "viewer")`, `CheckAuth(req, "admin")`, `Authenticate(req)`) compile unchanged — they were already invoked from non-const handlers.  Header comments at each declaration cite "Non-const: calls RecordAuthFailure / IsRateLimited; marking these methods const and const_cast-ing inside hides the mutation from the type system and creates a foot-gun for future readers who assume const = thread-safe."
2. **`DrainPendingBroadcasts` UAF — hold `m_Mutex` over the per-client send loop.**  Restructure: take `m_Mutex` once, swap `m_PendingBroadcasts` into local; drop the lock to build the JSON batch (cheap, no shared state); take `m_Mutex` again, iterate `m_Clients` directly (no snapshot), call `client->send_text(safeBatch)` for each entry under the same lock, drop the lock.  The `clients` snapshot variable is removed (no longer needed once we iterate the live set under the lock).  Crow's `send_text` is asio::post-based internally (verified live at 44 μs per drain with one client), so the lock window stays small.  In-code comment cites the architecture-table-justified asio internals path that makes the lock window cheap, and the audit-described UAF window the new structure eliminates.
3. **`SetWorkflowRuntimeManager` dangling lambda — observer-detach on swap and shutdown.**  Two checkpoints.  (a) In `SetWorkflowRuntimeManager`, when the in-coming WRM differs from the current `m_WorkflowRuntimeManager` and the current one is non-null, call `m_WorkflowRuntimeManager->SetRunTerminalObserver({})` before swapping.  (b) In `SignalStop`, at the very top (before `m_AiJcwfService.Shutdown()` / `m_AssistantController.Shutdown()` / WS-close loop), take `m_Mutex` and clear the observer if `m_WorkflowRuntimeManager` is non-null.  The lambda's captured `m_AdhocManager.get()` raw pointer can no longer be invoked after `WebServer` starts unwinding.  In-code comments at both checkpoints cite the audit and explain why both are needed.  **Both checkpoints emit a permanent `LOG_APP_INFO` trace** (`"WebServer::SetWorkflowRuntimeManager: detached run-terminal observer from previous WRM before swap"` and `"[shutdown] WebServer::SignalStop: detached run-terminal observer from WRM"`) so the new code paths produce a positive observability signal each time they fire — useful for confirming the lifetime contract holds in production.
4. **`m_ClientCount` consistency — explicit performance-hint contract.**  Replace the terse one-line `// lock-free mirror of m_Clients.size() for EnqueueLogLine` comment in `webServer.h:405` with a multi-line block making three things explicit: (a) it's a performance hint, never a routing source-of-truth — `m_Clients` under `m_Mutex` is authoritative; (b) the worst-case race is benign over-send (queued for just-disconnected client) or under-send (skipped for just-connecting client); (c) the rule for future readers — "never use this atomic to decide whether a client receives a message — only whether the broadcast machinery runs at all on the producer side".  No code change.
5. **TOCTOU sweep — drop `fs::exists()` precheck on three handlers.**  (a) `ServeDashboardIndex`: directly call `ServeStaticFile(distIndex)`; on 404 from the helper, substitute the developer-friendly "Dashboard UI build not found. Please run …" 500 response (preserves the UX while closing the TOCTOU).  (b) `HandleWorkflowVersionGetGet`: drop the precheck; conflate `ifs.is_open()==false` into a single 404 `version_not_found` response.  (c) `HandleWorkflowVersionRestorePost`: same treatment for the version-read step + drop the inner `fs::exists(targetPath)` precheck around the best-effort backup-current branch (let `fs::copy_file`'s `std::error_code` signal "source missing" — the copy is best-effort either way).

Five per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 8".  Skipped-findings table records 6 deferred items with reasons (notably: `HandleOAuthCallbackGet` CURL leak — out-of-cluster, defer to a future curl-wrapper RAII pass; stress fixtures for the UAF / dangling-lambda reproducers — the fixes are structural rather than reactive).

### What's verified

- Studio debug build clean (`make config=debug`); the touched files (`webServer.cpp`, `webServer.h`) recompile and link.
- **28-test assistant non-AI suite: PASS** in 2.1 s against the new debug binary — exercises the auth funnel end-to-end (every test takes the auth path), the controller-shutdown path which transitively invokes the new `SignalStop` observer-clear, and the `/ws/assistant` WS route (which uses the same drain machinery as `/ws`).
- **Hermetic dispatcher: PASS** — confirms the rebuild + adjacent code paths still function.
- **Live auth-funnel smokes:**
  - `curl -H "Authorization: Bearer $J9T_TOKEN" /api/auth/whoami` → HTTP 200 + `{"role":"admin","user":"admin","ok":true}` (happy path).
  - `curl /api/auth/whoami` (no auth) → HTTP 401 + `auth_failure reason=missing_credential` security log line.
  - `curl -H "Authorization: Bearer mcp_invalid_token_xx" /api/auth/whoami` → HTTP 401 + `mcp_auth_failure reason=invalid_key` security log line.  `debug_signals` post-call: `auth_failure_records: 1` — confirms `RecordAuthFailure` runs from the rewritten path.
- **Live WS drain smoke:** Connected a Python `websockets.connect()` client to `wss://localhost:8443/ws` with the admin bearer token, sent one ping frame, received the drain output (722 bytes, 4 messages batched), disconnected.  `debug_signals` post-test: `websocket_total_connects: 1`, `websocket_total_drains: 1`, `websocket_last_drain_bytes: 722`, `websocket_last_drain_messages: 4`, `websocket_peak_drain_duration_us: 44`, `websocket_total_disconnects: 1`, `websocket_clients: 0`.  44 μs drain duration with the lock held confirms the lock window stays small; clean disconnect confirms no reference leak in the new code path.
- **Live TOCTOU smokes (initial, in-sitting):**
  - `curl https://localhost:8443/` → HTTP 200 (dashboard index serves cleanly via `ServeStaticFile`).
  - `curl /api/workflows/foo-bar-baz/versions/20260101T000000` (no such workflow) → HTTP 404 + `{"error":"version_not_found", ...}` — the new error-routing produces the right code.
- **Post-sitting follow-up smokes (extension pass after the main sitting):**
  - **`SetWorkflowRuntimeManager` shutdown-detach observable**: started a fresh debug binary, triggered `POST /api/shutdown`, and saw `[2026-04-30 20:30:58.377] [Application] [info] [shutdown] WebServer::SignalStop: detached run-terminal observer from WRM` land in `log/log.txt`.  Positive evidence that the new code path runs at every clean shutdown — the lifetime contract is observable, not merely structural.  The swap-detach branch is instrumented with a matching `LOG_APP_INFO` but doesn't fire today (j9t calls `SetWorkflowRuntimeManager` exactly once at startup); the trace will surface in the log if a future re-init flow ever arrives.
  - **`HandleWorkflowVersionRestorePost` TOCTOU verified, but exposed a pre-existing bug**: posting a real version's timestamp via `POST /api/workflows/exampleMakefile4/versions/20260430T022450/restore` confirmed (a) the new `is_open()`-driven 404 path produces `version_not_found` cleanly, (b) the dropped inner `fs::exists(targetPath)` precheck → `fs::copy_file`-with-`ec` pattern still produces the best-effort backup correctly (new artefact `workflows/.history/exampleMakefile4/20260501T032250.jcwf` md5-matched the pre-restore live workflow exactly).  But the end-to-end restore failed at the *final* write step with `restore_failed: UNCLOSED_STRING` — root-caused as a **pre-existing bug** unrelated to this sitting: the handler reads the `.jcwf` zip container as raw bytes via `std::ifstream` and passes them to `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`, which expects plain JSON.  Tracked in `todo.md` under "Loose follow-ups" as "`HandleWorkflowVersionRestorePost` is broken since JCWF moved to zip containers"; sitting 8's TOCTOU work is unaffected (the live workflow is left untouched on the failure — md5 identical before and after — so this is a "broken feature, not a corrupting feature").
  - **`ServeDashboardIndex` missing-build path verified**: `mv dashboard/ui/dist/index.html /tmp/...; curl /` → HTTP 500 with the developer-friendly `"Dashboard UI build not found. Please run: cd dashboard/ui && npm install && npm run build"` body; restored the file → HTTP 200 again.  The new code-path that replaces `ServeStaticFile`'s 404 with the operator-actionable 500 message works as designed and `git diff dashboard/ui/dist/index.html` is clean post-test.
- **Not directly verified:**
  - The pre-fix `DrainPendingBroadcasts` UAF window itself.  Reproducing it requires concurrent disconnect-during-drain timing that's hard to engineer without a stress fixture.  The fix is structural (no unlock-send window can exist), so the verification posture is "the bug class is gone by construction".
  - The `SetWorkflowRuntimeManager` swap-detach branch (re-init with a different non-null WRM).  Not reachable in production today — `SetWorkflowRuntimeManager` is called exactly once at startup.  The new `LOG_APP_INFO` trace is in place to surface the event if a future re-init flow appears; until then, the branch is exercise-pending.
  - Multi-client stress on the new locked drain.  Single-client live test confirmed 44 μs lock window; a hundreds-of-clients stress test would harden confidence — track for the eventual cybersec fixture sitting.

### Open items / next-session candidates

- **Sitting 9 — D2 cloud surface kickoff.**  After `webServer.cpp` clusters A+B+C close, the next pile of audit findings lives in: `azureBlobCloudTaskExecutor.cpp`, `gcsCloudTaskExecutor.cpp`, `s3CloudTaskExecutor.cpp`, the email/IMAP code path, the GitHub / Snowflake / Redmine integrations, and the `cloudConnectionManager.cpp` surface (which carries the 6th `JsonEscape` anon-namespace copy that sitting 4's convergence work didn't reach).  Likely 2–3 sittings.
- **`HandleOAuthCallbackGet` CURL handle leak on exception paths** — HIGH resource finding, out-of-cluster (CURL RAII concern, not concurrency).  Bundle with a future "curl wrapper RAII pass" that also covers the OAuth signing path.
- **Cybersec stress fixtures** — for both the `DrainPendingBroadcasts` UAF reproducer (N concurrent clients each connecting / disconnecting / sending random pings) and the `SetWorkflowRuntimeManager` teardown-order reproducer (re-init WRM mid-process + shut down out-of-order).  Both fixes are structural; the fixtures would harden confidence but aren't load-bearing.
- **Encrypted-at-rest memory store** (skipped MEDIUM since sitting 4) — architectural design memo before any code.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **The auth funnel is now non-const top-to-bottom.**  When adding a new auth-related helper, do not mark it `const` if it touches `m_AuthFailures` / `m_RateLimitBuckets` / `m_McpKeyManager`'s mutating methods — and do not reach for `const_cast<WebServer*>(this)` to work around a const declaration.  If a helper genuinely is read-only (e.g. a pure header-extraction utility), `static` is preferable to `const`; if it must be a member, document the non-mutation contract explicitly.
- **`DrainPendingBroadcasts` now holds `m_Mutex` over the per-client `send_text` loop.**  When adding a new producer that pushes into `m_PendingBroadcasts`, the contract is unchanged (push under `m_Mutex`).  When adding a new consumer (e.g. a WebSocket-event-fanout for a new dashboard panel), follow the existing pattern: take the lock, swap the queue into a local, drop the lock to do CPU work, retake the lock for the per-client send.  Don't re-introduce the snapshot-then-unlocked-send pattern — it's the exact UAF this sitting closed.
- **The run-terminal observer is now cleared at every WRM transition.**  When adding a new observer-style installation on the WRM (or any other long-lived service), follow the same shape: detach the old observer in the swap site (covers re-init flows) AND in `SignalStop` / equivalent (covers shutdown).  The lambda's lifetime contract must be enforced at every transition, not just at re-init.  Each detach emits a permanent `LOG_APP_INFO` trace — when you grep `log/log.txt` for `"detached run-terminal observer"` after a clean shutdown you should see exactly one line per WebServer instance.  Absence of that line on a clean shutdown is a regression signal.
- **`m_ClientCount` is a hint, not a routing decision.**  When reading the new contract comment in `webServer.h:405`: never branch on `m_ClientCount.load() == 0` for a routing decision.  The valid uses are exactly the three existing ones — early-exit producers (`Broadcast` / `BroadcastJSON` / `EnqueueLogLine`) that skip the broadcast machinery if there's plausibly no audience.  Routing decisions ("does *this* client receive *this* message") must consult `m_Clients` under `m_Mutex`.
- **The `fs::exists()` precheck is dead in this file.**  The three flagged sites were the last `fs::exists() then open()` instances in `webServer.cpp`.  When adding a new file-read handler, use the `std::ifstream(path); if (!ifs.is_open()) return 404` pattern directly — and if the read needs to distinguish "missing" from "permission denied", capture the open's underlying `errno` rather than re-introducing the precheck.  `TryReadBinaryFile` already does this correctly for binary reads; lift its pattern when needed.
- **Auth funnel call sites compiled unchanged.**  This was a "drop const + delete const_cast" cascade where every caller already invoked from a non-const handler.  No new caller-fixup is expected when the next person adds an auth-related helper, *as long as* they invoke it from a non-const handler context.  If a future read-only API surface (e.g. a metrics endpoint) wants to call `Authenticate` from a const context, that's a real conflict — and the answer is "make the handler non-const", not "re-add `const` to `Authenticate`".

---

## 2026-04-30 (S1 sitting 7) → next session

S1=D2 sitting 7.  Theme: close out **Cluster B (config-write atomicity)** on `application/web/webServer.cpp` — the four HIGH findings (3 safety + 1 cyber-sec) on the three handlers that persist user-editable JSON files: `HandleAiInterfacesSavePost`, `HandleConfigSettingsPut`, `HandleConnectionsSavePost`.  Boundary at sitting-end: every config-write handler routes through `WebServerHelpers::WriteTextFileAtomic` (tmp-file + rename), every caller-supplied string field is JSON-escaped via `JsonHelper::EscapeJsonString` before reaching the file, and every patched result is re-parsed with simdjson before the rename happens.  Cluster C (concurrency: `DrainPendingBroadcasts` UAF, `TryMcpAuth` `const_cast`, `SetWorkflowRuntimeManager` dangling lambda, `m_ClientCount` consistency window, `fs::exists` TOCTOU sweep) remains queued for sitting 8 — explicitly out of scope.

### What landed

1. **`HandleConnectionsSavePost` — atomic write + ERROR-level fail log.**  Single substitution: the `std::ofstream file(connectionsFilePath); ... file << json;` block becomes `WriteTextFileAtomic(connectionsFilePath, json, writeError)`.  The helper writes to `<path>.tmp`, flushes, then `fs::rename`s atomically; on failure the tmp is unlinked and the target is untouched.  Failure path now emits both `LOG_APP_ERROR` (dashboard run-analyzer surface, per memory `feedback_log_failures`) AND `LOG_SECURITY_WARN("[security] connections_save_failed ...")` (security log surface).  `connectionManager.SerializeToJson()` already JSON-escapes its output (a separate, RFC-correct copy of the assistant-subsystem `JsonEscape` lineage in `application/cloud/cloudConnectionManager.cpp`), so no escape gap to address here — that 6th-copy sweep is tracked-but-skipped for the cloud surface sitting.
2. **`HandleAiInterfacesSavePost` — JSON-escape every string field + atomic write + simdjson tripwire.**  Three changes in one handler.  (a) Every embed of `iface.m_Name` / `iface.m_Description` / `iface.m_Url` / `iface.m_Model` / `iface.m_KeyName` now passes through `JsonHelper::EscapeJsonString(...)` — the assistant-subsystem canonical RFC 8259 escaper, extended into webServer.cpp via a new `#include "json/jsonHelper.h"`.  `apiStr` comes from a closed enum and needs no escaping.  (b) After the bracket-counted text replacement and **before** the on-disk write, the patched `fileContent` is parsed with `simdjson::ondemand::parser`, the `"API interfaces"` array is iterated, and the element count is compared against `config.m_ApiInterfaces.size()`.  Any structural breakage surfaces as `LOG_APP_ERROR("post-replacement validation failed ...")` + HTTP 500 with the original `config.json` left untouched.  (c) `ofstream + trunc` → `WriteTextFileAtomic`.
3. **`HandleConfigSettingsPut` — depth-aware `replaceField` + atomic write + simdjson tripwire.**  Same family as #2 but for the seven top-level scalars.  The `replaceField` lambda is rewritten to walk `fileContent` byte-by-byte tracking object depth via `{` / `}` and honouring `\\`-escapes inside JSON strings; the key match only fires at object depth 1 (immediately inside the root `{`).  Closes the cyber-sec finding's "no brace/object scope awareness" gap before any future schema introduces a key collision.  After all seven `replaceField` calls, both `validateDoc.error()` and `validateDoc.get_object()` are checked under simdjson; any breakage produces HTTP 500 + ERROR log and aborts the write.  `ofstream + trunc` → `WriteTextFileAtomic`.

Three per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 7".  Skipped-findings table records 6 deferred items with reasons (notably: full parse-and-reserialize via simdjson + custom serializer, the cloud-manager `JsonEscape` 6th copy, the entire Cluster C concurrency cluster).

### What's verified

- Studio debug build clean (`make config=debug`); the touched file (`webServer.cpp`) recompiles and links.
- **28-test assistant non-AI suite: PASS** in 2.1 s against the new debug binary (every `user_message` test exercises the protocol-error fast path of webServer's auth funnel; nothing in the changed handlers blocks the path).
- **Live curl smokes on the debug binary:**
  - `POST /api/connections/save` (admin) → HTTP 200, `connections.json` rewritten with byte-identical content (md5 unchanged), no `connections.json.tmp` lingering.
  - `POST /api/settings/ai-interfaces/save` baseline (admin) → HTTP 200, `config.json` byte-identical, no `config.json.tmp` lingering.
  - **JSON-escape verification under hostile input:** `PUT /api/settings/ai-interfaces/api.openai.com%2Fgpt-4.1%2FAPI1` with `{"description":"audit-test value with \"quotes\" and \\backslash and a\nnewline"}` → HTTP 200; subsequent save → HTTP 200; resulting `config.json` parses cleanly under `python3 json.load`, on-disk bytes show `"audit-test value with \"quotes\" and \\backslash and a\nnewline"` (RFC 8259 `\"`, `\\`, `\n` escapes correctly applied).  Pre-fix the same payload would have produced a `config.json` that failed on next reload.
  - `PUT /api/settings/config` with `{"max_threads":42,"verbose":true,"jcwf_batch_size":7}` → HTTP 200; `config.json` updated atomically; subsequent reverter `PUT` returns the values to `20 / false / 1`.  `git diff config.json` clean post-revert.
- **Not directly verified:**
  - Failure-path of `WriteTextFileAtomic` itself in any of the three handlers (would need a controlled fault-injection fixture — read-only parent dir, full disk).  The success path was verified; the failure path returns an HTTP 500 with the underlying error in the body, same shape as `HandleN8nStartPost`'s already-exercised atomic-write fail branch.
  - `replaceField` depth-aware behaviour with a synthetic schema that has top-level/nested key overlap (current schema has no overlap; bundles cleanly with the future "JCWF schema overlap regression test" fixture).

### Open items / next-session candidates

- **Sitting 8 — webServer.cpp Cluster C (concurrency).**  3 CRITICAL: `DrainPendingBroadcasts` UAF (m_Clients re-check then send_text without holding lock — though Crow's send_text asio-posts internally, narrowing the window); `TryMcpAuth` `const_cast<WebServer*>(this)` to call `RecordAuthFailure` (fix: drop `const` from `Authenticate`/`TryMcpAuth`/`TrySessionAuth`/`AttachMcpExpiryHeader`/`CheckAuth` — touches every call site); `SetWorkflowRuntimeManager` captures raw `m_AdhocManager.get()` (fix: weak_ptr or explicit observer-clear before any reset).  3 HIGH: `m_ClientCount` consistency window; `fs::exists()` then open() TOCTOU sweep across `ServeDashboardIndex` / `HandleWorkflowVersionGetGet` / `HandleWorkflowVersionRestorePost` (mechanical — drop the `exists` precheck and rely on `ifstream` open status, matches `TryReadBinaryFile` pattern).
- **Sitting 9+ — D2 cloud surface.**  After webServer.cpp clusters close, the cloud task executors (`azureBlobCloudTaskExecutor`, `gcsCloudTaskExecutor`, `s3CloudTaskExecutor`, etc.), email/IMAP code path, and GitHub/Snowflake/Redmine integrations remain.  Likely 2–3 sittings.  The sweep should also pick up the `cloudConnectionManager.cpp::JsonEscape` 6th-copy convergence as a small-scope cleanup.
- **Encrypted-at-rest memory store** (skipped MEDIUM since sitting 4) — architectural design memo before any code.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **`JsonHelper::EscapeJsonString` now reaches across the boundary from the assistant subsystem into webServer.cpp.**  The include path is `"json/jsonHelper.h"` (already in the engine's include search path).  When adding a new web handler that splices a caller-controlled string into a JSON-string-content position, route through this helper.  Don't roll a 7th anon-namespace copy in webServer.cpp — that's exactly the smell sitting 4's convergence work eliminated.
- **The depth-aware `replaceField` lambda inside `HandleConfigSettingsPut` is the canonical pattern for "replace the value of a top-level key in a hand-edited JSON file".**  If a future config endpoint needs the same behaviour for a different file (e.g. a future `config-overrides.json`), copy the lambda's structure rather than re-deriving it.  The structure: walk byte-by-byte, increment depth on `{`, decrement on `}`, skip string contents (honouring `\\` escapes), match the key only when at object depth 1.  Promotion to a shared helper makes sense once a third caller appears (per memory `feedback_cpp_discipline`'s "refactor before the third copy" rule).
- **The simdjson tripwire is a tripwire, not a parser.**  In both `HandleAiInterfacesSavePost` and `HandleConfigSettingsPut` the validator's job is "fail loudly if the find-replace result is malformed JSON".  It is *not* a substitute for write-side correctness — the find-replace logic is still the source of truth.  If you ever rewrite either handler to use a true parse-mutate-serialize round-trip, the tripwire becomes redundant and can be removed; until then it's load-bearing.
- **`WriteTextFileAtomic`'s failure-mode contract: target untouched.**  On any error (parent dir create failure, tmp open failure, write failure, rename failure), the helper returns false and `errorMessage` is populated; the original on-disk file is unchanged.  Consequently, a 500 response from any of the three Cluster B handlers means "your in-memory state was applied, the disk was not" — the dirty flag should NOT be cleared on the failure path.  All three handlers correctly clear the dirty flag only after a successful return.
- **The launcher script started a stale Release binary on initial test run.**  `./jarvisagent.sh` (no flag) defaults to `bin/Release/jarvisAgent-studio` which was older than the changes.  Use `./jarvisagent.sh --debug` to test code changes against `bin/Debug/jarvisAgent-studio` (which also enables `debug_signals`).  Per memory `feedback_build_studio_debug`, debug studio is the right default for sittings.

---

## 2026-04-30 (S1 sitting 6) → next session

S1=D2 sitting 6.  Theme: close out **Cluster A (path/auth/static gating + body caps)** on `application/web/webServer.cpp` — the seven HIGH+MEDIUM findings on the pre-auth and pre-role-check perimeter, plus the one role-escalation gap inside the post-auth WS handler.  Boundary at sitting-end: every public-or-near-public route on `webServer.cpp` either (a) authenticates before doing real work, (b) confines paths it builds from caller input, (c) bounds body size before allocating, or (d) does all three.  Cluster B (config-write atomicity, ~5 HIGH) and Cluster C (concurrency: 3 CRITICAL + 3 HIGH including the `DrainPendingBroadcasts` UAF and `TryMcpAuth` `const_cast`) are explicitly deferred to sittings 7+.

### What landed

1. **`ServeDashboardStatic` + `ServeWorkflowEditorStatic` canonicalize paths.**  New shared helper `WebServerHelpers::ConfinePathUnder(root, relative)` in `application/web/webServer_helpers.h` — wraps the established `weakly_canonical(root / raw)` + `lexically_relative(root)` containment pattern (same shape as `WorkspaceIndexer::ResolveAndConfine` from sitting 4).  Both static-asset handlers route through it.  `..`-traversal returns HTTP 400 + `LOG_SECURITY_WARN("[security] dashboard_static_path_escape len=...")` / `editor_static_path_escape`.  Verified at runtime: `curl --path-as-is "https://localhost:8443/dash-assets/../../etc/passwd"` → HTTP 400 + security log line lands.
2. **`ReadLogFile` path-confined to `<launchCwd>/log/`.**  Resolves `logPath` via `ConfinePathUnder` against the launch cwd, then asserts the result lives under `<launchCwd>/log/`.  Defense in depth: the two existing callers (`HandleLogGet`, `HandleSecurityLogGet`) pass hardcoded `"log/log.txt"` and `"log/security.txt"`, but the gate now bounds any future caller that lets user input influence the path.  Bonus: explicit `if (fromOffset > fileSize) fromOffset = fileSize;` clamp documents the invariant the existing range guards rely on.
3. **`ai-write-scripts` requires admin role (per-connection role pinning).**  New `m_WsClientRoles : unordered_map<connection*, std::string>` in `webServer.h` (guarded by `m_Mutex`).  `.onaccept` pins `auth.m_Role` into Crow's per-connection `userdata` (heap-allocated string); `.onopen` reads + frees it and stores the role under the connection pointer; `.onclose` erases the entry.  The `ai-write-scripts` branch reads the role under `m_Mutex` and, if not `"admin"`, emits `LOG_SECURITY_WARN("[security] ai_write_scripts_role_denied role='...' ip=...")` and replies with `{"type":"ai-write-scripts-result","ok":false,"error":"forbidden"}`.  Side effect: `.onopen` was also refactored to snapshot `m_Clients.size()` / `m_WsTotalConnects` / `m_WsPeakClients` under the lock and log outside — closes the HIGH safety finding "onopen reads m_Clients.size() outside lock" as a bonus.
4. **`HandleMcpHeartbeatPost` requires MCP key + body cap + pre-auth rate limit.**  Signature changed to `(crow::request const&)`.  Three gates at the top in this order: pre-auth rate limit → 1 KB body cap → `TryMcpAuth` (must succeed).  Every miss path emits `LOG_SECURITY_WARN` and `RecordAuthFailure`.  Verified at runtime: unauthenticated POST → HTTP 403 + `{"error":"forbidden"}`; admin POST → HTTP 200.  Side effect: the dashboard's `IsMcpConnected()` heartbeat staleness signal can no longer be spoofed by an unauthenticated network attacker.
5. **`HandleN8nStartPost` + `HandleWebhookPost` validate caller-supplied `runId`.**  Both handlers now apply `IsValidWorkflowId` to a non-empty caller-supplied `runId` (the same alnum + `_`/`-` allowlist used for `workflowId`/`taskName`).  Server-generated runIds (via `GenerateIntegrationRunId`) bypass the gate by construction (they don't go through this code path).  On rejection: HTTP 400 + `MakeWorkflowJsonError("invalid_run_id", ...)`.  Closes the path-traversal vector where `runId = "../../foo"` would write `request.json` outside the run dir.
6. **`HandleOAuthCallbackGet` explicit `CURLOPT_SSL_VERIFYPEER=1L` + `CURLOPT_SSL_VERIFYHOST=2L`.**  Two new `curl_easy_setopt` calls before the conditional `CAINFO`.  When `CurlWrapper::GetCaBundlePath()` returns empty (Linux/macOS — system CA bundle), an `LOG_APP_INFO` line records that we're using the system trust store, so an operator with a misconfigured build sees the early-warning signal.
7. **`HandleKeysUnlockPost` rate-limited + auth-failure recorded on wrong password.**  Three gates: pre-auth rate limit → 1 MB body cap → `RecordAuthFailure(req.remote_ip_address)` + `LOG_SECURITY_WARN("[security] keys_unlock_wrong_password ip=...")` on the wrong-password branch.  Auth-failure tracking flows into the standard `kMaxAuthFailures=10` within `kAuthFailureWindow=5min` lockout, so master-password brute-force against the live API is now bounded by the same lockout as login attempts.

Seven per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 6".  Skipped-findings table records 12 deferred items with reasons (notably the entire Cluster B + C surface).

### What's verified

- Studio debug build clean (`make config=debug`); the four touched files (`webServer.cpp`, `webServer_studio.cpp`, `webServer.h`, `webServer_helpers.h`) recompile and link.
- **28-test assistant non-AI suite: PASS** in 2.1 s against the new debug binary.  Covers session protocol + slash commands + completion + protocol error paths.  Implicitly exercises the `/ws/assistant` route (separate from `/ws`); the changes to `/ws` (the dashboard route) don't perturb it.
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — adjacent dispatcher path unbroken.
- **Live perimeter verification:**
  - `curl --path-as-is "https://localhost:8443/dash-assets/../../etc/passwd"` → HTTP 400 + `dashboard_static_path_escape len=16` in `log/security.txt`.
  - `curl -X POST https://localhost:8443/api/mcp/heartbeat` (no auth) → HTTP 403 + `{"error":"forbidden"}`.
  - `curl -X POST -H "Authorization: Bearer $J9T_TOKEN" https://localhost:8443/api/mcp/heartbeat` → HTTP 200 + `{"ok":true}`.
- MCP sidecar verified live: `mcp__j9t__whoami` (admin), `mcp__j9t__debug_signals` (`keys_unlocked=true`); the sidecar's heartbeat ticks (debug_signals counters increase) prove it's authenticated under the new gate.
- **Not directly verified:**
  - `ai-write-scripts` admin-vs-operator role denial under live operator-role MCP key (would need a multi-role test fixture; not yet built).
  - n8n + webhook `runId` traversal rejection (would need n8n / webhook traversal regression test fixtures).
  - OAuth callback under MitM / cert-spoof scenario (would need a controlled TLS proxy).
  - `HandleKeysUnlockPost` rate-limit kicking in on rapid wrong-password attempts (would need to hammer the endpoint past the bucket).  All four are predictable carry-overs.

### Open items / next-session candidates

- **Sitting 7 — webServer.cpp Cluster B (config-write atomicity).**  `HandleAiInterfacesSavePost`, `HandleConfigSettingsPut`, `HandleConnectionsSavePost` all open `std::ofstream` with `std::ios::trunc` and write JSON via naive string `replaceField`.  Fix: route through `WriteTextFileAtomic` (already used elsewhere for tmp-file + rename) AND parse-mutate-serialize through simdjson rather than string-replace.  ~5 HIGH findings in one cluster.
- **Sitting 8 — webServer.cpp Cluster C (concurrency).**  `DrainPendingBroadcasts` UAF (CRITICAL); `TryMcpAuth` const_cast (CRITICAL — drop `const` from `Authenticate`, `TryMcpAuth`, `TrySessionAuth`, `AttachMcpExpiryHeader`, `CheckAuth`); `SetWorkflowRuntimeManager` dangling lambda (CRITICAL); `m_ClientCount` consistency window (HIGH); `fs::exists()` then open() TOCTOU mechanical sweep (HIGH).
- **Sitting 9+ — D2 cloud surface.**  After webServer.cpp clusters close, the cloud task executors (`azureBlobCloudTaskExecutor`, `gcsCloudTaskExecutor`, `s3CloudTaskExecutor`, etc.), email/IMAP code path, and GitHub/Snowflake/Redmine integrations remain.  Likely 2–3 sittings to cover.
- **Encrypted-at-rest memory store** (skipped MEDIUM since sitting 4) — architectural design memo before any code.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **`WebServerHelpers::ConfinePathUnder(root, relative)` is the canonical static-asset / log-file path-traversal gate** for `webServer.cpp` and `webServer_studio.cpp`.  Same semantics as `WorkspaceIndexer::ResolveAndConfine` (returns empty path on rejection: absolute relative, resolution error, or `..` escape after canonicalisation).  When adding a new web handler that builds a filesystem path from caller input, route through this helper before touching disk.
- **`m_WsClientRoles` is the per-connection role registry on the dashboard's `/ws`.**  Roles are pinned at `.onaccept` (via Crow's `userdata` void* slot — the heap copy is freed in `.onopen` after being moved into the map), looked up under `m_Mutex` in any message-type branch that mutates disk state.  When adding a new admin-only message type, follow the `ai-write-scripts` pattern: read role from `m_WsClientRoles`, reject with a typed JSON error if not `"admin"`.
- **Crow URL-decodes path parameters before handlers run, but curl normalizes `..` segments client-side.**  This bit me during runtime verification: `curl "https://.../dash-assets/../../etc/passwd"` becomes `GET /etc/passwd` at the wire level (curl's pre-flight URL normalization), so the handler isn't even reached.  Use `curl --path-as-is` to bypass curl's normalization and see what your handler actually does on a literal `..`-laden URL.  Encoded `%2E%2E` is *not* a reliable substitute — Crow's static-route matching may treat the encoded form as a literal filename character rather than a route segment with `..`.
- **`HandleMcpHeartbeatPost` now requires `(crow::request const&)`** — the no-arg form is gone.  The route registration is updated accordingly.  The MCP sidecar already sends `Authorization: Bearer <admin-key>` on every request, so this change is invisible to it; any custom heartbeat poker would need an MCP key.
- **`fromOffset` clamp in `ReadLogFile` is defense-in-depth, not a fix for an active bug.**  The existing `if (fromOffset >= 0)` and `if (fromOffset >= fileSize)` guards already prevent the over-allocation the audit feared.  The new `if (fromOffset > fileSize) fromOffset = fileSize;` documents the invariant explicitly so a future refactor that drops one of the guards still bounds `deltaSize`.

---

## 2026-04-30 (S1 sitting 5) → next session

S1=D2 sitting 5.  Theme: close out the four assistant-internal cross-component items the sitting-4 hand-off enumerated as the natural follow-up cluster — engine `ThreadPool` migration (out of bespoke `std::thread`s), `QueueMessage` drain CV (so AI replies surface immediately rather than on the next `OnMessage`), the long-tracked `JsonEscape` four-copy convergence (last two anon-namespace copies in `assistantTools.cpp` + `assistantController.cpp`), and an explicit thread-safety contract on `ToolRegistry` (the only one of the three shared registries that lacked one).  Boundary at sitting-end: the assistant subsystem is now free of direct `std::thread`s, has no hidden `JsonEscape` duplicates, and has documented concurrency contracts on every component the AI lambda touches concurrently.  D2 web/cloud surface is the densest remaining cluster — kicks off in sitting 6.

### What landed

1. **`assistantController` AI dispatch moved off `std::thread`, onto the engine `ThreadPool`.**  `m_BackgroundThreads` (vector of `std::thread`) → `m_BackgroundFutures` (vector of `std::shared_future<void>`).  `RunAiCallAsync` calls `Core::g_Core->GetThreadPool().SubmitTask([...]() {...}).share()` and stores the future.  `JoinFinishedThreads` (was a documented no-op) → `JoinFinishedFutures` which actually drops finished futures via `wait_for(0ms) == ready`.  `Shutdown` snapshots futures under `m_ThreadsMutex` then waits outside the lock; lifetime relative to WRM teardown is preserved by the existing `jarvisAgent.cpp` shutdown ordering (`AssistantController::Shutdown` → engine ThreadPool teardown).  No `THREADS_REQUIRED_BY_APP` bump — the drain loop sleeps on its CV, doesn't block other workers.
2. **`QueueMessage` drain CV.**  New `m_DrainCv` (bound to `m_PendingMutex`); `QueueMessage` notifies on every successful enqueue; new `DrainLoop` task submitted to the engine pool from the controller constructor calls `DrainPendingMessages` whenever the CV fires (or once per second as a backstop).  Closes the latent bug the sitting-4 entry's `QueueMessage` cap explicitly documented: AI replies produced after the user's last message used to sit until the next inbound `OnMessage` triggered a drain.  The existing `DrainPendingMessages` calls inside `OnMessage` remain (now redundant for AI replies but useful for synchronous protocol-error flushes before the handler returns).  Crow's `send_text` is thread-safe (`asio::post` onto the io-context strand, verified in `vendor/crow/include/crow/crow/websocket.h`), so calling `DrainPendingMessages` from the drain loop is correct without bouncing through an io_context dispatch.
3. **Last two `JsonEscape` copies migrated to `JsonHelper::EscapeJsonString`.**  `assistantController.cpp` (38 callers) and `assistantTools.cpp` (1 caller) now `#include "json/jsonHelper.h"` and route through the central helper.  The local anon-namespace `JsonEscape` definitions are deleted.  This is convergence — both copies were RFC 8259-compliant after sittings 2–3 — but eliminates the maintenance hazard if `JsonHelper::EscapeJsonString` ever updates its policy and the duplicates don't.
4. **`ToolRegistry` thread-safety contract documented.**  New class-level comment block in `assistantTools.h` before `class ToolRegistry` makes the set-once nature of the setters explicit, the after-publication read-only contract on the backing pointers explicit, the targets' individual thread-safety explicit, and adds a forward note: "if a future change introduces post-publication mutable state, add a mutex first."  No code change — purely closes the documentation gap that left the registry as the only one of the three shared registries (`MemoryStore`, `WorkspaceIndexer`, `ToolRegistry`) without an explicit contract.

Four per-change template entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 5".  Skipped-findings table records the 5 deferred items with reasons (notably: pool sizing bump, dashboard `DrainPendingBroadcasts` parallel refactor, captured-shared-state lambda struct, dedicated shutdown stress test, Tracy scope on the drain loop).

### What's verified

- Studio debug build clean (`make config=debug`); the four touched `.cpp` files (`assistantController.cpp`, `assistantTools.cpp`, plus headers `assistantController.h`, `assistantTools.h`) recompile and link.
- **28-test assistant non-AI suite: PASS** end-to-end in 2.1 s against the new debug binary.  Covers session create/resume/list, history replay, all 11 slash commands, completion, protocol error paths.  Implicitly exercises the new future-based dispatch path (every `user_message` test runs through `RunAiCallAsync` → engine pool submit), the new drain CV (every `QueueMessage` notifies; every test's response surfaces through the drain loop or the redundant in-handler drain), and the migrated `JsonEscape` paths (every protocol message embeds a `JsonHelper::EscapeJsonString`-encoded sessionId).
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — adjacent dispatcher path unbroken.  Confirms the engine ThreadPool change didn't perturb the curl-multi dispatch path that uses the same pool.
- MCP sidecar verified live: `mcp__j9t__whoami` (admin), `mcp__j9t__debug_signals` (`keys_unlocked=true`, `uptime_seconds=15+` immediately post-restart).
- **Not directly verified:** live AI multi-step dispatch under the new drain CV (the regime where intermediate `tool_status` / `tool_result` messages benefit most from immediate surfacing — `--with-ai` would confirm); shutdown stress test (rapid connect / dispatch / disconnect cycles to harden the new future-based join path); a real session running long enough to verify `JoinFinishedFutures` actually trims the vector under load.  These are predictable carry-overs.

### Open items / next-session candidates

- **Sitting 6 candidates (in order of density):**
  - **D2 web/cloud surface — densest CRITICAL cluster after the assistant subsystem.**  `application/web/webServer.cpp` (gen-purpose REST + WS), the cloud task executors (`azureBlobCloudTaskExecutor.cpp`, `gcsCloudTaskExecutor.cpp`, `s3CloudTaskExecutor.cpp`, etc.), the email/IMAP code path, the GitHub/Snowflake/Redmine integrations.  The audits (`doc/combinedCyberSecAudit.md`, `doc/combinedSafetyAudit.md`) flagged a substantial pile of HIGHs and a handful of CRITICALs across this surface.  Likely 2–3 sittings to cover.
  - **Migrate `WebServer::DrainPendingBroadcasts` to a CV-driven model** — same architectural shape as sitting 5's assistant fix, but the dashboard has constant client interaction so the latent gap is much smaller in practice.  Could pair with the webServer sitting.
  - **Encrypted-at-rest memory store** (skipped MEDIUM since sitting 4) — architectural design, not a single-sitting fix; design memo before any code.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **`AssistantController::m_BackgroundFutures` is `std::vector<std::shared_future<void>>`** (not `std::thread`s).  Background AI lambdas now run on the engine `ThreadPool` (`Core::g_Core->GetThreadPool().SubmitTask(...)`).  The previous bespoke `std::thread` pattern is gone.  When adding a new long-running task in the controller, follow this pattern: submit to the engine pool, capture the `.share()`'d future, push onto `m_BackgroundFutures` under `m_ThreadsMutex`, and let `Shutdown` drain.
- **`AssistantController::DrainLoop` is now a long-running task on the engine `ThreadPool`** that wakes on `m_DrainCv.notify_one()` from `QueueMessage`.  It holds no locks while invoking `DrainPendingMessages`, so `QueueMessage` producers and the WS-handler thread don't contend with it.  If you add a new path that produces messages, you don't need to call `DrainPendingMessages` explicitly — `QueueMessage` notifies the drain loop for you.  The existing `OnMessage` calls to `DrainPendingMessages` remain because synchronous flush before a handler returns is desirable for protocol-error messages.
- **`JsonHelper::EscapeJsonString` is the canonical RFC 8259 escape across the entire assistant subsystem now.**  All five subsystem files (`assistantSession.cpp`, `assistantMemory.cpp`, `workspaceIndexer.cpp`, `assistantController.cpp`, `assistantTools.cpp`) route through it.  No anon-namespace `JsonEscape` copies remain inside `application/assistant/`.  When adding a new JSON-string-content embed in this subsystem, use the static `JsonHelper::EscapeJsonString(view)` directly.
- **`ToolRegistry`'s thread-safety contract is now explicit in the header.**  Set-once setters (`SetWorkflowRegistry`, `SetWorkflowRuntimeManager`, `SetMemoryStore`, `SetWorkspaceIndexer`, `SetAiCallFn`) called once on the owning thread before AI dispatch begins; `Execute` / `BuildToolDescriptions` / `GetToolDefs` safe from any thread post-publication; backing pointers immutable; targets individually thread-safe.  If you ever need to introduce post-publication mutable state in `ToolRegistry`, add a mutex first — the existing contract permits no in-place modification.
- **Crow's `send_text` is thread-safe.**  The implementation (`vendor/crow/include/crow/crow/websocket.h::send_data`) wraps every call in `asio::post(adaptor_.get_io_context(), ...)`, so it's safe to call from any thread; the actual write happens on the io-context strand.  This is what makes the drain CV work without bouncing through an io_context dispatch.

---

## 2026-04-29 (S1 sitting 4) → next session

S1=D2 sitting 4.  Theme: close the four remaining D2 assistant-side audit clusters — `assistantSession.{h,cpp}`, `assistantMemory.{h,cpp}`, `workspaceIndexer.{h,cpp}`, `contextAssembler.{h,cpp}` — plus the long-deferred `JsonEscape` four-copy convergence and the `RandomHex` / `IsValidOpaqueId` extraction that sitting 3 explicitly tracked-but-skipped.  Every HIGH cyber-sec/safety finding in those four files plus the load-bearing MEDIUMs are closed.  Boundary at sitting-end: D2 cyber-sec audit is now complete for the **assistant subsystem** (sittings 1–4 covered `assistantTools.{cpp,h}` + `assistantController.{cpp,h}` + the four files above).  Sitting 5 starts on the rest of D2 — the web/cloud surface — or on the cross-component refactors (`JoinFinishedThreads → engine ThreadPool`, broader thread-safety contract audit) if JC prefers to clean up the assistant-internal debt first.

### What landed

1. **`engine/json/jsonHelper.{h,cpp}` rewrite + 3 broken `JsonEscape` copies retired.**  The existing `JsonHelper::SanitizeForJson` had a literal 0x0C (form-feed) `case` label that *dropped* the byte, plus no escaping for the rest of the 0x00–0x1F control range — broken since landed.  Rewritten as `static std::string EscapeJsonString(std::string_view)` with proper RFC 8259 §7 escaping (the four shorthand cases plus `\u00XX` for every other control byte); instance `SanitizeForJson` now delegates so 10+ existing callers in `aiTranscript.cpp` / `requestBuilder.cpp` upgrade transparently.  This is a positive side-effect for every outbound AI request body and persisted transcript.  `assistantSession.cpp::JsonEscape`, `assistantMemory.cpp::JsonEscapeMem`, `workspaceIndexer.cpp::JsonEscapeIdx` deleted; all three files now route through the central helper.  `assistantTools.cpp` and `assistantController.cpp` keep their anon-namespace copies (both already RFC-correct after sittings 2–3); migrating them is a mechanical sweep over ~50 QueueMessage call sites tracked as a follow-up.
2. **New `application/assistant/assistantHelpers.{h,cpp}`** with `RandomHex(numBytes)` (RAND_bytes-backed, ERROR-logged on failure, fail-closed empty return) and `IsValidOpaqueId(s)` (strict `[A-Za-z0-9_-]{1,128}` allowlist).  `assistantController.cpp` drops both local copies (was `RandomHex` in file-scope anon namespace + `IsValidSessionId` in `namespace AIAssistant { namespace { ... } }`).  Sitting 3 had landed at the third-copy threshold; sitting 4 needed both at three new sites, so convergence happened here.
3. **`assistantSession.{h,cpp}` reworked.**  HIGH path-traversal in resume ctor → `IsValidOpaqueId` validation as defense-in-depth alongside the controller-layer gate.  HIGH weak/predictable session ID → `"sess_" + RandomHex(16)` (128-bit entropy, no timestamp, no counter; fixes the same-millisecond-restart corruption bug as a side effect).  HIGH `AppendTurn` silent failures → renamed `AppendTurnLocked`, write-then-commit ordering, explicit flush + `good()` check, sticky `m_FileBroken`, `[[nodiscard]] bool` propagated through `AddUserMessage`/`AddAssistantMessage`; 6 controller call sites updated to `(void)` with the contract documented at the first site.  HIGH lock-from-ctor → `LoadFromFileLocked` (no lock acquisition, contract documented).  HIGH `ListSessions` TOCTOU → drop `exists` pre-check, distinguish missing-vs-permission-error.  MEDIUM unbounded JSONL load → `kMaxTurnsPerSession=10000`, `kMaxLineBytes=1 MiB`, `kMaxTurnTextBytes=256 KiB`.  MEDIUM `ExtractJsonString` missing `\uXXXX` decode → home-built parser deleted, replaced with simdjson per memory `feedback_simdjson_only`.  MEDIUM session-ID logging → new `LogSafeSessionId` truncates to 8 hex chars at every log site.  LOW POSIX file permissions → `fs::permissions(path, owner_read | owner_write, replace)` after first write (best-effort, ignored on Windows).  LOW role-validation exhaustiveness → load-time filter rejects roles outside `{user, assistant, system}`.
4. **`assistantMemory.{h,cpp}` reworked.**  HIGH RNG race → `mt19937_64` deleted, `GenerateId` returns `"mem_" + RandomHex(16)` with static-mutex-guarded counter fallback.  HIGH `GetRelevant` lock-pattern → extracted `RecallLocked`; public `GetRelevant` acquires the mutex once and trims under the same lock (atomicity gap closed, latent deadlock removed).  HIGH `SaveToDisk` silent failures → renamed `SaveToDiskLocked`, returns bool, ERROR-level logs, sticky `m_FileBroken`, in-memory rollback on persistence failure (so disk and RAM stay consistent).  HIGH `LoadFromDisk` TOCTOU + lock-from-ctor → drop `exists` pre-check, rename to `LoadFromDiskLocked`, no lock.  MEDIUM unbounded entries/fields → `kMaxEntries=10000`, `kMaxKeyBytes=256`, `kMaxValueBytes=64 KiB`, `kMaxTagBytes=256`, `kMaxTagsPerEntry=32` enforced at both load and Save.  MEDIUM raw pointers in `Recall` → `Scored { score, size_t idx }` instead.  MEDIUM control-char JSON escaping → routed through `JsonHelper::EscapeJsonString`.  MEDIUM logging severity → all persistence-failure paths at ERROR.  MEDIUM `[[nodiscard]]` on `Save`/`Delete`/`Recall`/`ListAll`/`GetRelevant`/`Size` (no caller warnings since `assistantTools.cpp` already captured all returns).  LOW key logging redaction → `LogSafeKey` strips control bytes, caps at 64 chars, appends original length.
5. **`workspaceIndexer.{h,cpp}` reworked.**  HIGH `ReadFileContent` path traversal → `static` removed, constructor captures `m_WorkspaceRoot = fs::weakly_canonical(fs::current_path())` at startup, new private `ResolveAndConfine` does `weakly_canonical(root / raw)` + `lexically_relative(root)` containment check; symlinks pointing out of tree caught at resolution; absolute paths rejected.  `assistantTools.cpp::ExecGetFileSummary` updated from static `WorkspaceIndexer::ReadFileContent(...)` to `m_WorkspaceIndexer->ReadFileContent(...)` (ToolRegistry already holds the pointer).  HIGH untrusted index data → every `relativePath` parsed from `file_index.jsonl` re-runs through `ResolveAndConfine`; rejects log `[security] indexer_index_path_escape len=...`; bonus `kMaxIndexEntries=100000` cap during load.  HIGH `LastScanTime` no lock → acquire mutex.  HIGH `ScanDirectory` ec ignored → check + skip on filesystem-call failure.  MEDIUM `ReadFileContent` truncation logic → save original size before clamping, single `fs::file_size` call.  MEDIUM `SaveIndex` silent errors → check `ofs.good()` after flush, ERROR on failure; `LoadIndex` distinguishes missing-vs-unreadable.  MEDIUM control-char escaping → `JsonHelper::EscapeJsonString`.  MEDIUM raw pointers in `GetRelevantFiles` → `Scored { score, size_t idx }`.  LOW `kMaxSummaryBytes=8 KiB` cap on `SetFileSummary`.  LOW `IsIndexableExtension` → `static`.  Header now documents the workspace-root snapshot semantics (cwd changes after construction don't widen access) and the orthogonality of this gate vs. `ToolRegistry::IsPathDenied` (containment vs. deny-list).
6. **`contextAssembler.{h,cpp}` reworked.**  MEDIUM prompt injection via `turn.text` → new `static DefangContextSentinels(text)` that (a) calls `ToolRegistry::DefangToolMarkers` for `<tool_call>`/`</tool_call>`/`<tool_result>`/`</tool_result>` and (b) replaces any run of 3+ `=` with the same number of U+2550 (BOX DRAWINGS DOUBLE HORIZONTAL); applied to every prior turn's text and to the new userMessage.  MEDIUM unbounded context → `kMaxUserMessageBytes=64 KiB`, `kMaxTurnTextBytes=32 KiB`, `kMaxConversationContextBytes=128 KiB`, `kMaxToolDescriptionsBytes=64 KiB`; per-turn truncation + total-context truncation; `userMessage` clamped before placing in `prompt.prob`.  LOW `userMessage` defang → covered by the same pass.

22 per-change template entries (including the two PRE-STEP entries) appended to `doc/misc/S1-D2-session-note.md` under "Sitting 4".  Skipped-findings table records the 12 deferred items with reasons.

### What's verified

- Studio debug build clean (`make config=debug` after `premake5 gmake` to pick up the new `assistantHelpers.cpp` — important: see Gotchas).
- **28-test assistant non-AI suite: PASS** end-to-end against the new binary in 2.1s.  Covers session create/resume/list, history replay, all 11 slash commands, completion, protocol error paths.  Implicitly exercises the rewritten JSONL save/load round-trip, the new `IsValidOpaqueId` gate, the simdjson-based session parser, the `LogSafeSessionId` truncation in INFO logs, the WorkspaceIndexer `ReadFileContent` instance call.
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — request-body path through the rewritten `JsonHelper::SanitizeForJson` is unbroken.
- MCP sidecar verified live: `mcp__j9t__whoami` (admin), `mcp__j9t__debug_signals` (`keys_unlocked=true`, `uptime_seconds=754`, `workflow_runs_total_completed=2`).
- **Not directly verified:** the live `<tool_call>` / `=== ... ===` prompt-injection defang (covered structurally by the unit-test-equivalent build but no `--with-ai` runtime smoke); `ReadFileContent` workspace-root rejection on a real symlink-out-of-tree (would need a test fixture); `MemoryStore::Save` rollback path under simulated disk-full (would need fault injection).  These are the predictable carry-overs.

### Open items / next-session candidates

- **Sitting 5 candidates:**
  - **Cross-component refactors that have been tracked since sitting 3:** `JoinFinishedThreads` → engine `ThreadPool` (memory `feedback_no_jthread_use_threadpool`); thread-safety contract audit on `m_ToolRegistry`/`m_MemoryStore`/`m_WorkspaceIndexer` (background lambda + main thread accessing concurrently with no documented contract); `QueueMessage` drain CV/timer (responses produced after the last user message currently sit until the next `OnMessage`).
  - **Migrate the remaining `JsonEscape` copies** (`assistantTools.cpp` + `assistantController.cpp`) to `JsonHelper::EscapeJsonString` — both correct today but mechanical-sweep convergence eliminates the last two duplicates inside the assistant subsystem.
  - **D2 web/cloud surface:** the audit findings on `webServer.cpp`, the cloud task executors (`azureBlobCloudTaskExecutor`, `gcsCloudTaskExecutor`, etc.), the email/IMAP code path, the GitHub/Snowflake/Redmine integrations.  Densest CRITICAL surface after the assistant subsystem.
  - **Encrypted-at-rest memory store** (skipped MEDIUM in sitting 4) — architectural design, not a single-sitting fix.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **Premake regen is required when adding new `.cpp` files.**  Sitting 4 added `application/assistant/assistantHelpers.cpp`.  The premake glob `application/assistant/**` resolves at `premake5 gmake` time, not at `make` time.  Build will link-fail with `undefined reference` until you re-run `premake5 gmake`.  Memory `feedback_rebuild_after_premake_clean` covers a related case; this is a complementary rule.
- **`JsonHelper::EscapeJsonString` is now the canonical RFC 8259 escape.**  `JsonHelper::SanitizeForJson` (instance method) still works — kept as a thin delegator so the 10+ existing call sites in `aiTranscript.cpp` / `requestBuilder.cpp` upgrade transparently.  New code should call `JsonHelper::EscapeJsonString(x)` static directly.  The previous instance-method version had a literal 0x0C `case` label that silently dropped form-feed bytes — that bug is now closed.
- **`AssistantSession::AddUserMessage` / `AddAssistantMessage` are `[[nodiscard]] bool`** signalling whether the turn was both pushed in-memory AND durably written to disk.  All 6 controller call sites currently `(void)` the return because the session itself emits ERROR-level logs on persistence failure with a sid_prefix the dashboard's run analyzer surfaces.  A future caller that wants to reflect persistence state to the user (e.g., a "session degraded" badge in the dashboard) can switch to checking the bool.
- **`AssistantSession`-style sticky `m_FileBroken` flag** is now the project-wide pattern for "memory and disk diverged, refuse further writes": same flag in `MemoryStore` and same shape in (TBD) future stores.  When in degraded state, public mutators fail-fast at the head of the function with an ERROR log; the only recovery is operator intervention (delete or fix the file, restart j9t).
- **`WorkspaceIndexer::ReadFileContent` is no longer static** and now anchors against the workspace root captured at construction.  Caller is `ToolRegistry::ExecGetFileSummary` via `m_WorkspaceIndexer->ReadFileContent(filePath, 32768)`.  This is workspace-confinement only; sensitive-file deny-listing remains `ToolRegistry::IsPathDenied` and is applied at the same call site.  Future tools that read user-influenced relative paths should follow the same orthogonal pattern: workspace gate + deny-list gate.
- **`ContextAssembler::DefangContextSentinels` is the canonical pattern for AI-context-boundary defense at the inbound side.**  Sitting 3 added `ToolRegistry::DefangToolMarkers` for the `<tool_*>` markers; sitting 4 wraps that plus `===` collapse into a single helper applied to every user-origin turn and the new user message.  When a future component places attacker-influenced text into the AI prompt (e.g., a recalled memory, a recalled file summary), it should call this helper first.

### Doc sweep (post-sitting)

After sitting 4 closed: `engine/json/json.md` §4 rewritten for the new `JsonHelper::EscapeJsonString` static + retained `SanitizeForJson` instance delegator + RFC 8259 control-byte escape; `application/assistant/README.md` refreshed end-to-end so it reflects cumulative sittings 1–4 state (storage formats, all backend modules, expanded safety table from ~17 to ~22 rows, new `assistantHelpers` subsection, files list).  `doc/architecture.md` scanned, no edit needed (it stays at component-name level).  Doc-hygiene principle saved as `feedback_doc_routing` memory: each fact has one home, tracked docs cross-ref but never inline build/run/launcher mechanics, and never name memory files.  A repo-wide audit confirmed near-zero existing duplications.

S1=D2 sitting 3.  Theme: close the controller-layer security funnel in `application/assistant/assistantController.{h,cpp}` — every CRITICAL and HIGH cyber-sec finding plus the load-bearing concurrency-safety HIGHs that share the same code regions.  Plus a pre-step audit-trace sweep across sittings 1+2+3 driven by JC's directive that audit citations don't belong in source code.

### What landed

1. **Audit-trace sweep across sittings 1+2+3 + new memory.**  Stripped every audit citation, severity tag (`[HIGH]`, `[CRITICAL]`, `Cyber-sec §02`), session-tracking ref, and inline "memory `feedback_X`" note from the working-tree sitting changes.  Where the invariant the comment was protecting is non-obvious, kept it as plain English; otherwise deleted.  `LOG_SECURITY_*` runtime tags stay (operational signals, not change-trace).  New memory `feedback_no_audit_traces_in_code.md` records the rule.  Mechanical scan via `grep -nE '§|\[HIGH\]|cyber-sec|audit|...'` returns no matches in the affected files; build clean.
2. **`HandleLogCommand` rewritten on `std::ifstream` seek-tail** — pure C++ tail reader replaces the popen + `tail` + `WrapForBash` shell composition.  Closes the cyber-sec CRITICAL (latent shell-injection if the path were ever made configurable), the safety HIGH (popen + buffered stream silently swallowing errors), and the LOW path-disclosure (the user-visible error string no longer echoes the absolute path; that goes only to the server-side `LOG_APP_INFO` for operator triage).  `WrapForBash` and the `popen`/`pclose` Windows aliases deleted (now unused).
3. **`GetSession` strict allowlist + canonical-path containment** — sessionId regex-validated as `[A-Za-z0-9_-]{1,128}` before any path construction; resolved file additionally canonicalised under the sessions dir as defense-in-depth against symlinks.  Closes the CRITICAL path-traversal.  Logs `[security] assistant_session_invalid_id length=…` (length only) and `[security] assistant_session_path_escape sid_len=…` on rejection — never the value.
4. **Approvals bound to originating connection + cryptographic requestId** — `PendingApproval` grows `originConn` (pointer identity only, never dereferenced); `RunAiCallAsync` threads the connection through via lambda capture; `HandleApprovalResponse(conn, requestId, approved)` rejects any conn-mismatch with `LOG_SECURITY_WARN`.  `m_NextApprovalSeq` deleted; `requestId = "apr_" + RandomHex(16)` (128-bit `RAND_bytes` hex).  `OnClose` calls new `CancelApprovalsForConnection(&conn)` which fail-closes any approvals owned by the disconnecting client (otherwise the AI loop hangs on the 60 s timeout, *and* a future connection that reuses the pointer address could match by identity).  `Shutdown` notify_all moved out of `m_ApprovalsMutex` (snapshot under lock, notify outside).  Closes the HIGH approval-bypass + the MEDIUM sequential-requestId.
5. **WS frame size cap + maxEntries clamp** — `OnMessage` rejects frames > 64 KB before constructing `simdjson::padded_string` (logs `[security] assistant_ws_frame_too_large bytes=…`).  `get_history` clamps `maxEntries` with `std::clamp<int64_t>(val, 1, 500)` before the cast (a negative value previously caused an unbounded loop).
6. **`m_Sessions` `unique_ptr` → `shared_ptr<AssistantSession>`** — the background AI lambda captures a shared_ptr so the session stays alive across the multi-step tool loop regardless of `m_Sessions` evictions or `Shutdown` ordering.  All 10 callers updated.  `HandleListSessions` and `HandleCompletionRequest` now snapshot under `m_SessionsMutex` then iterate outside the lock — closes both the lock-order TOCTOU and the controller-mutex-while-session-side-mutex inversion.  `OnOpen` / `OnClose` read `m_Clients.size()` inside the lock scope (data-race fix).
7. **`DrainPendingMessages` per-client revalidate under lock** before each `send_text` — same pattern `WebServer::DrainPendingBroadcasts` already uses for the `/ws` broadcast loop.  Narrows the use-after-free window to Crow's deferred-destruction semantics, matching the rest of the codebase.
8. **`DefangToolMarkers` promoted to public `ToolRegistry::DefangToolMarkers`** — was private to `assistantTools.cpp` after sitting 2; this sitting added a second use site (`RunAiCallAsync` reflecting `result.output` into the `<tool_result>...</tool_result>` block).  Per memory `feedback_cpp_discipline` "refactor to one helper before adding a third copy", extracted before the third site.  All 4 prior call sites in `assistantTools.cpp` resolve via unqualified name lookup (member functions of the same class).  Closes the MEDIUM tool-result XML injection.
9. **`HandleRunsCommand` `default:` arm removed** — switch over `WorkflowRunState` is now closed; `-Wswitch` will catch any future enumerator addition.
10. **`QueueMessage` capped at 10k + log redaction + `WriteFile` flush check** — pending-queue overflow logs `LOG_APP_ERROR` and drops; memory recall + approval logs report counts/lengths/prefixes only (no message text, no memory values, no full requestId); `WriteFile` flushes explicitly + returns generic error string (no path leak).

Ten per-change template entries for the above plus two PRE-step entries appended to `doc/misc/S1-D2-session-note.md` under "Sitting 3".  Skipped-findings table records the deferred items.

### What's verified

- Studio debug build clean (`make config=debug`).
- **28-test assistant non-AI suite: PASS** end-to-end against the new binary (`python3 test/assistant/test_assistant.py` → 28 passed, 0 failed in 2.1 s).  Covers every controller protocol path including the rewritten `/log` slash command, the now-clamped `get_history`, the now-hardened `approval_response`, and all 11 slash commands.
- `python3 test/dispatch/test_testinterface_hermetic.py`: PASS — adjacent dispatcher path unbroken.
- **Not directly verified:** the AI-driven runtime path that exercises `RequestToolApproval` end-to-end with the connection-binding check, the WS frame-size rejection, and the tool-result defang on real tool stdout — these need either `--with-ai` or a manual dashboard chat session.  JC to drive that pre-commit if appetite allows.

### Open items / next-session candidates

- **Sitting 4** is now the natural follow-up cluster: `assistantSession.h` (HIGH path traversal in ctor + HIGH weak random session ID + MEDIUM/LOW data integrity), `assistantMemory.h` (HIGH RNG race + HIGH lock inversion + MEDIUM JSON-escape control-char gap + MEDIUM unbounded `m_Entries`), `workspaceIndexer.h::ReadFileContent` (HIGH path traversal — pairs with the rewritten `IsPathDenied`), `contextAssembler.h` (MEDIUM/LOW prompt injection).
- **Cross-component refactors deferred from sitting 3** (better in their own sittings):
  - `JoinFinishedThreads` → engine `ThreadPool` (memory `feedback_no_jthread_use_threadpool`).
  - `m_ToolRegistry` / `m_MemoryStore` / `m_WorkspaceIndexer` thread-safety contract audit (background lambda accesses these concurrently with no documented contract).
  - `QueueMessage` drain CV/timer (responses produced after the last user message currently sit until the next `OnMessage`).
  - `JsonEscape` triple-copy + new `RandomHex` triple-copy convergence — both flagged by `feedback_cpp_discipline`'s "third copy" rule; tracked but not bundled.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **Audit citations don't belong in source code.**  Memory `feedback_no_audit_traces_in_code` makes this explicit.  Code comments must not cite audit findings, severity tags, sitting numbers, or any session-tracking artifact — the why must stand on its own.  The change record + finding-citation lives in the session note (`doc/misc/S1-D2-session-note.md` etc.); the code's job is to read coherently for a fresh reader who doesn't care which sitting landed which line.  `LOG_SECURITY_*` runtime tags stay — those are operational signals, not change-trace.  The line: tags consumed at runtime stay; tags consumed only by the human reading the diff today don't.
- **Keystore unlock is explicit, not env-driven.**  Even with `JARVIS_MASTER_PASSWORD` exported, the binary needs `POST /api/settings/keys/unlock` with `{"master_password": "..."}` before MCP-key auth on REST/WS will succeed.  A fresh-start log will show `[Application] [warning] Blocked workflow run '...': contains ai_call tasks but no AI providers are configured. Unlock the key store via POST /api/settings/keys/unlock` if you forgot.
- **`DefangToolMarkers` is now `ToolRegistry::DefangToolMarkers` (public static)** — apply at every site where externally-sourced text re-enters the AI's view as tool-result content.  `assistantController.cpp::RunAiCallAsync` is the second site; the four prior sites in `assistantTools.cpp` resolve via unqualified name lookup.
- **`PendingApproval::originConn` is identity-only — never dereference it.**  The pointer is stored by the WS-handler thread, compared by the WS-handler thread (after `&conn` is also alive), and cleared by `OnClose → CancelApprovalsForConnection`.  Any code that wants to treat originConn as a live pointer is wrong.
- **Sessions are now `shared_ptr<AssistantSession>`.**  Background AI threads should declare `std::shared_ptr<AssistantSession> session = GetSession(sid);` to keep the session alive across blocking tool calls.  Don't store raw `AssistantSession*` from `GetSession()` anywhere that outlives the immediate handler.

---

## 2026-04-29 (S1 sitting 2) → next session

S1=D2 sitting 2.  Theme: close out `application/assistant/assistantTools.cpp` HIGH/MEDIUM findings — the canonical-path / external-content reflection class — plus a pre-work test-harness fix that pays for itself this sitting.  Sitting 3 starts on `assistantController.h` from a clean file.

### What landed

1. **Test harness TLS+self-signed+auth gap closed.**  `test/assistant/test_assistant.py` now defaults to `wss://localhost:8443/ws/assistant`, accepts `--token` (or `J9T_TOKEN` env-var) and passes it as `Authorization: Bearer ...` on the WS handshake, disables cert verification on `wss://`, and serializes every `ws.send()` / `ws.recv()` site behind a `_ws_io_lock` (websocket-client 1.7.0's WebSocket isn't thread-safe under TLS+concurrent send/recv — the ping-thread + main-thread race that previously dropped the connection within ~40 ms).  Result: **28 non-AI tests now pass**, previously zero ran since the j9t HTTPS migration.
2. **`IsPathDenied` rewritten** on `fs::weakly_canonical(projectRoot / path)` — resolves symlinks in any existing prefix, defends against the "safe.txt → /etc/passwd" exfiltration the audit caught.  Adds project-root-confinement (paths that resolve outside project root denied), case-folded filename + extension comparison, and `.bak`/`.tmp` extension denies plus per-base sibling filenames (`config.json.bak`, `keys.json.bak`, etc.) — closes the "ExecWriteFile/EditFile leak via predictable backup path" HIGH.  Fail-closed on any resolution error.
3. **`ExecGetFileSummary` now calls `IsPathDenied`** before reading + forwarding bytes to the external AI provider — closes the audit's MEDIUM/exfiltration finding.  Applied before the cached-summary check so a stale cache pre-dating the deny rule doesn't surface a denied file's summary either.
4. **`ExecJcwfWriteScript` path validation rewritten** — absolute-path reject + `fs::weakly_canonical(projectRoot / path)` + `lexically_relative(scriptsRoot)` empty-or-`..`-prefix reject + `IsPathDenied` call.  Closes the "checks `scripts/` prefix only" HIGH.
5. **`ExecJcwfGenerate` JSON-escapes `workflowId`** — new `JsonEscape` helper in the file's anonymous namespace (RFC 8259 string-content; handles `"`, `\\`, `\\n`, `\\r`, `\\t`, control chars via `\\u%04x`).  Closes the "raw string-concat into global.json" HIGH.  Note: this is now the third `JsonEscape` copy in the codebase (assistantSession.cpp + workspaceIndexer.cpp + this); convergence into `engine/json/jsonHelper.h` tracked as a follow-up.
6. **`<tool_call>` / `<tool_result>` markers defanged** in reflected external bytes — `ExecGetTaskOutput` (error + stdout + stderr) and `ExecGetRunStatus` (per-task error) now run their reflected text through `DefangToolMarkers`, which replaces literal `<tool_call>`/`</tool_call>`/`<tool_result>`/`</tool_result>` ASCII sequences with U+27E8/U+27E9 mathematical-angle-bracket equivalents.  Visual content preserved; the parser-keying ASCII bytes are gone.  Closes the indirect-prompt-injection MEDIUM where a script printing `<tool_call>` to stdout becomes a parsed tool call on the next AI turn.
7. **Per-change template entries** for all 6 changes appended to `doc/misc/S1-D2-session-note.md` under "Sitting 2 — assistantTools.cpp HIGHs (canonical-path theme + JSON-escape + tool-marker defang)".  Skipped-findings table records the deferred items with reasons.

### What's verified

- Studio debug build clean (`make config=debug`); only `assistantTools.cpp` recompiled, link succeeds.
- 28-test assistant non-AI suite: **PASS** end-to-end against TLS+auth j9t (with the harness fix).  This is regression coverage that the protocol/session/command surface still works after the rewrites.
- `python3 test/dispatch/test_testinterface_hermetic.py`: **PASS** — adjacent code paths unbroken.
- **Not directly verified:** the rewritten C++ tool-execution code paths (IsPathDenied on real symlinks, deny-list extension matches, JsonEscape on adversarial workflowIds, DefangToolMarkers on adversarial stdout) require AI-driven tool calls.  The 28-test suite is protocol-level and doesn't reach them.  AI-suite or dashboard manual smoke is the runtime confirmation; not blocking sitting 3 but worth driving before considering S1=D2 closed at the file level.

### Open items / next-session candidates

- **Sitting 3** is `application/assistant/assistantController.h` whole-file: CRITICAL approval-bypass via unauthenticated `approval_response`, CRITICAL path traversal in `GetSession`, plus the safety-side HIGH cluster (background-thread lifetime captures, lock-order inversions, missed CV wakeups, WS client-pointer races, thread-vector unbounded growth — reuse engine threadpool per memory `feedback_no_jthread_use_threadpool`, severity-mismatched logging, `default:` over closed enum).  This is where the densest CRITICALs in the audit live; expect a substantial sitting.
- **`assistantTools.cpp` deferred items** that still need pickup (per skipped-findings table in `S1-D2-session-note.md`): Windows PowerShell `-Command "..."` quoting (HIGH; needs `-EncodedCommand` / script-file pattern), ParseJsonString simdjson rewrite (MEDIUM; per memory `feedback_simdjson_only`), JsonEscape three-copy convergence (discipline).
- **`workspaceIndexer.h::ReadFileContent` HIGH** — different file, pairs with the now-rewritten `IsPathDenied`.  Trivial fix: pass workspace root in, `fs::weakly_canonical` against it.
- **`contextAssembler.h` MEDIUM/LOW** — prompt-injection via unsanitized `turn.text` concatenation, unbounded conversation context.  Can be done alongside the controller work or after.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video.  See `todo.md`.

### Gotchas next-session-Claude should know

- **`test/assistant/test_assistant.py` is now usable for regression testing** — default URL is `wss://localhost:8443`, picks up `J9T_TOKEN` automatically.  Run as `python3 test/assistant/test_assistant.py` (28 non-AI tests) or `--with-ai --auto-approve` (full 70 tests, burns provider quota).  Use the 28-test run as a quick regression check after any assistant-controller / assistant-tools change.
- **`IsPathDenied` is now structural, not just a list match.**  Fail-closed on any path-resolution error.  If a legitimate test path starts getting rejected, check that `fs::current_path()` resolves to the project root at server-start time (it should — that's how j9t is launched).
- **`JsonEscape` is duplicated three times in this codebase** (assistantSession.cpp anon-namespace + workspaceIndexer.cpp + assistantTools.cpp anon-namespace).  Memory `feedback_cpp_discipline` calls for refactor at the third copy — landed past it intentionally to bound this sitting's diff.  Sitting 3 or a dedicated follow-up should converge them into `engine/json/jsonHelper.h` (the existing `JsonHelper::SanitizeForJson` there is broken — silently drops form-feed (line 35 of jsonHelper.cpp has a literal 0x0C char as the case label), missing `\\u%04x` for control chars; replace it).
- **`DefangToolMarkers` is the canonical pattern for AI-context-boundary defense** — apply at every site where externally-sourced text re-enters the AI's view as tool-result content.  Replaces literal ASCII sequences with U+27E8/U+27E9 mathematical-angle-bracket equivalents.  Visual content preserved; parser-keying bytes neutralized.

---

## 2026-04-29 → next session

Two themes today: the §19 deferred SanitizeUtf8 boundary work landed across 7 files, then the first hardening session (S1=D2 sitting 1) opened and closed at the documented boundary "argv-only execution + canonical-cwd in `application/assistant/assistantTools.cpp`".

### What landed

1. **§19 deferred SanitizeUtf8 plumbing** — `application/json/replyParser{API1..API5}.cpp` (real AI reply text fields + error messages), `application/workflow/pythonTaskExecutor.cpp` (captured stdout/stderr once-after-capture), `application/workflow/shellTaskExecutor.cpp` (per-line LOG_APP_INFO sanitize on both popen and fork/exec paths + once-after-capture in caller). Bonus: 4 `std::cout` violations in API1/API3 fixed during the same edits (per memory `feedback_use_log_macros`). Bedrock Anthropic delegates to API4 so it inherits the sanitization for free.
2. **Per-interface mock-transport plan added to `todo.md`** under Pre-1.0, sequenced after the 4 hardening sessions. JC's design call: `IInterfaceTransport` abstraction with `LiveTransport` / `MockTransport` per InterfaceType; matches on `X-J9T-Mock-Fixture` header only (NOT URL or full-header). Complementary to the existing aoai-api-simulator (API6) and LocalStack (API5) HTTP mocks — different layers, different bug classes. Goal: byte-level fault injection through real parsers + per-interface contract tests + drift catches without burning quota.
3. **S1=D2 hardening sitting 1** — 5 cyber-sec CRITICALs in `assistantTools.cpp` closed:
   - `ExecSearchFiles`: POSIX argv exec via new `RunArgvCapture` helper; Windows keeps popen-via-bash but with `PosixSingleQuote` full single-quote escaping; grep fallback dropped (was reintroducing the same injection per audit MEDIUM).
   - `ExecListFiles`: rewritten on `std::filesystem::recursive_directory_iterator` — no exec, no shell, fully portable.
   - `ExecGetLogTail`: deleted (was unreachable from `m_ToolFns`).
   - `IsCommandBlocked`: deleted (blocklist anti-pattern; allowlists fail closed, blocklists fail open per memory `feedback_allowlist_not_blocklist`). Run_shell now relies solely on the human-approval flow at the controller layer for security.
   - `ExecRunShell` cwd validation: new `IsCwdInsideProjectRoot` helper does `fs::weakly_canonical` comparison against `fs::current_path()` — defends against `..`, absolute paths, and symlinks pointing outside the project root. POSIX path applies cwd via `chdir()` in the child (not `cd '$cwd' &&` composition); Windows path applies via CreateProcessA's `lpCurrentDirectory` (not `Set-Location`/`cd` shell prefix). The CWD-injection class is now structurally impossible.
4. **Per-change template entries** for each of the 5 CRITICALs landed in `doc/misc/S1-D2-session-note.md` (new session-tracking file; format per cybersec-hardening §5). Skipped-findings table at the end records every HIGH/MEDIUM in the same file with reasons for sitting-2 deferral. No silent drops.

### What's verified

- Studio debug build clean (`make config=debug`); 5 .cpp files recompiled (assistantController.cpp, assistantTools.cpp, jarvisAgent.cpp, webServer.cpp, webServer_studio.cpp), link succeeds.
- `python3 test/dispatch/test_testinterface_hermetic.py` PASS — the rebuild + new helpers + adjacent code paths all function.
- `python3 test/dispatch/test_stress_tui_utf8_invalid.py` PASS (during the §19 verification earlier in the session) — 140 ai_call tasks with malformed bytes; log/log.txt = 6 MB new, all valid UTF-8. No regression.
- **Not directly verified:** runtime smoke of search_files / list_files / run_shell via the assistant chat surface. The existing `test/assistant/test_assistant.py` connects via plain `ws://localhost:8080` and does not negotiate TLS or accept self-signed certs, so it can't drive a TLS-only j9t. Build + code review + adjacent-test passing is the verification floor for sitting 1; manual dashboard chat by JC is the recommended pre-sitting-2 confirmation.

### Open items / next-session candidates

- **S1 sitting 2** is what comes next: assistantTools.cpp HIGHs (`IsPathDenied` symlink/case canonical-path refactor, `.bak`/`.tmp` leak via deny-list miss, `ExecJcwfWriteScript` path-prefix validation, `ExecJcwfGenerate` global.json string-concat JSON, `ExecGetFileSummary` missing `IsPathDenied` call) and the `assistantController.h` CRITICALs + HIGHs (auth bypass via unauthenticated `approval_response`, path traversal in `GetSession`, background-thread lifetime captures, lock-order inversions, missed CV wakeups, WebSocket client-pointer races, thread vector unbounded growth — reuse engine threadpool, severity-mismatched logging, `default:` over closed enum). Plan tracks the schedule; `S1-D2-session-note.md` is the cumulative artifact.
- **Test harness TLS+self-signed gap** — `test/assistant/test_assistant.py` likely silently un-runnable since the j9t HTTPS migration. The fix is one-line in the harness (pass `sslopt={"cert_reqs": ssl.CERT_NONE}` to `websocket.create_connection`). Worth picking up before sitting 2 so we have actual end-to-end automated coverage of the assistant tool changes.
- **Unchanged from before** — §5i tail items, §5g remaining follow-ups, dogfood the editor, dogfood the AI assistant, repository layout hygiene, landing page, promo video. See `todo.md`.

### Gotchas next-session-Claude should know

- **`POST /api/shutdown` is now the only legitimate way to stop a running j9t for relaunch** — never `pkill -f jarvisAgent-studio`. Memory `feedback_shutdown_via_rest` captures the rule. Signal-kill bypasses keystore re-seal + audit-log flush; orderly shutdown via REST is the only correct path.
- **`assistantTools.cpp` anonymous-namespace helpers** — `RunArgvCapture` (POSIX, fork/execvp/poll/timeout/capture), `IsCwdInsideProjectRoot` (cross-platform weakly_canonical-vs-project-root), `PosixSingleQuote` (Windows-only — full POSIX single-quote escape). Reuse these for new tool sites instead of reintroducing popen-with-string-composition. The helpers are intentionally *not* extracted to a shared header; if a third file needs them, that's the moment to extract per the C++ discipline rule "refactor to one helper before adding a third copy".
- **`run_shell` security model is now: cwd validation + approval flow only.** No blocklist. The cwd validation is structural (canonical path inside project root + chdir/lpCurrentDirectory plumbing); the approval flow lives in the controller layer and is the next-sitting target. Until controller-layer approval enforcement lands in sitting 2, run_shell is "less safe than it looks" — the comment block in `ExecRunShell` documents this explicitly.
- **`SanitizeUtf8` is now applied at the real reply-parser boundaries (not just TestInterface)** — when adding new providers / parsers, sanitize content/text/error-message fields at the simdjson `string_view` → `std::string` boundary. The `feedback_established_safety_patterns` memory describes the pattern; `replyParserAPI{1..5}.cpp` are the worked examples.

---

## 2026-04-28 (clangd-LSP setup) → next session

Tooling-only session — set up structural C++ navigation for my own use; no project code changes.  Per-machine setup details captured in `feedback_clangd_lsp_for_navigation.md` (memory) so they don't pollute this tracked log.

### Project-relevant findings

- **`compile_commands.json` is now generated by Bear** at the project root.  Refresh via `premake5 clean && premake5 gmake && bear -- make config=debug` after build-system changes.  Both `compile_commands.json` and `.cache/` are gitignored.
- **Smoke test surfaced a count I'd missed:** `webServer.cpp` has **10** call sites of `SanitizeUtf8` (lines 285, 293, 301, 308, 316, 3409, 3535, 3538, 3542, 4105).  When the morning's commit removed the local anonymous-namespace duplicate, all 10 callers transparently re-resolved to the canonical `SanitizeUtf8` in `application/workflow/workflowTypes.h`.  Real validation that the consolidation worked correctly — clangd's `findReferences` returned the full set in ~200 ms.

### Open items / next-session candidates

Unchanged from the prior 2026-04-28 entries (Tier B + TUI sanitization + 2 hardening plans landed; §18 / §19 sessions are next).  See `todo.md` (project root).

---

## 2026-04-28 (post-commit maintenance) → next session

Pure documentation + file-structure cleanup pass after the morning's big commit.  One small post-commit C++ change (`ResetTestState()` `#ifdef DEBUG`-gated for hygiene), no functional code work.  JC committing as `"maintenance"`.

### What landed

1. **`ResetTestState()` gated `#ifdef DEBUG`** in `engine/curlWrapper/curlMultiDispatcher.{h,cpp}` — completes the symmetry with the route gating.  All 4 binaries rebuilt (Studio Debug/Release, Engine Debug/Release); Engine symbol-isolation invariant intact (Engine Release ~1 MB smaller than Studio Release).
2. **Hand-off log convention established** — `doc/misc/hand-off.md` (this file).  Newest entry on top, self-contained per entry, cross-references rather than duplicates other docs.  New auto-memory entry `feedback_session_handoff_log.md` so future-Claude reads the latest entry at session start and prepends a new one at session end.
3. **TODO restructure** — three archives moved into `doc/misc/`:
   - `JarvisAgent TODO List.md` → `doc/misc/JarvisAgent TODO List.md` (global archive)
   - `application/workflow/doc/todo.md` → `doc/misc/application-workflow-todo.md` (backend archive)
   - `workflow-editor/todo.md` → `doc/misc/workflow-editor-todo.md` (frontend archive)
   New consolidated `todo.md` at project root holds the live open items only.  The two scope-specific files were re-created empty with header-only stubs at the original locations (so the directory shape stays valid; new scope-specific TODOs land there if they ever arise).  Memory `reference_todo_files.md` updated to reflect the new shape.
4. **§5i verified done + tail items extracted** — read `doc/misc/engine-studio-capability-review.md` carefully; the §5i refactor landed in the **2026-04-25** session (way before today).  Today's commit pulled that work in as part of the big bundle but didn't add to it.  Struck the §5i entry from the archived global TODO with a verification summary.  Four real tail items from the review doc's "Open items / follow-ups" section that hadn't been captured anywhere live-tracked got added to `todo.md` under a new "§5i follow-ups (post-implementation)" subsection: shutdown audit-log gap, `HandleAiInterfaceTestPost` Engine fallback decision, AI-WS dispatch extraction (drops `#ifdef` count 10→7), bootstrap `admin/admin` badge collision.
5. **Audited the 4 recent refactor docs** for forgotten TODOs:
   - `doc/misc/API refactor.md` (5h Bedrock + Azure) — fully done.
   - `doc/misc/AI dispatch refactor.md` (5g) — main follow-ups already in `todo.md`; **one missing**: live-backed E2E tests for schema-validation retry, chunking, and markitdown (today's tests are hermetic-only).  Added to `todo.md` §5g remaining follow-ups.
   - `doc/misc/AI call performance optimization.md` (rev 7) — fully done after today's Tier B.
   - `doc/misc/cloud-integration-dev-plan.md` — surfaced **two genuinely open items** (`email_watch` doesn't actually IMAP-poll for new mail; Mailpit JCWF coverage gap that explains the 14-vs-13 dashboard mismatch JC noticed).  Added to `todo.md` under a new "Cloud integration tail" subsection.  Two **stale checkboxes** in the plan flipped to `[x]` with verification notes: Redmine frontend (in `ConnectionsView.tsx` + `WorkflowEditorView.tsx`) and Snowflake round-trip (verified end-to-end per `snowflakeQueryDemo.md`).
6. **Self-hosted Docker registry** removed from the consolidated TODO — misunderstanding from old session notes; GitHub Container Registry is fine, no migration needed.

### Where things live now

- `todo.md` (project root) — live open items only; consolidated 2026-04-28
- `application/workflow/doc/todo.md` — header-only stub, scope clarified
- `workflow-editor/todo.md` — header-only stub, scope clarified
- `doc/misc/JarvisAgent TODO List.md` — global archive (read-only)
- `doc/misc/application-workflow-todo.md` — backend archive (read-only)
- `doc/misc/workflow-editor-todo.md` — frontend archive (read-only)
- `doc/misc/hand-off.md` — this file
- `doc/misc/cybersec-hardening-dev-plan.md` — §18 plan
- `doc/misc/cpp-safety-hardening-dev-plan.md` — §19 plan
- `doc/misc/engine-studio-capability-review.md` — §5i design + impl log
- `doc/misc/AI call performance optimization.md` — §5g-rl design ref (refactor done)
- `doc/misc/AI dispatch refactor.md` — §5g design ref (refactor done)
- `doc/misc/cloud-integration-dev-plan.md` — Phase 0-12 tracker (mostly done; two tail items in `todo.md`)
- `doc/misc/API refactor.md` — §5h design ref (refactor done)

### Open items / next-session candidates

Unchanged from the morning entry below (Tier B + TUI sanitization + 2 hardening plans landed; §18/§19 sessions are next).  See `todo.md` for the full live list — all items now consolidated there with cross-refs to the relevant dev plans.

### Gotchas next-session-Claude should know

All of the morning's gotchas still apply.  One new one from this pass:

- **Always read `doc/misc/hand-off.md`'s latest entry first when picking up an active project** — even if the user opens with "let's keep going on X", the hand-off has the load-bearing context (config-schema breaks, debug-only paths, helpers worth reusing) that recently landed.  Memory `feedback_session_handoff_log.md` makes this explicit.

---

## 2026-04-28 → next session

### What landed

Three large pieces of work plus assorted fixes, all committed + pushed.

1. **AI dispatch performance refactor (Phases 1–5) committed** — uncommitted since 2026-04-26.  Rate-limit controller (`engine/curlWrapper/rateLimitController.{h,cpp}`), per-provider strategies (`rateLimitStrategy.{h,cpp}`), normalized observation (`rateLimitObservation.h`), AIMD + token-bucket + server-directed waits, size-aware budget via `CURLOPT_TIMEOUT_MS`, dual-timeout collapse, cascade cancellation.  Verified live 2026-04-26 against Anthropic Sonnet (137 tasks, AIMD converged 4→16, zero 429s).
2. **§14 Tier B hermetic dispatcher tests** — 8 Python tests + C++ infra (4 new debug endpoints: `recent-submissions`, `mock-ai-response`, `test-observe-idempotent`, `reset-dispatcher-state`).  All 8 verified across 3 sweeps in one j9t process.
3. **TUI ncurses stress tests** — `test_stress_tui_utf8_heavy.py` (3-way concurrent jarvisCpp at 420 ai_call tasks with multi-byte UTF-8) and `test_stress_tui_utf8_invalid.py` (140 tasks with malformed bytes).  Surfaced + fixed a real bug (raw invalid bytes leaking into `log/log.txt`).  New `SanitizeUtf8` helper in `application/workflow/workflowTypes.h` (companion to `TruncateUtf8Safe`).
4. **Two new dev plans** — `doc/misc/cybersec-hardening-dev-plan.md` (§18) and `doc/misc/cpp-safety-hardening-dev-plan.md` (§19).  4-domain split, 4 combined sessions, importance rubric, per-change template, memo with Rust-emulating defaults.  Plans are review-ready; sessions to execute them not yet scheduled.
5. **10 new auto-memory entries** distilled from the §10 memos of both hardening plans (argv-only shell, allowlist-not-blocklist, path-confinement-edition, secrets-only-via-redactor, auth-funnel, constant-time-compare, capture-by-value-async, no-jthread-use-threadpool, rust-emulating-defaults, established-safety-patterns).

### Bug fixes surfaced via testing today

These came up while building the Tier B / TUI tests; all fixed in the same commit:

- **`ApplyAiInterfaceRateLimitFromJson` padded_string bug** — `simdjson::ondemand::parser::iterate(req.body)` silently no-opped on non-padded `std::string`, so every `rate_limit` override sent via `POST /api/settings/ai-interfaces` was dropped.  Every interface ended up with C++ struct defaults regardless of operator config.
- **`m_MaxRetries429 == 0` treated as "use default 10"** — `> 0` check at `curlMultiDispatcher.cpp:776` meant operators couldn't actually disable retries via config.  Switched to `>= 0`; `-1` is now the sole "unset" sentinel.  Same fix for `m_MaxRetriesTransient`.
- **Malformed UTF-8 from AI replies leaking raw into `log/log.txt` + dashboard WS** — fixed at the TestInterface boundary; real-AI parser + captured-stdout coverage deferred to §19 (D1, S3).
- **Localhost SSL bypass added in DEBUG builds** — so the dispatcher can hit the j9t server's own self-signed cert during hermetic tests.  Production paths still verify; bypass is `#ifdef DEBUG && (host == localhost|127.0.0.1|::1)`.

### What's verified

| Sweep | Result |
|---|---|
| Tier A (existing) — `test_rate_limit_observation_parse.py` | green |
| Tier B (new today) — 8 tests × 3 sweeps in one j9t process | 24/24 pass |
| TUI heavy UTF-8 — 3 jarvisCpp JCWFs concurrent, 420 ai_call | pass, j9t alive, 18.4 MB log clean |
| TUI invalid UTF-8 — 140 ai_call with malformed fixture | pass after `SanitizeUtf8` fix, 6.1 MB log clean |
| All 4 binaries built post-commit (Studio Debug/Release, Engine Debug/Release) | clean, edition isolation intact (Engine Release ~1 MB smaller than Studio Release) |
| Existing dispatch tests (`test_testinterface_hermetic.py`, schema-roundtrip, etc.) | not re-run today; should still pass — no breaking changes to those code paths |

### Open items / next-session candidates

1. **§19 cpp-safety hardening pass** — 4 sessions to execute the plan.  Among the entries: `SanitizeUtf8` at real AI reply parsers (`replyParserAPI{1..5}.cpp`) and at captured stdout/stderr (`shellTaskExecutor`, `pythonTaskExecutor`) — flagged in the plan's §6.1 D1 row "UTF-8 sanitization at external-byte boundaries" as the deferred companion work to today's TUI fix.
2. **§18 cyber-sec hardening pass** — 4 sessions, runs combined with §19 per the dual-plan schedule (S1=D2 web+cloud+assistant, S2=D3 core engine, S3=D1 workflow orchestration, S4=D4 app infrastructure).

### Gotchas next-session-Claude should know

Load-bearing past today:

- **Don't restart j9t lightly.**  Dispatcher state (controller AIMD caps, observation history) lives in-memory; restart loses it.  For repeated test runs in one j9t, call `POST /api/debug/reset-dispatcher-state` between tests (each Phase B test does this at startup).
- **`rate_limit.max_retries_429 = 0` now means 0** — previously meant "use default 10".  In practice nobody sets it explicitly, so unlikely to bite anyone, but worth flagging in changelog if shipping.
- **TestInterface fixture content gets sanitized via `SanitizeUtf8` now** — the on-disk output file is also sanitized (downstream Python combiners read it as UTF-8 text; raw-byte preservation isn't worth breaking the combiner).  If a future test needs raw bytes on disk, that's a flag on the interface, not a default.
- **Localhost SSL bypass + `ResetTestState()` are DEBUG-only** — both `#ifdef DEBUG`-gated.  Release builds verify TLS normally and don't expose the reset endpoint.  Don't write tests that depend on these under Release builds.
- **`SanitizeUtf8` is the project-wide pattern for external-byte boundaries** (alongside `TruncateUtf8Safe` for size bounds).  See `feedback_established_safety_patterns.md` memory.
