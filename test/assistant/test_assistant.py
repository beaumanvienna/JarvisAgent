#!/usr/bin/env python3
"""
AI Assistant E2E Test Suite

Connects to the /ws/assistant WebSocket and exercises every major feature:
  - Session management (new, list, resume, history replay, turn counts)
  - Slash commands (/help, /status, /runs, /log, /memory, /index, /sessions, /new, /clear)
  - Auto-completion requests
  - Ctrl+R history retrieval
  - AI queries (requires a configured AI provider)
  - Tool calls triggered by AI (read-only and mutating)
  - Approval flow for mutating tools (approve and deny paths)
  - Protocol edge cases (unknown message type, malformed JSON, etc.)

Usage:
    # JarvisAgent must be running first.
    python test/assistant/test_assistant.py                # run all non-AI tests
    python test/assistant/test_assistant.py --with-ai      # include AI provider tests
    python test/assistant/test_assistant.py --list          # list available tests
    python test/assistant/test_assistant.py -k slash        # run tests matching "slash"
    python test/assistant/test_assistant.py --auto-approve  # auto-approve mutating tools

Requires: sudo apt install python3-websocket   (Debian/Ubuntu)
     or: pip install websocket-client            (venv / non-managed Python)
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
import ssl
import traceback
from typing import Any, Callable, Dict, List, Optional

try:
    import websocket  # from python3-websocket / websocket-client
except ImportError:
    print("ERROR: 'websocket' module not found. Install with:")
    print("  sudo apt install python3-websocket   (Debian/Ubuntu)")
    print("  pip install websocket-client          (venv)")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Default to TLS — j9t serves WSS-only on port 8443.  Override via --url for
# legacy ws:// configs.  Self-signed cert acceptance is automatic when wss://
# is used (this is a localhost dev harness, not a public client).
WS_URL = "wss://localhost:8443/ws/assistant"
DEFAULT_TIMEOUT = 15.0   # seconds to wait for a response
AI_TIMEOUT = 60.0        # longer timeout for AI provider calls
PING_INTERVAL = 0.5      # match frontend ping cadence

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------

class C:
    GREEN  = "\033[92m"
    RED    = "\033[91m"
    YELLOW = "\033[93m"
    CYAN   = "\033[96m"
    DIM    = "\033[2m"
    BOLD   = "\033[1m"
    RESET  = "\033[0m"

# ---------------------------------------------------------------------------
# Assistant client (uses websocket-client library)
# ---------------------------------------------------------------------------

class AssistantClient:
    def __init__(self, url: str = WS_URL, auto_approve: bool = False, verbose: bool = False,
                 token: Optional[str] = None):
        self.url = url
        self.auto_approve = auto_approve
        self.verbose = verbose
        self.token = token
        self.ws: Optional[websocket.WebSocket] = None
        self.session_id: str = ""
        self._queued: List[dict] = []
        self._lock = threading.Lock()
        # _ws_io_lock serializes ws.send() and ws.recv() across the ping thread + main
        # thread.  websocket-client 1.7.0's WebSocket isn't thread-safe under TLS +
        # concurrent send/recv — concurrent access drops the connection within ~40 ms.
        # Plain ws:// previously masked this; wss:// (now the default) exposes it.
        self._ws_io_lock = threading.Lock()
        self._ping_thread: Optional[threading.Thread] = None
        self._ping_stop = threading.Event()

    def connect(self):
        """Connect to the assistant WebSocket.

        For wss:// against the localhost dev server (self-signed cert), disable
        cert verification — this is a localhost dev harness, not a public client.
        For ws:// (plain), no SSL options applied.

        Auth: /ws and /ws/assistant gate at the upgrade handshake via Authorization:
        Bearer; pass --token or set J9T_TOKEN to authenticate.
        """
        kwargs: Dict[str, Any] = {"timeout": 5}
        if self.url.startswith("wss://"):
            kwargs["sslopt"] = {"cert_reqs": ssl.CERT_NONE, "check_hostname": False}
        if self.token:
            kwargs["header"] = [f"Authorization: Bearer {self.token}"]
        self.ws = websocket.create_connection(self.url, **kwargs)
        self._ping_stop.clear()
        self._ping_thread = threading.Thread(target=self._ping_loop, daemon=True)
        self._ping_thread.start()

    def close(self):
        """Close the connection."""
        self._ping_stop.set()
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
            self.ws = None

    def _ping_loop(self):
        """Send pings to trigger server-side message draining."""
        while not self._ping_stop.is_set():
            try:
                if self.ws and self.ws.connected:
                    with self._ws_io_lock:
                        self.ws.send(json.dumps({"type": "ping"}))
            except Exception:
                break
            self._ping_stop.wait(PING_INTERVAL)

    def send(self, msg: dict):
        """Send a JSON message."""
        raw = json.dumps(msg)
        if self.verbose:
            print(f"  {C.DIM}>>> {raw[:200]}{C.RESET}")
        with self._ws_io_lock:
            self.ws.send(raw)

    def recv(self, timeout: float = DEFAULT_TIMEOUT) -> Optional[dict]:
        """Receive a single non-ping message, with timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            # Check queued messages first.
            with self._lock:
                if self._queued:
                    return self._queued.pop(0)
            try:
                remaining = max(0.1, deadline - time.time())
                with self._ws_io_lock:
                    self.ws.settimeout(remaining)
                    raw = self.ws.recv()
                if raw is None:
                    continue
                msg = json.loads(raw)
                # Unwrap batch messages.
                if msg.get("type") == "batch":
                    for sub in msg.get("messages", []):
                        sub_msg = json.loads(sub) if isinstance(sub, str) else sub
                        if sub_msg.get("type") not in ("pong",):
                            with self._lock:
                                self._queued.append(sub_msg)
                    with self._lock:
                        if self._queued:
                            return self._queued.pop(0)
                    continue
                if msg.get("type") in ("pong",):
                    continue
                if self.verbose:
                    text = json.dumps(msg)
                    print(f"  {C.DIM}<<< {text[:300]}{C.RESET}")
                return msg
            except (socket.timeout, websocket.WebSocketTimeoutException):
                with self._lock:
                    if self._queued:
                        return self._queued.pop(0)
                return None
            except Exception as e:
                if self.verbose:
                    print(f"  {C.DIM}recv error: {e}{C.RESET}")
                return None
        return None

    def drain(self, timeout: float = 0.5) -> List[dict]:
        """Drain all pending messages."""
        msgs = []
        while True:
            m = self.recv(timeout=timeout)
            if m is None:
                break
            msgs.append(m)
        return msgs

    def wait_for(self, msg_type: str, timeout: float = DEFAULT_TIMEOUT) -> Optional[dict]:
        """Wait for a specific message type, collecting others."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.1, deadline - time.time())
            msg = self.recv(timeout=remaining)
            if msg is None:
                return None
            if msg.get("type") == msg_type:
                return msg
            # Handle auto-approval while waiting.
            if msg.get("type") == "approval_request" and self.auto_approve:
                self.send({
                    "type": "approval_response",
                    "requestId": msg["requestId"],
                    "approved": True,
                })
        return None

    def new_session(self) -> str:
        """Request a new session and return the session ID."""
        self.send({"type": "new_session"})
        msg = self.wait_for("session_active")
        if msg:
            self.session_id = msg["sessionId"]
        return self.session_id

    def send_command(self, command: str, args: str = "") -> Optional[dict]:
        """Send a slash command and wait for the response."""
        self.send({
            "type": "command",
            "sessionId": self.session_id,
            "command": command,
            "args": args,
        })
        return self.wait_for("assistant_done")

    def send_message(self, text: str, timeout: float = AI_TIMEOUT) -> Optional[dict]:
        """Send a user message and wait for assistant_done."""
        self.send({
            "type": "user_message",
            "sessionId": self.session_id,
            "text": text,
        })
        return self.wait_for("assistant_done", timeout=timeout)

    def request_completions(self, prefix: str) -> Optional[dict]:
        """Request auto-completions for a prefix."""
        self.send({
            "type": "completion_request",
            "prefix": prefix,
        })
        return self.wait_for("completion_response", timeout=5.0)

    def request_history(self, max_entries: int = 100) -> Optional[dict]:
        """Request command history."""
        self.send({
            "type": "get_history",
            "maxEntries": max_entries,
        })
        return self.wait_for("history", timeout=5.0)


# ---------------------------------------------------------------------------
# Test framework
# ---------------------------------------------------------------------------

class TestFailure(Exception):
    """Raised when a test check fails."""
    pass

def check(condition: bool, msg: str = ""):
    """Assert helper that raises TestFailure with a message."""
    if not condition:
        raise TestFailure(msg or "Check failed")

class TestResult:
    def __init__(self, name: str, passed: bool, message: str = "", duration: float = 0.0):
        self.name = name
        self.passed = passed
        self.message = message
        self.duration = duration

class TestSuite:
    def __init__(self):
        self.tests: List[tuple] = []  # (name, func, requires_ai)
        self.results: List[TestResult] = []

    def test(self, name: str, requires_ai: bool = False):
        """Decorator to register a test."""
        def decorator(func: Callable):
            self.tests.append((name, func, requires_ai))
            return func
        return decorator

    def run(self, client: AssistantClient, with_ai: bool = False, filter_str: str = ""):
        """Run all registered tests."""
        selected = [
            (n, f, ai) for n, f, ai in self.tests
            if (not ai or with_ai) and (not filter_str or filter_str.lower() in n.lower())
        ]

        print(f"\n{C.BOLD}AI Assistant Test Suite{C.RESET}")
        print(f"  Server:       {client.url}")
        print(f"  AI tests:     {'yes' if with_ai else 'no (use --with-ai)'}")
        print(f"  Auto-approve: {'yes' if client.auto_approve else 'no'}")
        print(f"  Tests:        {len(selected)}")
        print(f"{'─' * 60}\n")

        for name, func, _ in selected:
            t0 = time.time()
            try:
                func(client)
                dt = time.time() - t0
                self.results.append(TestResult(name, True, duration=dt))
                print(f"  {C.GREEN}✓{C.RESET} {name} {C.DIM}({dt:.1f}s){C.RESET}")
            except TestFailure as e:
                dt = time.time() - t0
                self.results.append(TestResult(name, False, str(e), dt))
                print(f"  {C.RED}✗{C.RESET} {name} {C.DIM}({dt:.1f}s){C.RESET}")
                print(f"    {C.RED}{e}{C.RESET}")
            except Exception as e:
                dt = time.time() - t0
                self.results.append(TestResult(name, False, str(e), dt))
                print(f"  {C.RED}✗{C.RESET} {name} {C.DIM}({dt:.1f}s){C.RESET}")
                print(f"    {C.RED}{traceback.format_exc().splitlines()[-1]}{C.RESET}")

        # Summary
        passed = sum(1 for r in self.results if r.passed)
        failed = sum(1 for r in self.results if not r.passed)
        total_time = sum(r.duration for r in self.results)
        print(f"\n{'─' * 60}")
        color = C.GREEN if failed == 0 else C.RED
        print(f"  {color}{passed} passed, {failed} failed{C.RESET} in {total_time:.1f}s\n")

        return failed == 0

# ---------------------------------------------------------------------------
# Test definitions
# ---------------------------------------------------------------------------

suite = TestSuite()

# --- Connection & session ---

@suite.test("Connect to WebSocket")
def test_connect(c: AssistantClient):
    check(c.ws is not None and c.ws.connected, "WebSocket not connected")

@suite.test("Create new session")
def test_new_session(c: AssistantClient):
    sid = c.new_session()
    check(sid.startswith("sess_"), f"Unexpected session ID format: {sid}")

@suite.test("List sessions")
def test_list_sessions(c: AssistantClient):
    c.send({"type": "list_sessions"})
    msg = c.wait_for("session_list")
    check(msg is not None, "No session_list response")
    sessions = msg.get("sessions", [])
    check(isinstance(sessions, list), "sessions is not a list")
    ids = [s["id"] if isinstance(s, dict) else s for s in sessions]
    check(c.session_id in ids, f"Current session {c.session_id} not in list: {ids}")

@suite.test("Resume session")
def test_resume_session(c: AssistantClient):
    c.send({"type": "resume_session", "sessionId": c.session_id})
    msg = c.wait_for("session_active")
    check(msg is not None, "No session_active response on resume")
    check(msg["sessionId"] == c.session_id, "Session ID mismatch on resume")

# --- Slash commands ---

@suite.test("Slash command: /help")
def test_cmd_help(c: AssistantClient):
    resp = c.send_command("help")
    check(resp is not None, "No response to /help")
    text = resp.get("text", "")
    check("/status" in text, f"/help output missing /status: {text[:100]}")
    check("/memory" in text, f"/help output missing /memory: {text[:100]}")

@suite.test("Slash command: /status")
def test_cmd_status(c: AssistantClient):
    resp = c.send_command("status")
    check(resp is not None, "No response to /status")
    text = resp.get("text", "")
    check("uptime" in text.lower() or "status" in text.lower(),
          f"/status output unexpected: {text[:100]}")

@suite.test("Slash command: /runs")
def test_cmd_runs(c: AssistantClient):
    resp = c.send_command("runs")
    check(resp is not None, "No response to /runs")

@suite.test("Slash command: /log")
def test_cmd_log(c: AssistantClient):
    resp = c.send_command("log", "5")
    check(resp is not None, "No response to /log")
    text = resp.get("text", "")
    check(len(text) > 0, "/log returned empty text")

@suite.test("Slash command: /memory")
def test_cmd_memory(c: AssistantClient):
    resp = c.send_command("memory")
    check(resp is not None, "No response to /memory")

@suite.test("Slash command: /index")
def test_cmd_index(c: AssistantClient):
    resp = c.send_command("index")
    check(resp is not None, "No response to /index")
    text = resp.get("text", "")
    check("index" in text.lower() or "files" in text.lower(),
          f"/index output unexpected: {text[:100]}")

# --- Completions ---

@suite.test("Completion: slash commands")
def test_completion_slash(c: AssistantClient):
    resp = c.request_completions("/he")
    check(resp is not None, "No completion response")
    candidates = resp.get("candidates", [])
    check("/help" in candidates, f"/help not in candidates: {candidates}")

@suite.test("Completion: slash prefix /mem")
def test_completion_memory(c: AssistantClient):
    resp = c.request_completions("/mem")
    check(resp is not None, "No completion response")
    candidates = resp.get("candidates", [])
    check("/memory" in candidates, f"/memory not in candidates: {candidates}")

@suite.test("Completion: empty prefix returns nothing")
def test_completion_empty(c: AssistantClient):
    resp = c.request_completions("")
    if resp:
        candidates = resp.get("candidates", [])
        check(isinstance(candidates, list), "candidates is not a list")

# --- History ---

@suite.test("History: retrieve entries")
def test_history(c: AssistantClient):
    resp = c.request_history(50)
    check(resp is not None, "No history response")
    entries = resp.get("entries", [])
    check(isinstance(entries, list), "entries is not a list")

# --- Additional slash command tests ---

@suite.test("Slash command: /sessions")
def test_cmd_sessions(c: AssistantClient):
    resp = c.send_command("sessions")
    check(resp is not None, "No response to /sessions")
    text = resp.get("text", "")
    check("sess_" in text, f"/sessions did not list session IDs: {text[:150]}")

# --- Session behavior tests ---
# (Run before /new which changes c.session_id)

@suite.test("Session: history replay on resume")
def test_session_history_replay(c: AssistantClient):
    # Prior slash-command tests added assistant turns to the session.
    c.send({"type": "resume_session", "sessionId": c.session_id})
    msg = c.wait_for("session_history")
    check(msg is not None, "No session_history message on resume")
    turns = msg.get("turns", [])
    check(len(turns) > 0, "Session history empty; expected turns from prior slash commands")

@suite.test("Session: turn count in session list")
def test_session_turn_count(c: AssistantClient):
    c.send({"type": "list_sessions"})
    msg = c.wait_for("session_list")
    check(msg is not None, "No session_list response")
    sessions = msg.get("sessions", [])
    current = next(
        (s for s in sessions if isinstance(s, dict) and s.get("id") == c.session_id),
        None,
    )
    check(current is not None, f"Current session {c.session_id} not in list")
    check(current.get("turns", 0) > 0, "Turn count is 0 after running slash commands")

@suite.test("Session: resume unknown session ID")
def test_session_resume_unknown(c: AssistantClient):
    c.send({"type": "resume_session", "sessionId": "sess_0000000000"})
    msg = c.wait_for("error")
    check(msg is not None, "No error response for unknown session ID")
    check("not found" in msg.get("message", "").lower(),
          f"Unexpected error message: {msg.get('message')}")

# --- More slash command tests ---

@suite.test("Slash command: /memory clear")
def test_cmd_memory_clear(c: AssistantClient):
    resp = c.send_command("memory", "clear")
    check(resp is not None, "No response to /memory clear")
    check("cleared" in resp.get("text", "").lower(),
          f"Unexpected response to /memory clear: {resp.get('text','')[:100]}")
    # Verify memory is now empty
    resp2 = c.send_command("memory")
    check(resp2 is not None, "No response to /memory after clear")
    text2 = resp2.get("text", "").lower()
    check("no memor" in text2 or "0 memor" in text2 or "empty" in text2,
          f"/memory after clear shows unexpected content: {resp2.get('text','')[:150]}")

@suite.test("Slash command: /index rescan")
def test_cmd_index_rescan(c: AssistantClient):
    resp = c.send_command("index", "rescan")
    check(resp is not None, "No response to /index rescan")
    text = resp.get("text", "")
    check("complete" in text.lower() or "indexed" in text.lower() or "rescan" in text.lower(),
          f"/index rescan unexpected output: {text[:150]}")

@suite.test("Slash command: /log large N")
def test_cmd_log_large(c: AssistantClient):
    resp = c.send_command("log", "999")
    check(resp is not None, "No response to /log 999")
    check(len(resp.get("text", "")) > 0, "/log 999 returned empty text")

@suite.test("Slash command: /log invalid arg")
def test_cmd_log_invalid(c: AssistantClient):
    resp = c.send_command("log", "abc")
    check(resp is not None, "No response to /log abc")
    # Any non-crashing response is acceptable

@suite.test("Slash command: /new")
def test_cmd_new(c: AssistantClient):
    old_sid = c.session_id
    c.send({"type": "command", "sessionId": c.session_id, "command": "new", "args": ""})
    # /new sends session_active then assistant_done — collect both
    new_sid = None
    got_done = False
    deadline = time.time() + DEFAULT_TIMEOUT
    while time.time() < deadline:
        msg = c.recv(timeout=min(1.0, deadline - time.time()))
        if msg is None:
            break
        if msg.get("type") == "session_active":
            new_sid = msg.get("sessionId", "")
        elif msg.get("type") == "assistant_done":
            got_done = True
            break
    check(got_done, "No assistant_done from /new command")
    check(new_sid is not None and new_sid.startswith("sess_"),
          f"Bad or missing session ID from /new: {new_sid}")
    check(new_sid != old_sid, "Session ID unchanged after /new")
    c.session_id = new_sid

@suite.test("Slash command: /clear")
def test_cmd_clear(c: AssistantClient):
    c.send({"type": "command", "sessionId": c.session_id, "command": "clear", "args": ""})
    msg = c.wait_for("clear")
    check(msg is not None, "No clear message from /clear command")

# --- Protocol edge cases ---

@suite.test("Protocol: unknown message type")
def test_protocol_unknown_type(c: AssistantClient):
    c.send({"type": "xyzzy_unknown_9999"})
    msg = c.wait_for("error")
    check(msg is not None, "No error response for unknown message type")

@suite.test("Protocol: malformed JSON")
def test_protocol_malformed_json(c: AssistantClient):
    c.ws.send("{not valid json !!!")
    msg = c.wait_for("error")
    check(msg is not None, "No error response for malformed JSON")

@suite.test("Protocol: empty text in user_message")
def test_protocol_empty_text(c: AssistantClient):
    c.send({"type": "user_message", "sessionId": c.session_id, "text": ""})
    # Server silently ignores empty messages — no assistant_done should arrive
    msg = c.recv(timeout=2.0)
    if msg is not None:
        check(msg.get("type") != "assistant_done",
              "Got assistant_done for empty user_message")

@suite.test("Protocol: stale approval_response")
def test_protocol_stale_approval(c: AssistantClient):
    # Unknown requestId should be silently ignored — connection must stay healthy
    c.send({
        "type": "approval_response",
        "requestId": "req_does_not_exist_99999",
        "approved": True,
    })
    c.send({"type": "list_sessions"})
    msg = c.wait_for("session_list")
    check(msg is not None, "Connection broken after stale approval_response")

# ---------------------------------------------------------------------------
# Helpers for AI-powered tests
# ---------------------------------------------------------------------------

def _wait_for_done_respond_approval(
    c: AssistantClient, approved: bool, timeout: float = AI_TIMEOUT
) -> Optional[dict]:
    """
    Wait for assistant_done, responding to every approval_request with the
    given ``approved`` flag.  Returns the assistant_done message or None on timeout.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.1, deadline - time.time())
        msg = c.recv(timeout=remaining)
        if msg is None:
            return None
        if msg.get("type") == "assistant_done":
            return msg
        if msg.get("type") == "approval_request":
            c.send({
                "type": "approval_response",
                "requestId": msg["requestId"],
                "approved": approved,
            })
    return None

