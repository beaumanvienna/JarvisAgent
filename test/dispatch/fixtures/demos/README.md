# Demo JCWF Mock Fixtures

Curated canned responses for running JCWFs end-to-end via `is_mock: true`
provider interfaces — proves workflow-shape correctness (dataflow + dependencies
+ fan-out + downstream shell/python tasks) without burning real provider credit.

Each subfolder holds a `golden_success.json` keyed to one JCWF.  The driver
pattern: configure a mock interface in `config.json` (or via
`POST /api/settings/ai-interfaces` with `is_mock: true` + `fixture_path`
pointing at the fixture here), then trigger the JCWF via the launcher / REST /
MCP.  All `ai_call` tasks routed through that interface receive the canned
body and run through the real `ReplyParserAPI1` parser — same code path as a
live OpenAI Chat dispatch, just without the network call.

## Mockable demos

| Demo | Fan-out | Notes |
|---|---|---|
| `aiZipDemo` | 3 single-mode `ai_call` tasks | Single canned body covers all three.  Provision three separate interfaces if you want per-task content variation; not required for shape correctness. |
| `bookSummaryPipeline` | 1 `per_item` `ai_call` task | Same canned body replays per chapter.  Fan-out cardinality + per-prob `.output.txt` write path are what the demo verifies — content variation is decorative. |

## Not mockable

These JCWFs feed the AI reply into `g++` / `python -c` so the canned response
must be syntactically valid compilable code that *adapts to its input*.  A
fixed canned reply cannot satisfy this:

| Demo | AI consumer | Why not mockable |
|---|---|---|
| `jarvisCpp*` (cyber-sec / safety / docu audits) | The AI replies feed downstream Python tasks that grep specific patterns, format JSON sections, and join cross-references between audit chunks.  Real outputs vary per source file; one canned body would either pass every grep (false-positive flood) or fail every grep (no signal). |
| `python_codegen_*` (if/when added) | Reply must be a Python source file the next shell task `python -c`'s.  Canned reply only works for the one specific input that was used to capture it. |
| Any JCWF whose downstream task is `g++ <reply>` or equivalent | Same as above for C++ source. |

A future enhancement could allow a per-item lookup table (fixture indexed by
PROB content hash), at which point some of these become semi-mockable.
