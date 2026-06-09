# Whole-System STRIDE Pass — Development Plan

**Status:** COMPLETE — Sittings 1–3 done 2026-06-07; `doc/threat-model.md` §1–§4 populated (full STRIDE walk, boundaries 1–11 + data stores).  No CRIT/HIGH.  F2 (cookie `Secure`) verified a non-finding — already mitigated in shipped code; F3 (stale shell-args comment) fixed.  F1 (webhook replay) accepted for 1.0 as AR-6.  No open findings; six accepted risks recorded (AR-1…AR-6).  The pass is closed.
**Target:** j9t 1.0 — the system-level security pass that closes out the pre-1.0 security list.
**Companion docs:** `doc/cyber security.md` (per-subsystem security controls), `doc/architecture.md` ("Key Design Decisions" already carries much of the trust-boundary rationale).
**Distinct from:** `cybersec-hardening-dev-plan.md` (§18) and `cpp-safety-hardening-dev-plan.md` (§19), which are per-file passes and are COMPLETE.

---

## Deliverables — three documents, three audiences

This pass produces / touches three distinct docs.  Do not conflate them:

1. **This plan** — `doc/misc/stride-pass-dev-plan.md` — *internal*: the methodology + sittings.  How we run the pass.  (`doc/misc/` = internal working docs.)
2. **The official analysis** — **`doc/threat-model.md`** ("Threat Model & Vulnerability Analysis (STRIDE)"; rename later if desired) — *external auditor–facing*: the OUTPUT.  The DFD, the trust-boundary + data-at-rest inventory, the STRIDE-per-element matrix, findings, and the accepted-risk register.  Polished, self-contained, top-level `doc/` — an artifact you can hand to an external security auditor.  **Created 2026-06-07 with §1–§2 (the model) populated; §3 (STRIDE walk) + §4 (findings) under construction.**
3. **The controls narrative** — `doc/cyber security.md` *(existing)* — gets a top-level section cross-referencing the analysis: "System-level threat model — see [the analysis doc]."  cyber security.md stays the *by-subsystem control* reference (SecureString, MCP key lifecycle, the SSRF cluster, …); the analysis doc is the *by-element threat enumeration*.  They cite each other: the analysis links cyber security.md for each mitigation's detail; cyber security.md links up to the analysis for the system view.

Relationship in one line: **cyber security.md = "here are our controls"; the analysis doc = "here is every threat we systematically considered and how each is covered."**

---

## Why this is not the per-file audit

The per-compilation-unit cyber-sec audit (`jarvisCppCyberSecAudit`) reviews each `.h`/`.cpp` in isolation, so it **structurally cannot** surface emergent / architectural / data-at-rest issues — no single file's review sees the aggregate.  It never flagged that the old `connections.json` held plaintext endpoint URLs + credential references on disk, because that exposure only exists at the *system* level.

STRIDE is **model-driven, not file-driven**: build a system model (data-flow diagram + trust boundaries + data-at-rest inventory), then walk threats per model element.  The whole point is to catch what isolated file review can't.

**Acceptance bar (the regression test):** the pass must independently re-derive the plaintext-`connections.json`-class exposure (now fixed) — i.e. seed that historical state into the model walk and confirm the method flags it.  If it wouldn't have caught that, the method is wrong.

---

## The six STRIDE categories

STRIDE (Microsoft; Kohnfelder & Garg, 1999) is a mnemonic for six threat categories — each the violation of one security property:

|   | Threat | Violates |
|---|---|---|
| **S** | Spoofing — pretending to be someone/something else | Authentication |
| **T** | Tampering — unauthorized modification of data/code | Integrity |
| **R** | Repudiation — denying an action with no way to prove it happened | Non-repudiation |
| **I** | Information disclosure — exposing data to those not authorized | Confidentiality |
| **D** | Denial of service — degrading/blocking availability | Availability |
| **E** | Elevation of privilege — gaining capabilities you shouldn't have | Authorization |

