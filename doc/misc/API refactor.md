# API refactor — AWS Bedrock + Azure OpenAI adapters

Plan for adding two new AI provider adapters to j9t. Tracks TODO list section 5h.

## Sequencing

1. **Azure first** (~1 day). Tiny lift, validates the new `AuthStyle` slot. Body identical to API1.
2. **Bedrock second** (~3-5 days). SigV4 signing is the hard part; per-model-family body switch is the second.

## Shared scaffolding (touched by both)

- `engine/json/configParser.h` — add `API5_Bedrock`, `API1_Azure` to `InterfaceType` enum (lines 41-50). Bump `NumAPIs`.
- `engine/json/configParser.cpp` — extend the if-else chain at lines 523-546 (parse) and 555-574 (serialize). Strings: `"API5"` and `"API1Azure"` (final names per Q1).
- `engine/curlWrapper/curlWrapper.h` — extend `AuthStyle` enum (lines 71-76) with `AzureApiKey` and `AwsSigV4`. Enum stays as the factory key for `IAuthSigner` (see Abstractions). The header-composition branches in the curl pool collapse into a single `signer->Apply(req, cfg)` call.
- `application/json/requestBuilder.cpp` — extend factory switch (lines 164-180) to dispatch new variants.
- `application/json/replyParser.cpp` — extend factory switch (lines 55-89).

## Azure OpenAI (5h.2) — implemented

