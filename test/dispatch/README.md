# Dispatch contract tests

Tests that exercise the envelope-direct AI dispatch path end-to-end against a
running JarvisAgent instance.  Each script is self-contained, exits 0 on
success and non-zero on failure, and can be run individually or stitched into
a higher-level harness.

## Running

All tests assume a running Studio build on the default port (8443, self-signed
TLS — scripts pass `verify=False`).  Most tests require an MCP admin key:

```
export J9T_TOKEN=mcp_...                 # or pass --token on the CLI
```

### Offline / no live AI

These tests drive the refactored dispatch end-to-end without any network
call outside localhost.  The `TestInterface` (Phase 7) is used as a hermetic
AI backend — it short-circuits the curl dispatcher and returns a canned
reply from a fixture file.

```
python3 test/dispatch/test_schema_covers_parser.py        # JCWF schema ⊇ parser
python3 test/dispatch/test_direct_dispatch_signals.py     # refactor signals + no-legacy fields
python3 test/dispatch/test_envelope_empty_body_rejected.py# empty prompt -> Failed
python3 test/dispatch/test_testinterface_hermetic.py      # Test interface byte-exact round-trip + PROV sidecar
python3 test/dispatch/test_relaxed_env_warnings.py        # Phase 1 relaxed env (STNG/TASK/CNTX optional)
python3 test/dispatch/test_chunking_fanout.py             # Phase 6 chunked fan-out + reduce pass
python3 test/dispatch/test_markitdown_cntx.py             # CNTX office-file auto-conversion
python3 test/dispatch/test_cross_workflow_parallel.py     # cross-workflow concurrency (2026-04-22 bug)
```

The chunking and markitdown tests use the hermetic Test interface too —
no network.  Markitdown requires the `markitdown` CLI on PATH (pip install
markitdown).  An 8 MB sample PDF at `workflows/in.pdf` is the input
fixture.

The hermetic / relaxed-env tests create a transient `InterfaceType::Test`
entry via `POST /api/settings/ai-interfaces`, run, and `DELETE` it on exit.
They never call `/api/settings/ai-interfaces/save`, so `config.json` on disk
stays untouched.

### Live AI (`--with-ai` style — costs real tokens)

```
python3 test/dispatch/test_api4_anthropic_live.py            # Anthropic end-to-end
python3 test/dispatch/test_output_schema_roundtrip.py        # Phase 3 schema-enforced output
```

The Anthropic live test uses the `api.anthropic.com/claude-opus-4-7/API4`
interface configured in `config.json`.  It submits an adhoc ai_call and
asserts a well-formed reply lands at `<prob>.output.txt` inside the run
folder.

The schema-roundtrip test defaults to `api.openai.com/gpt-4.1-mini/API1`,
sends a prompt with `output_schema`, and asserts the produced
`<prob>.output.json` is valid JSON matching the declared schema, plus that
`ai_structured_submissions` incremented and `ai_schema_validation_retries`
stayed within the `output_retries` budget.

## Fixtures

* `fixtures/hermetic_reply.txt` — canned reply for the TestInterface.  The
  hermetic + relaxed-env tests assert `<prob>.output.txt` is a byte-exact
  copy of this file.  Do not add trailing whitespace or encoding BOMs.