---

## Methodology — model first, then STRIDE-per-element

Classic Microsoft-style STRIDE.  The hard, high-value work is building the model; once it exists, the threat walk is mechanical.

### Phase 1 — Build the model

**1a. Data-at-rest inventory** — every persisted datum: where it lives, in what form, who can read it, what protects it.  Draft skeleton (verify + complete during the pass):

| Store | Holds | Form at rest | Protected by |
|---|---|---|---|
| `keys.json.enc` | AI provider keys, OAuth access/refresh tokens | AES-256-GCM | master password |
| `mcp_keys.json.enc` | MCP API keys + enrollments | AES-256-GCM | master password |
| `API.json.enc` | AI routing config (interfaces, default/jcwf selectors) | AES-256-GCM | master password |
| `connections.json.enc` | cloud connection configs (endpoints + key refs) | AES-256-GCM | master password |
| `config.json` | ports, folder paths, TLS cert/key paths, non-secret scalars | **plaintext** | filesystem perms only |
| `certs/j9t-key.pem` | **TLS private key** | plaintext PEM | filesystem perms only |
| `workflows/*.jcwf` + extracted `workflows/<id>/` | workflow definitions + bundled input data | plaintext zip / files | filesystem perms only |
| `queue/<wf>/<task>/` (STNG / CNTX / TASK / PROB / `*.output.*`) | task I/O — **prompts + outputs may carry PII or template-substituted secrets** | plaintext | filesystem perms only |
| `log/log.txt`, `log/security.txt` | application + security logs (secret-redacted) | plaintext | redactor + filesystem perms |
| `_adhoc/<user_slug>/...` | per-user adhoc run scratch (own `workflows/` + `queue/`) | plaintext | per-tenant dir + TTL reaper |
| `.email_watermarks.json` | IMAP UID watermarks per email-watch trigger | plaintext | filesystem perms only |
| session cookie | authenticated browser session | HttpOnly cookie | SameSite + TLS |

For each store, the walk asks: who can read/write it; is the form appropriate for the sensitivity; what happens if the box is compromised at the FS level; does it survive into a future SaaS/multi-tenant deployment.

**1b. Trust-boundary map** — enumerate every boundary a datum or request crosses:

1. Browser ↔ j9t REST/WS (session cookie, TLS, CSRF/SameSite posture)
2. MCP agent / sidecar ↔ j9t (Bearer MCP key)
3. Webhook caller ↔ j9t (HMAC-signed trigger)
4. j9t → AI providers (outbound; `IAuthSigner`, `UrlPolicy`, loopback guard)
5. j9t → cloud connectors (outbound; `ConnectorHttp` SSRF gate, credentials)
6. j9t ↔ embedded Python engine (script execution; `pathConfinement`, sys.path)
7. j9t ↔ shell tasks (argv-only; `scripts/`-gated, no `system()`/`popen()`)
8. j9t ↔ filesystem (path confinement; `ConfineUnderProjectRoot` / extraction)
9. Studio vs Engine **edition capability boundary** (removefiles isolation, capability gates)
10. **Single-box (`127.0.0.1`-trusted) vs future SaaS / multi-tenant** — the boundary that's implicit today and the biggest assumption to make explicit
11. operator / viewer / adhoc-user **privilege tiers** (auth funnel, run-control gating)

**1c. Data-flow diagram** — external entities → processes → data stores, annotating which flows cross which of the boundaries above.  Processes: WebServer, WorkflowRuntime, TriggerEngine, AiRequestPool/dispatcher, PythonEnginePool, CloudConnectors, KeyManager.  External entities: browser user, MCP agent, webhook source, AI provider, cloud service, JCWF author.

**1d. Actor / adversary model** — external network attacker; malicious authenticated user (viewer / operator); malicious JCWF author; malicious AI-provider response; malicious cloud endpoint; local-FS attacker; compromised dependency.

### Phase 2 — STRIDE per element