# ---------------------------------------------------------------------------
# AI-powered tests (require --with-ai)
# ---------------------------------------------------------------------------

@suite.test("AI: simple question", requires_ai=True)
def test_ai_simple(c: AssistantClient):
    resp = c.send_message("What is JarvisAgent? Answer in one sentence.")
    check(resp is not None, "No assistant_done response (AI provider timeout?)")
    text = resp.get("text", "")
    check(len(text) > 10, f"AI response too short: {text}")

@suite.test("AI: tool call — list_workflows", requires_ai=True)
def test_ai_list_workflows(c: AssistantClient):
    resp = c.send_message("List all registered workflows. Use the list_workflows tool.")
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 5, f"AI response too short: {text}")

@suite.test("AI: tool call — get_system_status", requires_ai=True)
def test_ai_system_status(c: AssistantClient):
    resp = c.send_message("What is the current system status? Use get_system_status.")
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 10, f"AI response too short: {text}")

@suite.test("AI: tool call — search_files", requires_ai=True)
def test_ai_search(c: AssistantClient):
    resp = c.send_message(
        "Search for 'WrapForBash' in the codebase using the search_files tool."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check("WrapForBash" in text or "assistantTools" in text or "found" in text.lower(),
          f"Search result seems wrong: {text[:200]}")

@suite.test("AI: tool call — read_file", requires_ai=True)
def test_ai_read_file(c: AssistantClient):
    resp = c.send_message(
        "Read the first 10 lines of code/backend/application/assistant/ai-assistant.md using the read_file tool."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check("AI Assistant" in text or "assistant" in text.lower(),
          f"read_file result seems wrong: {text[:200]}")

@suite.test("AI: tool call — list_files", requires_ai=True)
def test_ai_list_files(c: AssistantClient):
    resp = c.send_message(
        "List the files in the code/backend/application/assistant/ directory using list_files."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check("assistant" in text.lower(),
          f"list_files result seems wrong: {text[:200]}")

@suite.test("AI: memory — save and recall", requires_ai=True)
def test_ai_memory(c: AssistantClient):
    resp = c.send_message(
        "Save a memory with key 'test_key_12345' and value 'test_value_hello'. "
        "Use the save_memory tool."
    )
    check(resp is not None, "No response to save_memory")
    resp2 = c.send_message(
        "Recall the memory with key 'test_key_12345' using recall_memory."
    )
    check(resp2 is not None, "No response to recall_memory")
    text2 = resp2.get("text", "")
    check("test_value_hello" in text2 or "test_key" in text2,
          f"recall_memory did not find saved value: {text2[:200]}")
    c.send_message("Delete the memory with key 'test_key_12345' using delete_memory.")

@suite.test("AI: get_log_tail tool", requires_ai=True)
def test_ai_log_tail(c: AssistantClient):
    resp = c.send_message("Show the last 5 lines of the log using get_log_tail.")
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 5, f"get_log_tail response too short: {text}")

@suite.test("AI: get_dashboard_status tool", requires_ai=True)
def test_ai_dashboard(c: AssistantClient):
    resp = c.send_message("Give me a full dashboard status using get_dashboard_status.")
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 20, f"Dashboard response too short: {text}")

@suite.test("AI: multi-step tool use", requires_ai=True)
def test_ai_multistep(c: AssistantClient):
    resp = c.send_message(
        "First use get_system_status to check the system, then use list_workflows "
        "to list workflows. Report both results."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 20, f"Multi-step response too short: {text}")

@suite.test("AI: run_shell (requires approval)", requires_ai=True)
def test_ai_run_shell(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    resp = c.send_message("Run 'echo hello_from_test' using run_shell.")
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check("hello_from_test" in text or "echo" in text,
          f"run_shell result seems wrong: {text[:200]}")

# --- Approval flow ---

@suite.test("AI: approval — deny path", requires_ai=True)
def test_ai_approval_deny(c: AssistantClient):
    c.send({"type": "user_message", "sessionId": c.session_id,
            "text": "Run the shell command 'echo deny_path_test' using run_shell."})
    resp = _wait_for_done_respond_approval(c, approved=False)
    check(resp is not None, "No assistant_done after denial")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response after denial")
    # The AI may mention the command name in its "denied" response — that is
    # expected.  What must NOT happen is the literal echo output appearing as
    # if the shell actually ran.  We verify the response does not look like
    # raw shell output (i.e. the word alone on a line or preceded by a prompt).
    import re as _re
    check(not _re.search(r'(?m)^deny_path_test\s*$', text),
          "Response looks like raw shell output despite denial")

@suite.test("AI: approval — request message structure", requires_ai=True)
def test_ai_approval_structure(c: AssistantClient):
    # Use a fresh session so prior denial context does not deter the AI from
    # calling run_shell again.
    fresh_sid = c.new_session()
    c.send({"type": "user_message", "sessionId": fresh_sid,
            "text": "Run the shell command 'echo approval_structure_test' using run_shell."})
    found_approval = False
    deadline = time.time() + AI_TIMEOUT
    while time.time() < deadline:
        msg = c.recv(timeout=min(2.0, deadline - time.time()))
        if msg is None:
            break
        if msg.get("type") == "approval_request":
            check("requestId"   in msg, "Missing requestId in approval_request")
            check("tool"        in msg, "Missing tool in approval_request")
            check("args"        in msg, "Missing args in approval_request")
            check("description" in msg, "Missing description in approval_request")
            check(isinstance(msg.get("args"), dict), "args is not a dict")
            check(len(msg.get("requestId", "")) > 0, "requestId is empty")
            found_approval = True
            c.send({"type": "approval_response",
                    "requestId": msg["requestId"], "approved": False})
        elif msg.get("type") == "assistant_done":
            break
    check(found_approval, "No approval_request received for run_shell")

# --- Memory ---

@suite.test("AI: memory — update existing key", requires_ai=True)
def test_ai_memory_update(c: AssistantClient):
    c.send_message(
        "Save memory key 'update_test_key_99' with value 'first_value' using save_memory."
    )
    c.send_message(
        "Save memory key 'update_test_key_99' with value 'second_value' using save_memory."
    )
    resp = c.send_message(
        "Recall 'update_test_key_99' using recall_memory and show the current value."
    )
    check(resp is not None, "No response to recall_memory")
    text = resp.get("text", "")
    check("second_value" in text,
          f"Updated value not found in recall: {text[:200]}")
    c.send_message("Delete memory key 'update_test_key_99' using delete_memory.")

@suite.test("AI: memory — list_memories tool", requires_ai=True)
def test_ai_memory_list(c: AssistantClient):
    # Fresh session — avoids the AI hallucinating tool calls based on the
    # heavy memory-operation context accumulated in the previous tests.
    c.new_session()
    c.send_message(
        "Save memory key 'list_test_key_99' value 'list_test_value' using save_memory."
    )
    resp = c.send_message("List all stored memories using the list_memories tool.")
    check(resp is not None, "No response to list_memories")
    text = resp.get("text", "")
    check(len(text) > 5, f"list_memories response too short: {text}")
    check("list_test" in text,
          f"Saved key not found in list_memories output: {text[:200]}")
    c.send_message("Delete memory key 'list_test_key_99' using delete_memory.")

# --- Read-only tools not yet covered ---

@suite.test("AI: tool call — list_recent_runs", requires_ai=True)
def test_ai_list_recent_runs(c: AssistantClient):
    resp = c.send_message(
        "List the 5 most recent workflow runs using list_recent_runs."
    )
    check(resp is not None, "No assistant_done response")
    check(len(resp.get("text", "")) > 5, "list_recent_runs response too short")

@suite.test("AI: tool call — get_run_status", requires_ai=True)
def test_ai_get_run_status(c: AssistantClient):
    resp = c.send_message(
        "Use get_run_status to check run ID 'run_nonexistent_test_9999' "
        "and report what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    check(len(resp.get("text", "")) > 5, "get_run_status response too short")

@suite.test("AI: tool call — get_task_output", requires_ai=True)
def test_ai_get_task_output(c: AssistantClient):
    resp = c.send_message(
        "Use get_task_output for run 'run_nonexistent_9999' task 'task_none' "
        "and report what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    check(len(resp.get("text", "")) > 5, "get_task_output response too short")

@suite.test("AI: tool call — get_file_summary", requires_ai=True)
def test_ai_get_file_summary(c: AssistantClient):
    resp = c.send_message(
        "Use get_file_summary on 'code/backend/application/assistant/assistantController.h' "
        "and show the summary."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 20, f"get_file_summary response too short: {text}")
    check(
        "assistant" in text.lower() or "websocket" in text.lower()
        or "controller" in text.lower() or "session" in text.lower(),
        f"get_file_summary content seems unrelated: {text[:200]}",
    )

@suite.test("AI: tool call — get_folder_summary", requires_ai=True)
def test_ai_get_folder_summary(c: AssistantClient):
    resp = c.send_message(
        "Use get_folder_summary on 'code/backend/application/assistant/' and describe what you find."
    )
    check(resp is not None, "No assistant_done response")
    check(len(resp.get("text", "")) > 20, "get_folder_summary response too short")

@suite.test("AI: tool call — jcwf_read", requires_ai=True)
def test_ai_jcwf_read(c: AssistantClient):
    resp = c.send_message(
        "Use list_workflows to find registered workflows. "
        "If any exist, use jcwf_read on the first one and show its content. "
        "If none exist, reply exactly: NO_WORKFLOWS"
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    if "NO_WORKFLOWS" in text:
        print(f"    {C.YELLOW}⚠ No workflows registered{C.RESET}")
        return
    check(len(text) > 10, f"jcwf_read response too short: {text}")

@suite.test("AI: tool call — jcwf_explain", requires_ai=True)
def test_ai_jcwf_explain(c: AssistantClient):
    resp = c.send_message(
        "Use list_workflows to find registered workflows. "
        "If any exist, use jcwf_explain on the first one. "
        "If none exist, reply exactly: NO_WORKFLOWS"
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    if "NO_WORKFLOWS" in text:
        print(f"    {C.YELLOW}⚠ No workflows registered{C.RESET}")
        return
    check(len(text) > 10, f"jcwf_explain response too short: {text}")

@suite.test("AI: tool call — jcwf_validate", requires_ai=True)
def test_ai_jcwf_validate(c: AssistantClient):
    resp = c.send_message(
        "Use list_workflows to find registered workflows. "
        "If any exist, use jcwf_validate on the first one and report the result. "
        "If none exist, reply exactly: NO_WORKFLOWS"
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    if "NO_WORKFLOWS" in text:
        print(f"    {C.YELLOW}⚠ No workflows registered{C.RESET}")
        return
    check(len(text) > 5, f"jcwf_validate response too short: {text}")

@suite.test("AI: tool call — jcwf_read_plan", requires_ai=True)
def test_ai_jcwf_read_plan(c: AssistantClient):
    resp = c.send_message(
        "Use list_workflows to find registered workflows. "
        "If any exist, use jcwf_read_plan on the first one and show the content "
        "(or say if no plan file exists for it). "
        "If no workflows exist, reply exactly: NO_WORKFLOWS"
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    if "NO_WORKFLOWS" in text:
        print(f"    {C.YELLOW}⚠ No workflows registered{C.RESET}")
        return
    check(len(text) > 5, f"jcwf_read_plan response too short: {text}")

@suite.test("AI: tool call — read_file denied (config.json)", requires_ai=True)
def test_ai_read_file_denied(c: AssistantClient):
    resp = c.send_message(
        "Attempt to read 'config.json' using the read_file tool and report what happens."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "").lower()
    check(
        "denied" in text or "not allowed" in text or "access" in text
        or "permission" in text or "sensitive" in text or "restrict" in text
        or "cannot" in text or "can't" in text or "protect" in text
        or "refuse" in text or "blocked" in text,
        f"read_file config.json did not report denial: {text[:200]}",
    )

# --- Mutating tools (require --auto-approve) ---

@suite.test("AI: tool call — write_file", requires_ai=True)
def test_ai_write_file(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    import pathlib
    # write_file paths must be relative to the project root (absolute paths are rejected).
    test_path = f"tmp/j9t_test_write_{int(time.time())}.txt"
    resp = c.send_message(
        f"Write the text 'j9t_write_test_content' to '{test_path}' using write_file. "
        f"The path is relative to the project root."
    )
    check(resp is not None, "No assistant_done response")
    p = pathlib.Path(test_path)
    check(p.exists(), f"File was not created: {test_path}")
    content = p.read_text()
    check("j9t_write_test_content" in content,
          f"Written content not found: {content[:100]}")
    p.unlink(missing_ok=True)
    pathlib.Path(test_path + ".bak").unlink(missing_ok=True)

@suite.test("AI: tool call — edit_file", requires_ai=True)
def test_ai_edit_file(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    import pathlib
    # edit_file paths must be relative to the project root (absolute paths are rejected).
    test_path = f"tmp/j9t_test_edit_{int(time.time())}.txt"
    p = pathlib.Path(test_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("j9t_original_line\n")
    resp = c.send_message(
        f"Edit '{test_path}' using edit_file: "
        f"replace 'j9t_original_line' with 'j9t_edited_line'. "
        f"The path is relative to the project root."
    )
    check(resp is not None, "No assistant_done response")
    check(p.exists(), f"File missing after edit: {test_path}")
    content = p.read_text()
    check("j9t_edited_line" in content, f"Edit not applied: {content[:100]}")
    check("j9t_original_line" not in content, f"Old content still present: {content[:100]}")
    p.unlink(missing_ok=True)
    pathlib.Path(test_path + ".bak").unlink(missing_ok=True)

@suite.test("AI: tool call — jcwf_write_plan", requires_ai=True)
def test_ai_jcwf_write_plan(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    resp = c.send_message(
        "Use list_workflows to find a registered workflow. "
        "If one exists, use jcwf_write_plan to write 'Automated test plan.' for it "
        "and confirm what was written. "
        "If no workflows exist, reply exactly: NO_WORKFLOWS"
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    if "NO_WORKFLOWS" in text:
        print(f"    {C.YELLOW}⚠ No workflows registered{C.RESET}")
        return
    check(
        "plan" in text.lower() or "written" in text.lower() or "created" in text.lower(),
        f"jcwf_write_plan result unexpected: {text[:200]}",
    )

# --- Memory edge cases ---

@suite.test("AI: memory — delete non-existent key", requires_ai=True)
def test_ai_memory_delete_nonexistent(c: AssistantClient):
    # Use a fresh session and a unique key that is guaranteed not to exist.
    c.new_session()
    resp = c.send_message(
        "Use delete_memory to delete the key 'no_such_key_xyzzy_99999'. "
        "Report exactly what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response")
    # Server returns "Memory not found" — AI should relay that gracefully (no crash/exception).
    check(
        "not found" in text.lower() or "does not exist" in text.lower()
        or "no memory" in text.lower() or "couldn't find" in text.lower()
        or "could not find" in text.lower() or "no such" in text.lower()
        or "xyzzy" in text.lower(),
        f"delete_memory non-existent key gave unexpected response: {text[:200]}",
    )

# --- Error handling: read_file edge cases ---

@suite.test("AI: read_file — nonexistent file", requires_ai=True)
def test_ai_read_file_nonexistent(c: AssistantClient):
    resp = c.send_message(
        "Use read_file to read 'code/backend/application/assistant/does_not_exist_xyzzy.txt'. "
        "Report exactly what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response")
    check(
        "not found" in text.lower() or "does not exist" in text.lower()
        or "no such" in text.lower() or "cannot" in text.lower()
        or "error" in text.lower() or "xyzzy" in text.lower(),
        f"read_file nonexistent did not report an error: {text[:200]}",
    )

@suite.test("AI: read_file — path traversal rejected", requires_ai=True)
def test_ai_read_file_traversal(c: AssistantClient):
    resp = c.send_message(
        "Use read_file to read '../../../etc/hosts'. "
        "Report exactly what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response")
    # Server rejects with "Path traversal not allowed" — AI relays the error.
    # Accept either an explicit traversal rejection or "file not found" —
    # both mean the traversal did not expose data outside the project.
    check(
        "not allowed" in text.lower() or "traversal" in text.lower()
        or "denied" in text.lower() or "rejected" in text.lower()
        or "cannot" in text.lower() or "error" in text.lower()
        or "not found" in text.lower(),
        f"read_file path traversal was not rejected: {text[:200]}",
    )

# --- Error handling: write_file edge cases (require --auto-approve) ---

@suite.test("AI: write_file — path traversal rejected", requires_ai=True)
def test_ai_write_file_traversal(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    resp = c.send_message(
        "Use write_file to write 'test' to '../../../tmp/j9t_traversal_test.txt'. "
        "Report exactly what the tool returns."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response")
    check(
        "not allowed" in text.lower() or "traversal" in text.lower()
        or "denied" in text.lower() or "rejected" in text.lower()
        or "cannot" in text.lower() or "error" in text.lower(),
        f"write_file path traversal was not rejected: {text[:200]}",
    )
    import pathlib
    assert not pathlib.Path("/tmp/j9t_traversal_test.txt").exists(), \
        "write_file traversal test: file was actually written!"

@suite.test("AI: write_file — denied by user", requires_ai=True)
def test_ai_write_file_denied(c: AssistantClient):
    # Do NOT use auto-approve — we manually deny the approval request.
    test_path = f"tmp/j9t_write_denied_{int(time.time())}.txt"
    c.send({
        "type": "user_message",
        "sessionId": c.session_id,
        "text": f"Write the text 'should_not_appear' to '{test_path}' using write_file.",
    })
    resp = _wait_for_done_respond_approval(c, approved=False)
    check(resp is not None, "No assistant_done after denial")
    import pathlib
    check(not pathlib.Path(test_path).exists(),
          f"File was created despite denial: {test_path}")

# --- Mutating workflow tools (require --auto-approve) ---

_TEST_WORKFLOW_ID = "j9t_test_workflow_auto"

def _ensure_test_workflow(c: AssistantClient) -> bool:
    """Create a minimal test workflow (.jcwf zip container) if it doesn't exist. Returns True if ready."""
    import pathlib, json as _json, zipfile, io
    wf_path = pathlib.Path(f"workflows/{_TEST_WORKFLOW_ID}.jcwf")
    if wf_path.exists():
        return True
    # Build a minimal valid JCWF zip container directly (no AI needed).
    global_json = {
        "version": "1.0",
        "id": _TEST_WORKFLOW_ID,
        "label": "Auto-test workflow",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True}],
    }
    canvas_json = {
        "tasks": {
            "hello": {
                "id": "hello",
                "type": "shell",
                "label": "Say hello",
                "params": {"command": "scripts/j9t_test_hello.sh"},
                "working_directory": f"{_TEST_WORKFLOW_ID}/01_hello",
            }
        },
    }
    wf_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(wf_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("global.json", _json.dumps(global_json, indent=2))
        zf.writestr(f"{_TEST_WORKFLOW_ID}.json", _json.dumps(canvas_json, indent=2))
    return wf_path.exists()

@suite.test("AI: tool call — jcwf_generate", requires_ai=True)
def test_ai_jcwf_generate(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    import pathlib
    # Ensure the test workflow exists, then write a plan for it.
    _ensure_test_workflow(c)

    # Write a plan first (required by jcwf_generate).
    plan_resp = c.send_message(
        f"Use jcwf_write_plan for workflow '{_TEST_WORKFLOW_ID}' with the content: "
        f"'A simple hello-world workflow that runs a shell echo command.'"
    )
    check(plan_resp is not None, "No response from jcwf_write_plan")

    # Now generate the workflow from the plan.
    resp = c.send_message(
        f"Use jcwf_generate for workflow '{_TEST_WORKFLOW_ID}' to regenerate it from the plan. "
        f"Report what happened."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 5, f"jcwf_generate response too short: {text}")
    check(
        "generat" in text.lower() or "written" in text.lower() or "created" in text.lower()
        or "workflow" in text.lower() or "success" in text.lower(),
        f"jcwf_generate result unexpected: {text[:200]}",
    )
    # Verify the .jcwf file was (re)written.
    wf_path = pathlib.Path(f"workflows/{_TEST_WORKFLOW_ID}.jcwf")
    check(wf_path.exists(), f"Workflow file not found after jcwf_generate: {wf_path}")

@suite.test("AI: tool call — jcwf_fix_task", requires_ai=True)
def test_ai_jcwf_fix_task(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    import pathlib
    _ensure_test_workflow(c)
    resp = c.send_message(
        f"Use jcwf_fix_task on workflow '{_TEST_WORKFLOW_ID}', task 'hello', "
        f"with instructions: 'Update the label to say Hello World task'. "
        f"Report what happened."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 5, f"jcwf_fix_task response too short: {text}")
    check(
        "fix" in text.lower() or "updat" in text.lower() or "task" in text.lower()
        or "hello" in text.lower() or "success" in text.lower(),
        f"jcwf_fix_task result unexpected: {text[:200]}",
    )

@suite.test("AI: tool call — jcwf_write_script", requires_ai=True)
def test_ai_jcwf_write_script(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    import pathlib
    # Fresh session — avoids confusion from heavy accumulated context.
    c.new_session()
    script_path = "scripts/j9t_test_hello.sh"
    resp = c.send_message(
        f"Use jcwf_write_script to write a shell script to '{script_path}'. "
        f"The script should print 'hello from j9t test'. "
        f"Type is 'shell'. Report what happened."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 5, f"jcwf_write_script response too short: {text}")
    check(
        "written" in text.lower() or "created" in text.lower() or "script" in text.lower()
        or "success" in text.lower(),
        f"jcwf_write_script result unexpected: {text[:200]}",
    )
    # Verify the script was written and has required safety header.
    p = pathlib.Path(script_path)
    check(p.exists(), f"Script not found after jcwf_write_script: {script_path}")
    content = p.read_text()
    check("#!/" in content, "Script missing shebang")
    check("set -euo pipefail" in content, "Script missing 'set -euo pipefail'")
    check("hello from j9t test" in content, "Script missing expected content")
    # Clean up.
    p.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Workflow runtime control tests
# These tests use jarvisCppDocu as a long-running fixture (many AI tasks).
# They require --with-ai and --auto-approve.
# ---------------------------------------------------------------------------

_RUNTIME_WORKFLOW_ID = "jarvisCppDocu"

def _extract_run_id(text: str) -> Optional[str]:
    """Extract run ID from an AI response containing 'run ID: <id>'."""
    import re as _re
    m = _re.search(r'run[_ ]id[:\s]+([A-Za-z0-9_\-]+)', text, _re.IGNORECASE)
    return m.group(1) if m else None

def _start_workflow(c: AssistantClient) -> str:
    """Start jarvisCppDocu and return the run_id. Raises TestFailure on error."""
    c.new_session()
    resp = c.send_message(
        f"Use run_workflow to start the workflow '{_RUNTIME_WORKFLOW_ID}'. "
        f"Report the run ID exactly as returned by the tool."
    )
    check(resp is not None, "No assistant_done response from run_workflow")
    text = resp.get("text", "")
    check(
        "run" in text.lower() and ("start" in text.lower() or "id" in text.lower()),
        f"run_workflow response unexpected: {text[:200]}",
    )
    run_id = _extract_run_id(text)
    check(run_id is not None, f"Could not extract run_id from response: {text[:200]}")
    return run_id

@suite.test("AI: tool call — run_workflow", requires_ai=True)
def test_ai_run_workflow(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    run_id = _start_workflow(c)
    # Verify run appears in dashboard / recent runs.
    status_resp = c.send_message(
        f"Use get_run_status to check run '{run_id}' and report its state."
    )
    check(status_resp is not None, "No response from get_run_status")
    status_text = status_resp.get("text", "").lower()
    check(
        "running" in status_text or "pending" in status_text
        or "complete" in status_text or "success" in status_text
        or run_id.lower() in status_text,
        f"Run state not found in response: {status_text[:200]}",
    )
    # Stop the run to leave a clean state.
    c.send_message(f"Use workflow_stop to stop run '{run_id}'.")

@suite.test("AI: tool call — workflow_pause and workflow_resume", requires_ai=True)
def test_ai_workflow_pause_resume(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    run_id = _start_workflow(c)
    # Give the engine a moment to start executing tasks.
    time.sleep(2)
    # Pause.
    pause_resp = c.send_message(
        f"Use workflow_pause to pause run '{run_id}'. Report what the tool returns."
    )
    check(pause_resp is not None, "No response from workflow_pause")
    pause_text = pause_resp.get("text", "").lower()
    check(
        "pause" in pause_text or run_id.lower() in pause_text,
        f"workflow_pause response unexpected: {pause_text[:200]}",
    )
    # Verify paused state.
    time.sleep(1)
    state_resp = c.send_message(
        f"Use get_run_status to check run '{run_id}' and report its state."
    )
    check(state_resp is not None, "No response from get_run_status after pause")
    state_text = state_resp.get("text", "").lower()
    check(
        "pause" in state_text or run_id.lower() in state_text,
        f"Run not paused: {state_text[:200]}",
    )
    # Resume.
    resume_resp = c.send_message(
        f"Use workflow_resume to resume run '{run_id}'. Report what the tool returns."
    )
    check(resume_resp is not None, "No response from workflow_resume")
    resume_text = resume_resp.get("text", "").lower()
    check(
        "resum" in resume_text or run_id.lower() in resume_text,
        f"workflow_resume response unexpected: {resume_text[:200]}",
    )
    # Stop to clean up.
    c.send_message(f"Use workflow_stop to stop run '{run_id}'.")

@suite.test("AI: tool call — workflow_stop", requires_ai=True)
def test_ai_workflow_stop(c: AssistantClient):
    if not c.auto_approve:
        print(f"    {C.YELLOW}⚠ Skipped (use --auto-approve to test){C.RESET}")
        return
    run_id = _start_workflow(c)
    time.sleep(1)
    # Stop.
    stop_resp = c.send_message(
        f"Use workflow_stop to stop run '{run_id}'. Report what the tool returns."
    )
    check(stop_resp is not None, "No response from workflow_stop")
    stop_text = stop_resp.get("text", "").lower()
    check(
        "stop" in stop_text or "cancel" in stop_text or run_id.lower() in stop_text,
        f"workflow_stop response unexpected: {stop_text[:200]}",
    )
    # Verify stopped state.
    time.sleep(1)
    state_resp = c.send_message(
        f"Use get_run_status to check run '{run_id}' and report its state."
    )
    check(state_resp is not None, "No response from get_run_status after stop")
    state_text = state_resp.get("text", "").lower()
    check(
        "stop" in state_text or "cancel" in state_text or "fail" in state_text
        or run_id.lower() in state_text,
        f"Run not stopped: {state_text[:200]}",
    )


# ---------------------------------------------------------------------------
# Multi-step loop behavior tests
# ---------------------------------------------------------------------------

@suite.test("AI: loop detection — same tool called 4+ times", requires_ai=True)
def test_ai_loop_detection(c: AssistantClient):
    # get_system_status takes no arguments, so every call has an identical
    # signature.  Calling it 5 times in a row must trigger loop detection
    # on the 4th call: the server injects a "Loop detected" error result and
    # forces the AI to give a final answer instead of looping indefinitely.
    c.new_session()
    resp = c.send_message(
        "Call get_system_status exactly 5 times in a row and list all 5 results. "
        "Do not summarise — show each result separately.",
        timeout=AI_TIMEOUT * 2,
    )
    check(resp is not None, "No assistant_done response (timeout?)")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response after loop detection")
    # The AI must have received the loop-detected error and given a final answer.
    # We do NOT require "loop" to appear in the text — the AI may just summarise
    # however many results it got.  The key assertion is that a response arrived
    # (no hang) and it is non-empty.

@suite.test("AI: response validation — hallucinated file path warning", requires_ai=True)
def test_ai_hallucinated_path_warning(c: AssistantClient):
    # Force the AI to mention a specific non-existent path without using tools.
    # The response validator will detect the path, confirm it doesn't exist, and
    # append "Note: <path> was not found in the workspace."
    c.new_session()
    fake_path = "code/backend/application/docs/nonexistent_xyzzy_hallucination.h"
    resp = c.send_message(
        f"Without using any tools, write one sentence describing what "
        f"'{fake_path}' might contain."
    )
    check(resp is not None, "No assistant_done response")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response")
    # The validator appends this note when it finds a mentioned path that doesn't exist.
    check(
        "not found in the workspace" in text.lower() or "was not found" in text.lower(),
        f"Expected file-not-found note in response, got: {text[:300]}",
    )

@suite.test("AI: max iterations — 10 distinct tool calls", requires_ai=True)
def test_ai_max_iterations(c: AssistantClient):
    # Ask for get_run_status on 10 distinct (non-existent) run IDs.  Each call
    # has a unique signature so loop detection does NOT fire.  Instead the engine
    # hits MAX_TOOL_ITERATIONS (10) and either forces a final answer on the last
    # iteration or emits "Reached maximum tool call iterations".
    c.new_session()
    run_ids = " ".join(f"run_loop_test_{i:02d}" for i in range(1, 11))
    resp = c.send_message(
        f"Use get_run_status to check each of the following run IDs one by one and "
        f"report the status for each: {run_ids}",
        timeout=AI_TIMEOUT * 3,
    )
    check(resp is not None, "No assistant_done response (timeout?)")
    text = resp.get("text", "")
    check(len(text) > 0, "Empty response after max iterations")
    # Either a summary of results or the "Reached maximum" safety message.
    check(
        "run_loop_test" in text.lower()
        or "maximum" in text.lower()
        or "iteration" in text.lower()
        or "status" in text.lower(),
        f"Unexpected response after max-iterations scenario: {text[:200]}",
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="AI Assistant E2E Test Suite")
    parser.add_argument("--with-ai", action="store_true",
                        help="Include tests that require a configured AI provider")
    parser.add_argument("--auto-approve", action="store_true",
                        help="Auto-approve mutating tool calls (run_shell, write_file, etc.)")
    parser.add_argument("--list", action="store_true",
                        help="List available tests and exit")
    parser.add_argument("-k", "--filter", default="",
                        help="Only run tests whose name contains this string")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Show raw WebSocket messages")
    parser.add_argument("--url", default=WS_URL,
                        help=f"WebSocket URL (default: {WS_URL})")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN", ""),
                        help="Bearer token for WS handshake (default: $J9T_TOKEN); required when "
                             "the server runs with auth enabled — which is the default.")
    args = parser.parse_args()

    if args.list:
        print(f"\n{C.BOLD}Available tests:{C.RESET}\n")
        for name, _, ai in suite.tests:
            tag = f" {C.YELLOW}[AI]{C.RESET}" if ai else ""
            print(f"  • {name}{tag}")
        print(f"\n  Total: {len(suite.tests)} ({sum(1 for _, _, ai in suite.tests if ai)} require --with-ai)\n")
        return

    client = AssistantClient(url=args.url, auto_approve=args.auto_approve, verbose=args.verbose,
                             token=args.token or None)

    try:
        print(f"\n{C.CYAN}Connecting to {args.url} ...{C.RESET}")
        client.connect()
        client.drain(timeout=1.0)
        print(f"{C.GREEN}Connected.{C.RESET}")
    except Exception as e:
        print(f"\n{C.RED}Failed to connect to {args.url}{C.RESET}")
        print(f"{C.RED}{e}{C.RESET}")
        print(f"\nMake sure JarvisAgent is running (bin/Release/jarvisAgent or bin/Debug/jarvisAgent).")
        sys.exit(1)

    try:
        success = suite.run(client, with_ai=args.with_ai, filter_str=args.filter)
    finally:
        client.close()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
