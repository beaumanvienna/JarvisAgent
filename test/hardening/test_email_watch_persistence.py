#!/usr/bin/env python3
"""
email_watch persistence round-trip test (greenmail-based, fully automated).

Verifies the trigger engine's `.email_watermarks.json` save+load path,
the four integrity guards, and the restore happy-path:

  S1  save with UIDVALIDITY   — first poll seeds v2 entry on disk
  S2  v1 file rejected        — format_version=1 → WARN, in-memory map empty
  S3  (conn,folder) guard     — trigger repointed to another connection → WARN
  S4  UIDVALIDITY-change      — greenmail restart → V mismatch → WARN, no fire
  S5  restore happy-path      — load applies watermark; only newer UIDs fire
  S6  prune on removal        — uninstall a workflow → its orphaned watermark is
                                pruned on a surviving trigger's next poll

S1 + S6 run against the live instance without a restart.
S2-S5 require j9t lifecycle ownership — the test REST-shuts-down the live
instance, relaunches via `jarvisagent.sh`, and re-unlocks the keystore.
Requires `JARVIS_MASTER_PASSWORD` in env (per feedback_jc_dev_env_vars).
Without it, S2-S5 are skipped with a warning.

Setup:
  - greenmail container on localhost (SMTP 3025, IMAP 3143, user test:test).
  - j9t already running on https://localhost:8443 with `my-greenmail` and
    `my-email` connections configured + keystore unlocked.
  - $JARVIS_MASTER_PASSWORD exported for the relaunch cycles.

Usage:
  python3 test/hardening/test_email_watch_persistence.py --admin-key "$J9T_TOKEN"
"""

import argparse
import json
import os
import shutil
import smtplib
import socket
import ssl
import subprocess
import sys
import time
import urllib.request
import urllib.error
import uuid
import zipfile
from email.mime.text import MIMEText
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BASE = "https://localhost:8443"
GREENMAIL_HOST = "localhost"
GREENMAIL_SMTP_PORT = 3025
GREENMAIL_IMAP_PORT = 3143
GREENMAIL_USER = "test"
GREENMAIL_PASS = "test"
GREENMAIL_CONTAINER = "greenmail"
CONNECTION_NAME = "my-greenmail"
ALT_CONNECTION = "my-email"  # second email connection for scenario 3 swap
WATERMARK_RELPATH = "queue/.email_watermarks.json"
LOG_FILE = PROJECT_ROOT / "log" / "log.txt"
RELAUNCH_LOG = PROJECT_ROOT / "log" / "test-email-watch-relaunch.log"


class C:
    RESET = "\033[0m"; BOLD = "\033[1m"
    RED = "\033[91m"; GREEN = "\033[92m"; CYAN = "\033[96m"; YELLOW = "\033[93m"


def ok(m): print(f"  {C.GREEN}✓{C.RESET} {m}")
def fail(m): print(f"  {C.RED}✗{C.RESET} {m}")
def info(m): print(f"  {C.CYAN}ℹ{C.RESET} {m}")
def warn(m): print(f"  {C.YELLOW}⚠{C.RESET} {m}")
def header(m): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {m}\n{'─'*70}{C.RESET}")


# ─── HTTP / readiness ──────────────────────────────────────────────────────

def http(method, path, admin_key, body=None, timeout=15):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    headers = {"Content-Type": "application/json"}
    if admin_key:
        headers["Authorization"] = f"Bearer {admin_key}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            return resp.status, text
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except urllib.error.URLError:
        return 0, ""


