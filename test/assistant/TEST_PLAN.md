# AI Assistant Test Plan

**Status:** Complete — 70/70 passing
**Coverage baseline:** 70 tests in `test_assistant.py` (28 non-AI, 42 require `--with-ai`)

Run the suite:
```bash
# Start j9t, then:
python test/assistant/test_assistant.py              # 28 non-AI tests
python test/assistant/test_assistant.py --with-ai   # all 70 tests (some AI tests skipped without --auto-approve)
python test/assistant/test_assistant.py --with-ai --auto-approve  # all 70 tests including mutating tools
```

---

## What the tests cover

### Non-AI tests (28)

| Area | Tests |
|------|-------|
| Connection & session | Connect, new session, list sessions, resume session |
| Slash commands | /help, /status, /runs, /log, /memory, /index, /sessions, /new, /clear |
| Slash command edge cases | /memory clear, /index rescan, /log 999 (large N), /log abc (invalid arg) |
| Session behaviors | History replay on resume, turn count in session list, resume unknown session ID |
| Completions | /he → /help, /mem → /memory, empty prefix |
| History (Ctrl+R) | Retrieve entries |
| Protocol edge cases | Unknown message type, malformed JSON, empty text, stale approval_response |

### AI tests (42, require `--with-ai`)

| Area | Tests |
|------|-------|
| Basic AI | Simple question |
| Read-only tools | list_workflows, get_system_status, search_files, read_file, list_files, get_log_tail, get_dashboard_status |
| Run tools | list_recent_runs, get_run_status, get_task_output |
| Memory tools | save/recall, update existing key, list_memories, delete non-existent key |
| File tools | get_file_summary, get_folder_summary |
| JCWF read tools | jcwf_read, jcwf_explain, jcwf_validate, jcwf_read_plan |
| Access control | read_file denied (config.json), read_file nonexistent, read_file path traversal |
| Multi-step | get_system_status + list_workflows chain |
| Approval flow | run_shell (auto-approve), deny path, request message structure |
| Mutating tools (auto-approve) | write_file, write_file path traversal, write_file denied by user, edit_file, jcwf_write_plan, jcwf_generate, jcwf_fix_task, jcwf_write_script |
| Workflow runtime control (auto-approve) | run_workflow, workflow_pause + workflow_resume, workflow_stop |
| Loop behaviors | loop detection (same tool 4+ times), max iterations (10 distinct calls) |
| Response validation | hallucinated file path warning |

---

## Manually verified (no automated test needed)

| Scenario | Result |
|----------|--------|
| Memory persistence across restart | **PASS** — verified 2026-03-28 |
| Index persistence across restart | **PASS** — verified 2026-03-28 |
| Approval timeout auto-deny (60 s) | **PASS** — verified 2026-03-28 |

---

## Notes

- Tests for mutating tools clean up after themselves (delete written files, scripts, etc.)
- Workflow runtime control tests use `jarvisCppDocu` as fixture (many AI tasks = long enough to pause)
- Approval timeout automate only if a configurable timeout is added to j9t
- There is no server-side blocklist for `run_shell`. Any shell command (including destructive ones like `rm -rf`) goes through the normal approval flow. The only server-side access controls are path traversal rejection and the sensitive-file deny-list for `read_file`/`write_file`/`edit_file`.
