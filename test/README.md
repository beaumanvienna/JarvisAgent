# JarvisAgent Workflow Test Runner

Automated test harness for `.jcwf` workflows. Cleans, runs, polls, and verifies each workflow via the REST API.

## Quick start

```bash
# 1. Start JarvisAgent in a separate terminal
cd /path/to/jarvisAgent && ./bin/Release/jarvisAgent

# 2. Run all configured workflows
python3 test/run_tests.py --all

# 3. Or test a single workflow
python3 test/run_tests.py --workflow make-example

# 4. List available workflows
python3 test/run_tests.py --list
```

## What the script does

1. **Health check** — verifies JA is reachable, prints status (registered workflows, active runs, session managers).
2. **Workflow folder check** — if `workflows/` has no `.jcwf` files, offers to copy `make-example.jcwf` as a starter.
3. **Clean prompt** — asks whether to wipe all workflow outputs before testing.
4. **Test list** — prints the ordered list of workflows to run, then waits for confirmation.
5. **Per-workflow loop** — for each workflow: start via REST → poll until done/timeout → verify expected artifacts (file existence, minimum sizes).
6. **Final report** — pass/fail/skip summary.

All terminal output is also written to `test/log.txt` (ANSI codes stripped).

## Files

| File | Purpose |
|------|---------|
| `run_tests.py` | Main test runner script |
| `test_config.json` | Per-workflow config: context, timeouts, prerequisites, expected artifacts |
| `log.txt` | Auto-generated log of the last test run (git-ignored) |

## Configuration (`test_config.json`)

Each workflow entry supports:

- **`context`** — JSON object passed to `POST /api/workflows/<id>/run`
- **`timeout_sec`** — max seconds to wait for completion
- **`prerequisites`** — files that must exist before running (use `__polarion_mock__` for service checks)
- **`expected_artifacts`** — list of `{ glob, min_bytes, min_count }` to verify after completion

## IDE-assisted testing (Windsurf / Cascade / Cursor)

The script is designed for a human + AI pair-programming workflow:

1. **You** start JarvisAgent in a terminal (needed for the ncurses TUI).
2. **You** run `python3 test/run_tests.py --all` in another terminal.
3. The script pauses after listing workflows — at this point, tell the AI to `tail -f test/log.txt` so it can follow progress.
4. **You** press Enter to begin.
5. The AI reads `test/log.txt` and the generated artifacts to analyze results, spot issues, and suggest fixes.

This gives the AI full visibility into test execution and artifact content without needing direct terminal access or stdin interaction. The dashboard at `http://localhost:8080` remains the primary real-time monitor.

## Dependencies

- Python 3.8+
- `requests` (`pip install requests`)
- JarvisAgent running with the REST API on `localhost:8080` (configurable via `--base-url`)