Each DFD element type maps to a fixed set of threat categories (the standard mapping):

| Element | S | T | R | I | D | E |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| External entity | ✓ | | ✓ | | | |
| Process | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Data flow | | ✓ | | ✓ | ✓ | |
| Data store | | ✓ | ✓ | ✓ | ✓ | |

Per element × applicable threat: state the **existing mitigation** (cite the code site + the `cyber security.md` section) **or flag a gap**.  Worksheet row format:

> `<element>` — `<S/T/R/I/D/E>` — *threat statement* — **Mitigation:** `<code site>` / `cyber security.md §<x>`  **|  Gap:** `<none | description + severity>`

### Phase 3 — Findings → triage → remediation

Collect the gaps, severity-rank (CRIT/HIGH/MED/LOW), and emit a remediation list that becomes the next hardening cohort.  Explicitly record **accepted risks** (e.g. the single-box FS-trust model, `run_shell` forever-HIGH) so they're decisions, not oversights.

---

## Execution options

1. **Solo, doc-driven (recommended).**  Claude builds the model + runs the STRIDE walk into the official analysis doc (deliverable #2), citing code/cyber-sec per element, flagging gaps.  Whole system fits in context; strongest at citing actual code.  ~2–3 focused sittings.
2. **AI-JCWF mode.**  Extend `scripts/buildJarvisCppDocu.py` with a `--mode stride` fed `architecture.md` + the inventory.  Not a per-file fan-out → a few holistic AI calls; weaker at citing real code; the model still has to be hand-built first.  Lower value.
3. **Multi-agent workflow** (requires explicit opt-in — "ultracode" / "use a workflow").  One agent per trust boundary builds + STRIDE-walks its slice in parallel; adversarial verifiers challenge every "this is mitigated" claim; synthesis stage.  Most thorough/confident, highest token cost.  STRIDE decomposes cleanly by boundary, so this fits well if maximal assurance is wanted.

**Recommended sequencing:** option 1, and ship **Phase 1 (the model) as a standalone artifact for review first** — the model is where the insight lives and it's cheap to sanity-check before the (mechanical) threat walk runs over it.

---

## Sittings (option 1)

- ~~**Sitting 1 — the model.**~~  DONE: `threat-model.md` §2 (deployment assumption, actors, 11 trust boundaries, DFD, data-at-rest inventory).
- ~~**Sitting 2 — STRIDE walk, inbound + at-rest.**~~  DONE: §3.1–3.8 (boundaries 1–3, 8–11 + data stores); surfaced F1, F2 + AR-1…AR-3.
- ~~**Sitting 3 — STRIDE walk, outbound + execution + close.**~~  DONE: §3.9–3.12 (AI / cloud / Python / shell) — all mitigated; F3 tidy-up + AR-4, AR-5; §4 findings/triage; the `doc/cyber security.md` cross-ref (deliverable #3) landed.  F2 verified already-mitigated; F3 comment fixed; F1 accepted as AR-6.  Pass closed — no open findings.

Estimate: ~2–3 sittings.  Output: the official analysis doc (deliverable #2) + the `cyber security.md` cross-ref (deliverable #3) + a remediation cohort.

---

## Acceptance criteria

- [ ] Complete data-at-rest inventory — every persisted datum, its form, and its protection.
- [ ] Every DFD element walked for its applicable STRIDE categories, each with mitigation-or-gap.
- [ ] **Regression:** the method independently flags the plaintext-`connections.json`-class exposure when that historical state is seeded.
- [ ] The single-box-vs-SaaS trust assumption is made **explicit** (the model's biggest implicit boundary).
- [ ] Each gap → severity + remediation; accepted risks recorded as decisions.
- [ ] **Deliverable #2 is self-contained and external-auditor-presentable** — no reliance on internal context; defines its own scope, diagram, and terminology.
- [ ] **`doc/cyber security.md` cross-references the analysis** (deliverable #3) so the two docs are linked, not orphaned.