- `application/json/requestBuilder.cpp` — `RequestBuilderAPI1Azure` inherits from `RequestBuilderAPI1` (`final` removed from base). Overrides only `GetAuthStyle()` → `AzureApiKey`. `BuildBody` and `ResolveUrl` are inherited unchanged.
- **URL composition lives on the interface, not the provider.** The user puts the full deployment URL (`https://{resource}.openai.azure.com/openai/deployments/{deployment}/chat/completions?api-version={ver}`) in `iface.m_Url`. Same shape as how Anthropic/OpenAI interfaces work today. `ProviderConfig::m_Params` stays empty for Azure — it's reserved for Bedrock's per-request secret material.
- Reply parser: factory maps `API1Azure` → `ReplyParserAPI1`. Bodies are byte-identical, no new parser.
- Dashboard UX (task #13): the Azure form will collect `resource` / `deployment` / `api_version` and compose the URL client-side before POSTing to `/api/settings/interfaces`. Backend stays provider-agnostic.

## AWS Bedrock (5h.1)

- `application/json/requestBuilderAPI5.{h,cpp}` (new files) — `RequestBuilderAPI5`. URL template `https://bedrock-runtime.{region}.amazonaws.com/model/{modelId}/invoke`. Per-family body selector dispatches on `modelId` prefix:
  - `anthropic.claude-*` → wrap API4-shaped body
  - `meta.llama*` → Llama-native body
  - `amazon.titan*` / `amazon.nova-*` → Titan/Nova body
- `application/json/replyParserAPI5.{h,cpp}` (new files) — unwrap Bedrock envelope, delegate to existing family parser by sniffing the same `modelId`.
- **SigV4 signing** — new `SigV4Signer : IAuthSigner` in `engine/curlWrapper/awsSigV4.{h,cpp}`. Hand-rolled on top of OpenSSL (already vendored: HMAC-SHA256, SHA256). No new vendor dep. The curl pool calls `signer->Apply(req, cfg)` polymorphically — no special-case branch for SigV4.
- Connection params: `region`, `access_key_id`, `secret_access_key`, optional `session_token`. Extend `ProviderConfig` accordingly.

## Dashboard UI (`shared-ui/views/ProvidersSettingsView.tsx`)

The current form is credentials-only (`name`, `credential_type`, secret fields). Backend `ProviderConfig` already carries `endpoint` / `api_type` / `default_model` / a new `params` map, but the UI doesn't expose them — works today only because `name` → preset is inferable for OpenAI/Anthropic/Gemini. Azure and Bedrock are per-tenant, so this breaks.

- **API type selector.** Add an `api_type` dropdown (or "preset" dropdown that pre-fills `api_type` + endpoint template) covering existing types plus `API1Azure` and `API5`.
- **Conditional params editor.** Mirror the pattern in `shared-ui/views/ConnectionsView.tsx` (line 489-490 — `editing.params.region` bound to a free-text input).
  - When `api_type === "API1Azure"`: inputs for `resource`, `deployment`, `api_version`.
  - When `api_type === "API5"`: input for `region`.
- **`shared-ui/api/providers.ts`** — extend `ProviderEntry` and `ProviderCreateInput` with `params?: Record<string, string>`. Backend `keyManager` round-trips it to `ProviderConfig::m_Params`.
- **AWS dual-secret credential.** Bedrock needs `access_key_id` + `secret_access_key` (+ optional `session_token`). Doesn't fit the existing `CredentialType` enum (`api_key | oauth | key_pair | credentials`). Add `"aws"` variant with two-input rendering. Touches `engine/keys/keyManager.{h,cpp}` and the providers REST endpoint, not just UI.

Workflow editor: provider selection in `ai_call` nodes is by interface name (string), provider-agnostic — **no changes**.

## Test infra

Both providers are tested against public Docker mocks, kept in `docker-compose.example.yml` alongside the existing services. Live-against-real-cloud tests stay possible via env-var override but are not the default path.

- **Azure OpenAI mock:** [`microsoft/aoai-api-simulator`](https://github.com/microsoft/aoai-api-simulator) (Microsoft first-party). Add as a service; Azure live test points at the simulator URL by default. Validates URL composition (`{resource}.openai.azure.com/openai/deployments/{deployment}/chat/completions?api-version=...`) + `api-key:` header + OpenAI-compatible request/response body.
- **Bedrock mock:** LocalStack Hobby tier ([Bedrock docs](https://docs.localstack.cloud/aws/services/bedrock/)). Free for non-commercial / OSS dev. Requires `LOCALSTACK_AUTH_TOKEN` env var — document in README under test setup, never commit. Bedrock-in-LocalStack runs models via Ollama under the hood; covers `Invoke`, `Converse`, and Batch APIs. SigV4 signing is exercised end-to-end against LocalStack's AWS-compatible front door.

## Tests

- `test/dispatch/test_api1azure_live.py` — mirror `test_api4_anthropic_live.py`. Default endpoint = `aoai-api-simulator` service; `J9T_AZURE_OPENAI_ENDPOINT` env var to override against real Azure.
- `test/dispatch/test_api5_bedrock_anthropic_live.py` and `test_api5_bedrock_llama_live.py` — exercise both family-selectors against LocalStack's Bedrock endpoint. `J9T_BEDROCK_ENDPOINT` env var to override against real AWS.
- C++ unit test for SigV4: known-answer test against [AWS's published canonical-request example vectors](https://docs.aws.amazon.com/general/latest/gr/sigv4-signed-request-examples.html). Must land before any Bedrock live test — easy to break, hard to debug live.

## Docs

- `README.md` lines 52-65 — extend the API-type table, drop the roadmap note (was: "Bedrock+Azure when enterprise needs it").
- `doc/jarvisagent.md` — add the two endpoints + auth style.
- `doc/architecture.md` line 48 table — note Bedrock's per-family parser delegation.
- `doc/api-endpoints.md` — example interface definitions for both.
- `doc/jcwf_generation_guide.md` + `doc/sub-jcwf_generation_guide.md` — provider-agnostic by interface-name; **no changes** unless the AI generator needs to know they exist (worth a single example mention).
- `JarvisAgent TODO List.md` — strike 5h.1 and 5h.2 once landed.

## Packaging

- `premake5.lua` — only if Bedrock SigV4 lives in new `.cpp` files: add to source list (probably auto-globbed — verify).
- `jarvisagent.sh` — no changes (provider-agnostic).
- Installer / Windows VS2022 — no changes (premake regenerates).

## Verified during sweep

- `premake5.lua` auto-globs `engine/**` and `application/**` — new files just live under the trees, no manifest edits.
- CI (`linux-workflow.yml`) is `--help` smoke only; no docker-compose or unit-test step. Mock infra is dev-time only.
- Engine edition strips assistant + aiJcwfService; AI request pool and provider code stay in both editions.
- `keys.json.enc` parser version-checks and ignores unknown fields — adding `m_Params` is backward-compat, no version bump.

**Ramification — `SecretRedactor` integration.** Bedrock's `secret_access_key` and `session_token` must be registered via `SecretRedactor::Get().AddSecret(...)` on `KeyManager::Load`, mirroring the OAuth token path in `engine/keys/oauthTokenManager.cpp`. Otherwise they leak into `log/log.txt`. Folds into tasks #1 and #14.

## Decisions

1. **Azure:** new `InterfaceType` (`API1_Azure`). Factory switch stays the single source of truth; no special-case branches inside `RequestBuilderAPI1`.
2. **Bedrock SigV4:** hand-rolled on top of OpenSSL primitives. No `aws-sdk-cpp` vendor dependency.
3. **Connection params:** introduce a `params` map on `ProviderConfig` (`std::unordered_map<std::string, std::string>`). Keeps `ProviderConfig` from growing per-provider; both Azure (`resource`, `deployment`, `api_version`) and Bedrock (`region`, `access_key_id`, `secret_access_key`, `session_token`) read from it.

## Abstractions

**Introduce `IAuthSigner`.** Today `AuthStyle` is a flat enum and the curl pool branches on it to prepend a static header. SigV4 breaks that model — it has to read the full request (method, URL, headers, body bytes), SHA256 the body, 4-step-derive an HMAC signing key, and emit three headers. Cramming it into another enum branch sprinkles signing logic inside `CurlWrapper`.

```cpp
class IAuthSigner {
public:
    virtual ~IAuthSigner() = default;
    virtual void Apply(HttpRequest& req, const ProviderConfig& cfg) = 0;
};
```

- `BearerSigner`, `XGoogApiKeySigner`, `AnthropicApiKeySigner` — trivial; existing branches move here.
- `AzureApiKeySigner` — trivial (`api-key:` header).
- `SigV4Signer` — heavy but isolated and unit-testable in one place.
- `AuthStyle` enum stays as the **factory key** — parsed from config, dispatched to `IAuthSigner::Create()`.

Pays for itself again whenever the next enterprise provider lands with OAuth/IAM (Vertex AI, Anthropic-on-Vertex, AWS IAM-via-IRSA): curl pool stays untouched, write a new signer.

**Explicitly *not* introducing:**
- Per-family Bedrock body/reply dispatch (`IBedrockFamilyBuilder`). Handle the `modelId` prefix switch as private free functions inside `requestBuilderAPI5.cpp` / `replyParserAPI5.cpp`. Only one provider would ever use it.
- Typed variant for provider params (`std::variant<NoExtra, AzureConfig, BedrockConfig>`). Map is fine; revisit if a 6th provider piles on >5 extra keys.