def is_port_listening(host, port, timeout=0.5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def greenmail_reachable():
    return (is_port_listening(GREENMAIL_HOST, GREENMAIL_IMAP_PORT, 2)
            and is_port_listening(GREENMAIL_HOST, GREENMAIL_SMTP_PORT, 2))


# ─── j9t lifecycle ─────────────────────────────────────────────────────────

def shutdown_j9t(admin_key, exit_timeout=15):
    """REST-shutdown j9t and wait for HTTPS port to stop listening."""
    if not is_port_listening("localhost", 8443):
        return True
    sc, _ = http("POST", "/api/shutdown", admin_key, timeout=5)
    if sc not in (200, 202, 0):
        warn(f"POST /api/shutdown → {sc} (proceeding to wait anyway)")
    deadline = time.time() + exit_timeout
    while time.time() < deadline:
        if not is_port_listening("localhost", 8443, timeout=0.3):
            return True
        time.sleep(0.3)
    return False


def launch_j9t():
    """Launch j9t via the launcher script in a detached process group."""
    RELAUNCH_LOG.parent.mkdir(parents=True, exist_ok=True)
    fh = open(RELAUNCH_LOG, "a")
    edition_file = PROJECT_ROOT / ".build-edition"
    edition = edition_file.read_text().strip() if edition_file.exists() else "studio"
    args = [str(PROJECT_ROOT / "jarvisagent.sh")]
    if edition == "engine":
        args.append("--engine")
    proc = subprocess.Popen(
        args, cwd=str(PROJECT_ROOT),
        stdout=fh, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    return proc


def wait_for_j9t_listening(timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if is_port_listening("localhost", 8443, timeout=0.3):
            # TLS handshake may not be ready for a beat after socket accept;
            # confirm with an actual HTTPS request that doesn't need auth.
            sc, _ = http("POST", "/api/settings/keys/unlock", "",
                         body={"master_password": "__probe__"}, timeout=2)
            if sc in (200, 400, 401, 403, 423, 429):
                return True
        time.sleep(0.3)
    return False


def unlock_keystore(master_password):
    sc, body = http("POST", "/api/settings/keys/unlock", "",
                    body={"master_password": master_password}, timeout=10)
    return sc == 200, sc, body


def test_connection_health(admin_key, connection):
    """Smoke-check that j9t can resolve credentials post-unlock."""
    sc, body = http("POST", f"/api/connections/{connection}/test", admin_key, timeout=10)
    return sc == 200, body


def restart_j9t(admin_key, master_password, pause_while_down=None):
    """Full restart cycle: shutdown → optional offline action → launch → unlock.

    `pause_while_down` runs while j9t is down (after shutdown, before launch)
    — use for planting files, swapping JCWFs, restarting greenmail, etc.
    Returns True on full success (port listening + keystore unlocked).
    """
    if not shutdown_j9t(admin_key):
        fail("shutdown_j9t timed out")
        return False
    if pause_while_down is not None:
        pause_while_down()
    launch_j9t()
    if not wait_for_j9t_listening(timeout=30):
        fail("j9t did not start listening within 30 s")
        return False
    unlocked, sc, body = unlock_keystore(master_password)
    if not unlocked:
        fail(f"keystore unlock failed: {sc} {body[:200]}")
        return False
    return True


# ─── log / file helpers ────────────────────────────────────────────────────

def current_log_offset():
    if not LOG_FILE.exists():
        return 0
    return LOG_FILE.stat().st_size


def wait_for_log_substring(needle, since_offset, timeout=30):
    """Tail log.txt from `since_offset` and return True if `needle` appears.

    j9t recreates log.txt on each restart (spdlog opens fresh), so when a
    restart cycle reduces the file size below `since_offset`, we seek to 0
    instead — the whole post-restart log is fair game.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if LOG_FILE.exists():
            size = LOG_FILE.stat().st_size
            effective_offset = since_offset if size >= since_offset else 0
            if size > effective_offset:
                with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(effective_offset)
                    tail = f.read()
                if needle in tail:
                    return True
        time.sleep(0.3)
    return False


def read_log_tail(since_offset, max_bytes=4000):
    if not LOG_FILE.exists():
        return ""
    size = LOG_FILE.stat().st_size
    effective_offset = since_offset if size >= since_offset else 0
    with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
        f.seek(effective_offset)
        text = f.read()
    if len(text) > max_bytes:
        return "..." + text[-max_bytes:]
    return text


def read_watermarks_file():
    path = PROJECT_ROOT / WATERMARK_RELPATH
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as e:
        return {"_parse_error": str(e), "_raw": path.read_text()[:500]}


def write_watermarks_file(content_dict):
    path = PROJECT_ROOT / WATERMARK_RELPATH
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(content_dict, indent=2))


def clear_watermarks_file():
    path = PROJECT_ROOT / WATERMARK_RELPATH
    if path.exists():
        path.unlink()


def find_entry(file_json, workflow_id, trigger_id):
    if not file_json:
        return None
    for e in file_json.get("watermarks", []):
        if e.get("workflow_id") == workflow_id and e.get("trigger_id") == trigger_id:
            return e
    return None


# ─── workflow install ──────────────────────────────────────────────────────

def send_test_email(subject, body_extra=""):
    # Recipient is the bare username 'test' — greenmail's `users=test:test`
    # config creates one user whose email IS `test` (no @-domain). Mail to
    # `test@<anything>` would auto-create a different user; only mail to
    # bare `test` lands in the inbox accessible via `LOGIN test test`.
    msg = MIMEText(f"j9t email_watch test message ({subject}) {body_extra}")
    msg["From"] = "sender@example.com"
    msg["To"] = "test"
    msg["Subject"] = subject
    with smtplib.SMTP(GREENMAIL_HOST, GREENMAIL_SMTP_PORT, timeout=5) as s:
        s.sendmail("sender@example.com", ["test"], msg.as_string())


def build_global_json(wfid, connection_name, folder="INBOX"):
    # `triggers` MUST live in global.json (not the canvas JSON) — the binder
    # iterates `WorkflowDefinition::m_Triggers` from the global parse path.
    # POST /api/workflows writes a minimal global.json missing the triggers
    # field, so the test bypasses the REST create path and writes the JCWF
    # zip directly.
    return {
        "version": "1.0",
        "id": wfid,
        "label": f"email_watch persistence canary {wfid}",
        "manual_start": False,
        "triggers": [{
            "type": "email_watch",
            "id": "ew",
            "enabled": True,
            "params": {
                "connection": connection_name,
                "folder": folder,
                "poll_interval_seconds": 2,
            }
        }],
    }


def build_canvas_json(wfid):
    # `scripts/echo.sh` is the no-op canvas task — it succeeds and emits a
    # workflow log line we can search for. Shell tasks enforce the `scripts/`
    # prefix (allowlist-over-blocklist per feedback_allowlist_not_blocklist),
    # so an absolute command like `touch /tmp/...` is rejected at executor
    # entry — using a sanctioned script keeps fires from logging as failures.
    return {
        "version": "1.0",
        "id": wfid,
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "shell",
                "label": "echo",
                "working_directory": "",
                "params": {
                    "command": "scripts/echo.sh",
                    "args": [f"email_watch test fire for {wfid}"],
                },
            }
        },
    }


def install_workflow(wfid, connection_name, admin_key, folder="INBOX"):
    """Write the JCWF zip + extracted folder directly on disk + trigger reload."""
    wf_dir = PROJECT_ROOT / "workflows" / wfid
    jcwf_path = PROJECT_ROOT / "workflows" / f"{wfid}.jcwf"
    wf_dir.mkdir(parents=True, exist_ok=True)
    (wf_dir / "global.json").write_text(
        json.dumps(build_global_json(wfid, connection_name, folder), indent=2))
    canvas = build_canvas_json(wfid)
    (wf_dir / f"{wfid}.json").write_text(json.dumps(canvas, indent=2))
    with zipfile.ZipFile(jcwf_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(wf_dir / "global.json", "global.json")
        zf.write(wf_dir / f"{wfid}.json", f"{wfid}.json")
    sc, body = http("POST", "/api/workflows/reload", admin_key)
    if sc != 200:
        warn(f"reload → {sc} body={body[:200]}")
        return False
    return True


def repoint_jcwf_connection_offline(wfid, new_connection, new_folder="INBOX"):
    """Edit the JCWF on disk while j9t is shut down — triggers re-bind on relaunch."""
    wf_dir = PROJECT_ROOT / "workflows" / wfid
    jcwf_path = PROJECT_ROOT / "workflows" / f"{wfid}.jcwf"
    (wf_dir / "global.json").write_text(
        json.dumps(build_global_json(wfid, new_connection, new_folder), indent=2))
    with zipfile.ZipFile(jcwf_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(wf_dir / "global.json", "global.json")
        zf.write(wf_dir / f"{wfid}.json", f"{wfid}.json")


def uninstall_workflow(wfid, admin_key):
    wf_dir = PROJECT_ROOT / "workflows" / wfid
    jcwf_path = PROJECT_ROOT / "workflows" / f"{wfid}.jcwf"
    queue_dir = PROJECT_ROOT / "queue" / wfid
    for p in (jcwf_path,):
        if p.exists():
            p.unlink()
    for d in (wf_dir, queue_dir):
        if d.exists():
            shutil.rmtree(d, ignore_errors=True)
    # Reload so the trigger is dropped on the running instance (best-effort).
    http("POST", "/api/workflows/reload", admin_key)


def wait_for_entry(workflow_id, trigger_id, deadline_s=90):
    deadline = time.time() + deadline_s
    last_file = None
    while time.time() < deadline:
        f = read_watermarks_file()
        if f is not None:
            entry = find_entry(f, workflow_id, trigger_id)
            if entry and entry.get("last_seen_uid"):
                return entry, f
            last_file = f
        time.sleep(0.5)
    return None, last_file


def wait_for_entry_absent(workflow_id, trigger_id, deadline_s=140):
    """Wait until no watermark entry for (workflow_id, trigger_id) remains in the
    file.  A surviving email_watch trigger re-saves on every poll (~60 s tick),
    and the save-time prune drops keys with no live trigger — so an orphaned
    entry disappears within a poll or two of its workflow being uninstalled."""
    deadline = time.time() + deadline_s
    last_file = None
    while time.time() < deadline:
        f = read_watermarks_file()
        if f is not None:
            last_file = f
            if find_entry(f, workflow_id, trigger_id) is None:
                return True, f
        time.sleep(1.0)
    return False, last_file


def expect(cond, label, results):
    if cond:
        ok(label); results[0] += 1
    else:
        fail(label); results[1] += 1


# ─── greenmail ─────────────────────────────────────────────────────────────

def docker_restart_greenmail():
    """Force a UIDVALIDITY change by restarting the greenmail container."""
    try:
        subprocess.run(["docker", "restart", GREENMAIL_CONTAINER],
                       check=True, capture_output=True, timeout=30)
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    # Wait for IMAP+SMTP to be back.
    deadline = time.time() + 30
    while time.time() < deadline:
        if greenmail_reachable():
            time.sleep(1.0)  # extra beat for greenmail's listener handshake
            return True, ""
        time.sleep(0.5)
    return False, "greenmail did not come back within 30 s"


# ─── scenarios ─────────────────────────────────────────────────────────────

def scenario_save_with_uidvalidity(admin_key, results):
    header("S1. send email → email_watch poll → .email_watermarks.json v2 entry with uid_validity")
    wfid = f"s1_emailwatch_{uuid.uuid4().hex[:8]}"
    try:
        send_test_email(f"j9t-{wfid}")
        if not install_workflow(wfid, CONNECTION_NAME, admin_key):
            warn("install failed — skipping the rest of this scenario")
            results[1] += 1
            return

        entry, file_json = wait_for_entry(wfid, "ew", deadline_s=90)
        if entry is None:
            fail(f"no watermark entry appeared within 90 s "
                 f"(trigger-engine tick is ~60 s)")
            if file_json:
                info(f"file contents: {json.dumps(file_json, indent=2)[:400]}")
            results[1] += 1
            return

        info(f"entry: {json.dumps(entry, indent=2)}")
        expect(file_json.get("format_version") == 2,
               f"format_version is 2 (got {file_json.get('format_version')!r})", results)
        expect(entry.get("connection_name") == CONNECTION_NAME,
               f"connection_name is '{CONNECTION_NAME}'", results)
        expect(entry.get("folder") == "INBOX", "folder is 'INBOX'", results)
        uv = entry.get("uid_validity")
        expect(isinstance(uv, int) and uv > 0,
               f"uid_validity is positive integer (got {uv!r})", results)
        last_uid = entry.get("last_seen_uid")
        expect(isinstance(last_uid, str) and last_uid.isdigit() and int(last_uid) > 0,
               f"last_seen_uid is positive digit string (got {last_uid!r})", results)
        expect(isinstance(entry.get("updated_at"), str) and entry["updated_at"].endswith("Z"),
               f"updated_at is ISO-8601 Z", results)
    finally:
        uninstall_workflow(wfid, admin_key)


def scenario_v1_rejection(admin_key, master_password, results):
    header("S2. plant format_version=1 file → restart j9t → load WARN, in-memory map empty")
    # Use a uniquely-tagged fake workflow_id so we can assert the file is
    # cleared on the first save after restart (no other test installs use it).
    fake_wfid = f"s2_fake_{uuid.uuid4().hex[:8]}"
    v1_content = {
        "format_version": 1,
        "watermarks": [{
            "workflow_id": fake_wfid,
            "trigger_id": "ew",
            "last_seen_uid": "999",
            "updated_at": "2025-01-01T00:00:00Z",
        }],
    }
    def offline_setup():
        clear_watermarks_file()
        write_watermarks_file(v1_content)
        info(f"planted v1 file at {WATERMARK_RELPATH} with fake workflow '{fake_wfid}'")

    # We need to capture the log offset AFTER restart launches but BEFORE the
    # load fires. The load happens early in TriggerEngine construction, before
    # the listener accepts connections — so by the time wait_for_j9t_listening
    # returns, the load WARN should already be in the log. Capture offset
    # before shutdown, then scan the whole post-shutdown tail.
    offset_pre = current_log_offset()

    if not restart_j9t(admin_key, master_password, pause_while_down=offline_setup):
        results[1] += 1
        return

    needle = f"format_version=1 (expected 2)"
    if wait_for_log_substring(needle, offset_pre, timeout=15):
        ok(f"log contains WARN: '...{needle}...'")
        results[0] += 1
    else:
        fail(f"log MISSING WARN substring: {needle!r}")
        info("recent log tail:")
        for line in read_log_tail(offset_pre).splitlines()[-10:]:
            info(f"  {line}")
        results[1] += 1

    # After load, the in-memory map should be empty. Inspect via the file: the
    # file is not rewritten until the next save (which only fires after a
    # successful poll). So we verify the file is STILL v1 (untouched) and the
    # planted fake entry is still there — proving the in-memory map didn't
    # restore the fake entry.
    f = read_watermarks_file()
    if f is None:
        fail("watermarks file disappeared (load shouldn't touch it)")
        results[1] += 1
    elif f.get("format_version") == 1 and find_entry(f, fake_wfid, "ew"):
        ok("watermarks file still v1 on disk (load is read-only, save only fires on poll)")
        results[0] += 1
    else:
        # Some real trigger may have polled and overwritten — if so, the new
        # v2 file should NOT contain the fake entry (in-memory map was empty
        # at load time).
        if f.get("format_version") == 2 and not find_entry(f, fake_wfid, "ew"):
            ok("watermarks file rewritten v2, fake v1 entry was not restored to in-memory map")
            results[0] += 1
        else:
            fail(f"unexpected watermarks file state: {json.dumps(f, indent=2)[:300]}")
            results[1] += 1


def scenario_connection_swap_guard(admin_key, master_password, results):
    header("S3. install on conn A → save → repoint to conn B offline → restart → guard WARN")
    wfid = f"s3_swap_{uuid.uuid4().hex[:8]}"
    try:
        send_test_email(f"j9t-{wfid}")
        if not install_workflow(wfid, CONNECTION_NAME, admin_key):
            warn("install failed")
            results[1] += 1
            return
        entry, _ = wait_for_entry(wfid, "ew", deadline_s=90)
        if entry is None:
            fail("no watermark appeared on connection A; aborting scenario")
            results[1] += 1
            return
        info(f"saved on connection={entry.get('connection_name')!r} "
             f"uid={entry.get('last_seen_uid')!r}")

        def offline_swap():
            repoint_jcwf_connection_offline(wfid, ALT_CONNECTION)
            info(f"swapped JCWF connection: {CONNECTION_NAME} → {ALT_CONNECTION}")

        offset_pre = current_log_offset()
        if not restart_j9t(admin_key, master_password, pause_while_down=offline_swap):
            results[1] += 1
            return

        needle = f"discarding stale watermark for trigger 'ew' workflow '{wfid}'"
        if wait_for_log_substring(needle, offset_pre, timeout=30):
            ok(f"log contains registration-time guard WARN for trigger '{wfid}'")
            results[0] += 1
        else:
            fail(f"log MISSING WARN: {needle!r}")
            info("recent log tail:")
            for line in read_log_tail(offset_pre).splitlines()[-15:]:
                info(f"  {line}")
            results[1] += 1
    finally:
        uninstall_workflow(wfid, admin_key)


def scenario_uidvalidity_change_guard(admin_key, master_password, results):
    header("S4. greenmail restart → UIDVALIDITY changes → poll-time guard WARN, no fire")
    wfid = f"s4_uidv_{uuid.uuid4().hex[:8]}"
    try:
        # Seed with greenmail's current UIDVALIDITY.
        send_test_email(f"j9t-{wfid}-seed")
        if not install_workflow(wfid, CONNECTION_NAME, admin_key):
            warn("install failed")
            results[1] += 1
            return
        entry, _ = wait_for_entry(wfid, "ew", deadline_s=90)
        if entry is None:
            fail("no watermark appeared on initial poll; aborting")
            results[1] += 1
            return
        uv1 = entry.get("uid_validity")
        info(f"baseline uid_validity={uv1}, last_seen_uid={entry.get('last_seen_uid')}")

        # Shutdown, restart greenmail (new UIDVALIDITY + empty INBOX), send a
        # fresh canary, relaunch j9t. Poll-time guard should fire.
        def offline_action():
            info("docker restart greenmail (forces UIDVALIDITY change)")
            ok_restart, err = docker_restart_greenmail()
            if not ok_restart:
                fail(f"greenmail restart failed: {err}")
                raise RuntimeError("greenmail restart failed")
            send_test_email(f"j9t-{wfid}-post-restart")
            info("sent fresh canary into post-restart INBOX")

        offset_pre = current_log_offset()
        if not restart_j9t(admin_key, master_password, pause_while_down=offline_action):
            results[1] += 1
            return

        # Match on the prefix only — the new V isn't known in advance.
        needle = f"UIDVALIDITY changed ({uv1} -> "
        if wait_for_log_substring(needle, offset_pre, timeout=90):
            ok(f"log contains poll-time UIDVALIDITY-change WARN ('{needle}...')")
            results[0] += 1
        else:
            fail(f"log MISSING UIDVALIDITY-change WARN: {needle!r}")
            info("recent log tail:")
            for line in read_log_tail(offset_pre).splitlines()[-20:]:
                info(f"  {line}")
            results[1] += 1

        # The guard zeroes hasNewMail before the fire branch — no "firing
        # trigger" line for this wfid should appear in the post-restart log.
        # Give the poll cycle a small grace window for log ordering races.
        time.sleep(2.0)
        fire_needle = f"firing trigger workflow='{wfid}' trigger='ew'"
        tail = read_log_tail(offset_pre, max_bytes=20000)
        expect(fire_needle not in tail,
               f"NO fire-trigger line for {wfid} after UIDVALIDITY change", results)

        # New watermark should record the new UIDVALIDITY.
        new_entry, _ = wait_for_entry(wfid, "ew", deadline_s=30)
        if new_entry:
            new_uv = new_entry.get("uid_validity")
            expect(isinstance(new_uv, int) and new_uv != uv1,
                   f"watermark uid_validity updated ({uv1} → {new_uv})", results)
        else:
            fail("watermark didn't reappear after restart")
            results[1] += 1
    finally:
        uninstall_workflow(wfid, admin_key)


def scenario_restore_happy_path(admin_key, master_password, results):
    header("S5. save watermark U1 → shutdown → send mail U2 → relaunch → fire for U2 only")
    wfid = f"s5_restore_{uuid.uuid4().hex[:8]}"
    try:
        send_test_email(f"j9t-{wfid}-seed")
        if not install_workflow(wfid, CONNECTION_NAME, admin_key):
            warn("install failed")
            results[1] += 1
            return
        entry, _ = wait_for_entry(wfid, "ew", deadline_s=90)
        if entry is None:
            fail("no watermark appeared on seed poll; aborting")
            results[1] += 1
            return
        u1 = entry.get("last_seen_uid")
        uv = entry.get("uid_validity")
        info(f"baseline last_seen_uid={u1}, uid_validity={uv}")

        def offline_action():
            send_test_email(f"j9t-{wfid}-fresh")
            info("sent fresh canary while j9t is down (UID > U1 on next poll)")

        offset_pre = current_log_offset()
        if not restart_j9t(admin_key, master_password, pause_while_down=offline_action):
            results[1] += 1
            return

        # On relaunch: load restores U1, trigger registers with U1, next poll
        # sees U2 > U1, fires.
        restore_log = f"restored persisted UID watermark '{u1}'"
        if wait_for_log_substring(restore_log, offset_pre, timeout=30):
            ok(f"log contains restore-INFO line ('{restore_log}')")
            results[0] += 1
        else:
            warn(f"log missing restore-INFO ('{restore_log}') — may have re-seeded")

        # Wait for the fire-trigger log line (one trigger-engine tick).
        fire_needle = f"firing trigger workflow='{wfid}' trigger='ew'"
        if wait_for_log_substring(fire_needle, offset_pre, timeout=90):
            ok(f"log contains fire-trigger line for {wfid} (fired for fresh UID)")
            results[0] += 1
        else:
            fail(f"log MISSING fire-trigger line: {fire_needle!r}")
            info("recent log tail:")
            for line in read_log_tail(offset_pre).splitlines()[-20:]:
                info(f"  {line}")
            results[1] += 1

        # Verify the new UID watermark recorded by the post-fire save.
        deadline = time.time() + 30
        u2 = None
        while time.time() < deadline:
            new_entry, _ = wait_for_entry(wfid, "ew", deadline_s=5)
            if new_entry:
                cand = new_entry.get("last_seen_uid")
                if cand and cand != u1:
                    u2 = cand
                    break
            time.sleep(1.0)
        if u2 is not None:
            expect(isinstance(u2, str) and u2.isdigit() and int(u2) > int(u1),
                   f"watermark advanced to last_seen_uid={u2} (was {u1})", results)
            new_entry, _ = wait_for_entry(wfid, "ew", deadline_s=5)
            expect(new_entry and new_entry.get("uid_validity") == uv,
                   f"uid_validity unchanged ({uv}) — no UIDVALIDITY drift", results)
        else:
            fail(f"watermark didn't advance past {u1} within 30 s after fire")
            results[1] += 1
    finally:
        uninstall_workflow(wfid, admin_key)


def scenario_prune_orphaned_watermark(admin_key, results):
    header("S6. uninstall an email_watch workflow → its watermark is pruned on the "
           "next surviving-trigger poll (no unbounded growth)")
    # Two email_watch workflows on the same INBOX.  After both have a persisted
    # entry, uninstall A: the reload path (ClearAll + RegisterAll) drops A's live
    # trigger but leaves its persisted entry orphaned.  B survives and re-saves on
    # its next poll; the save-time prune (live-key set = {B}) drops A's orphan.
    wfid_a = f"s6a_emailwatch_{uuid.uuid4().hex[:8]}"
    wfid_b = f"s6b_emailwatch_{uuid.uuid4().hex[:8]}"
    try:
        send_test_email(f"j9t-{wfid_a}-seed")
        if not install_workflow(wfid_a, CONNECTION_NAME, admin_key) or \
           not install_workflow(wfid_b, CONNECTION_NAME, admin_key):
            warn("install failed — skipping the rest of this scenario")
            results[1] += 1
            return

        entry_a, _ = wait_for_entry(wfid_a, "ew", deadline_s=90)
        entry_b, _ = wait_for_entry(wfid_b, "ew", deadline_s=90)
        if entry_a is None or entry_b is None:
            fail(f"both watermark entries didn't appear within 90 s "
                 f"(A={'ok' if entry_a else 'missing'}, B={'ok' if entry_b else 'missing'})")
            results[1] += 1
            return
        ok(f"both A ({wfid_a}) and B ({wfid_b}) have persisted watermark entries")
        results[0] += 1

        # Uninstall A only; B stays live and keeps polling.
        uninstall_workflow(wfid_a, admin_key)
        info(f"uninstalled {wfid_a}; waiting for its orphaned watermark to be pruned "
             f"on B's next poll (~60 s tick, deadline 140 s)")

        pruned, file_json = wait_for_entry_absent(wfid_a, "ew", deadline_s=140)
        expect(pruned, f"A's watermark entry pruned after uninstall ({wfid_a} absent)", results)
        # B must NOT have been collateral-pruned — it's still a live trigger.
        b_still_present = file_json is not None and find_entry(file_json, wfid_b, "ew") is not None
        expect(b_still_present,
               f"B's watermark entry survived the prune ({wfid_b} still present)", results)
        if file_json is not None:
            info(f"final watermark count: {len(file_json.get('watermarks', []))}")
    finally:
        uninstall_workflow(wfid_a, admin_key)
        uninstall_workflow(wfid_b, admin_key)


# ─── entry point ───────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--admin-key", required=True, help="MCP admin key (Bearer)")
    args = parser.parse_args()

    if not greenmail_reachable():
        print(f"{C.RED}ERROR:{C.RESET} greenmail not reachable on {GREENMAIL_HOST}:{GREENMAIL_SMTP_PORT}/"
              f"{GREENMAIL_IMAP_PORT}")
        return 2
    sc, _ = http("POST", f"/api/connections/{CONNECTION_NAME}/test", args.admin_key)
    if sc != 200:
        print(f"{C.RED}ERROR:{C.RESET} POST /api/connections/{CONNECTION_NAME}/test → {sc}; "
              f"is j9t running + keystore unlocked + connection configured?")
        return 2

    master_password = os.environ.get("JARVIS_MASTER_PASSWORD")
    can_restart = bool(master_password)
    if not can_restart:
        warn("JARVIS_MASTER_PASSWORD not set — S2/S3/S4/S5 will be skipped")

    results = [0, 0]
    scenario_save_with_uidvalidity(args.admin_key, results)
    scenario_prune_orphaned_watermark(args.admin_key, results)
    if can_restart:
        scenario_v1_rejection(args.admin_key, master_password, results)
        scenario_connection_swap_guard(args.admin_key, master_password, results)
        scenario_restore_happy_path(args.admin_key, master_password, results)
        scenario_uidvalidity_change_guard(args.admin_key, master_password, results)
    else:
        warn("skipped: S2 (v1 rejection), S3 ((conn,folder) guard), "
             "S4 (UIDVALIDITY change), S5 (restore happy-path)")

    passed, failed = results
    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
