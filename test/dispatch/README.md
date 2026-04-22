# Dispatch contract tests

Tests that exercise the envelope-direct AI dispatch path end-to-end against a
running JarvisAgent instance.

## Running

All tests assume a running Studio build on the default port (8443, self-signed
TLS — scripts pass `--insecure` / `verify=False`).

Offline / no-AI:
```
python3 test/dispatch/test_schema_covers_parser.py
python3 test/dispatch/test_envelope_empty_body_rejected.py
```

Live AI (`--with-ai` style — costs real tokens):
```
python3 test/dispatch/test_api4_anthropic_live.py
```

The Anthropic live test uses the `api.anthropic.com/claude-opus-4-7/API4`
interface configured in `config.json`.  It submits an adhoc ai_call and asserts
a well-formed reply lands at `<prob>.output.txt` inside the run folder.
