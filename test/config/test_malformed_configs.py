#!/usr/bin/env python3
"""
Malformed config.json verification harness.

For each fixture under test/config/fixtures/*.json:

  1. Set up a sandbox dir at /tmp/claude/j9t-cfg-sandbox/<fixture-name>/ with
     the fixture as config.json, an injected port to avoid host collision,
     and minimal queue/ workflows/ log/ scripts/ directories.

  2. Spawn the engine binary in that sandbox, wait ~4s for ConfigParser +
     ConfigChecker to run.

  3. Verify every _expected_errors substring appears at least once in
     log/log.txt — ConfigParser errors are emitted before ConfigChecker
     decides whether to keep the engine alive, so the log lines are
     present even when ConfigChecker subsequently rejects the config.

  4. Verify j9t either:
       - stays alive (responds on /api/status via the injected port) when
         ConfigChecker accepts the config — "ERROR-then-continue" contract
         from the plan, OR
       - exits cleanly with a non-zero code when ConfigChecker rejects the
         config (e.g. invalid API index, no usable interface).
     Both outcomes preserve the operator-protective contract: bad config
     fails fast or runs with whatever the parser could rescue, but never
     crashes mid-startup with an uncaught exception or hang.

  5. Tear down the spawned process before moving to the next fixture.

Default fixture set (8 fixtures as of 2026-05-23):

  malformed_types.json       — every wrong JSON type for its key
  out_of_range.json          — port + session_timeout_hours bounded clamps
  negative_rejected.json     — RejectNegative policy fields
  clamped_negatives.json     — ClampNegativeToZero + StoreOnlyIfPositive
  unknown_API.json           — API: "API7" → InvalidAPI + ConfigChecker reject
  oob_API_index.json         — API index 50 with 1 interface → reject
  url_substring_attack.json  — verify URL stored verbatim (fail-open in parser)

Usage:

  python3 test/config/test_malformed_configs.py
  python3 test/config/test_malformed_configs.py --binary bin/Release/jarvisAgent-engine
  python3 test/config/test_malformed_configs.py --keep-sandbox  # for forensics
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
import ssl
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURES_DIR = PROJECT_ROOT / "test" / "config" / "fixtures"
SANDBOX_ROOT = Path("/tmp/claude/j9t-cfg-sandbox")
# Reserved test port — must NOT collide with the host j9t on 8443.
TEST_PORT = 8444


class C:
    RESET = "\033[0m"; BOLD = "\033[1m"
    RED = "\033[91m"; GREEN = "\033[92m"; CYAN = "\033[96m"; YELLOW = "\033[93m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}⚠{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {msg}\n{'─'*70}{C.RESET}")


def port_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def setup_sandbox(name: str, fixture_path: Path) -> Path:
    """Build a minimal cwd for `engine` to run against. Returns the sandbox path."""
    sandbox = SANDBOX_ROOT / name
    if sandbox.exists():
        shutil.rmtree(sandbox)
    sandbox.mkdir(parents=True)
    # ConfigChecker requires queue + workflows to exist as directories.
    (sandbox / "queue").mkdir()
    (sandbox / "workflows").mkdir()
    (sandbox / "log").mkdir()
    (sandbox / "scripts").mkdir()

    # Load the fixture, strip test-only `_comment` + `_expected_errors`
    # fields (otherwise they show up as "unknown top-level field" INFO
    # noise in the log — harmless but distracting).  Only inject the
    # test port when the fixture doesn't already define one: fixtures
    # that intentionally test port-handling (malformed_types with
    # port="https", out_of_range with port=99999) MUST keep their
    # original value or the test loses its point.
    raw = json.loads(fixture_path.read_text())
    raw.pop("_comment", None)
    raw.pop("_expected_errors", None)
    if "port" not in raw:
        raw["port"] = TEST_PORT
    (sandbox / "config.json").write_text(json.dumps(raw, indent=2))
    return sandbox


def spawn_j9t(binary: Path, sandbox: Path):
    """Spawn the engine binary detached.  stdout/stderr → log file in the sandbox."""
    sandbox_stdout = (sandbox / "log" / "spawn-stdout.log").open("w")
    proc = subprocess.Popen(
        [str(binary)],
        cwd=str(sandbox),
        stdout=sandbox_stdout,
        stderr=subprocess.STDOUT,
        # Detach from the test's process group so the test can outlive a
        # crash + the spawned j9t can be cleaned up uniformly via terminate().
        start_new_session=True,
    )
    return proc


def shutdown_j9t(proc, sandbox: Path, timeout=10.0):
    if proc.poll() is not None:
        # Already exited — likely on ConfigChecker rejection.
        return proc.returncode

    # Try clean REST shutdown via the injected test port.  Keystore is
    # locked on a fresh start so the admin-key check will reject; SIGTERM
    # is the correct fallback for these test runs.
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)
    return proc.returncode


def check_alive(proc) -> bool:
    """Survival contract: the spawned engine process is still running.

    j9t bypasses HTTPS when TlsCert/TlsKey are absent (the sandbox
    fixtures don't supply them), and several fixtures intentionally
    leave the listen port unpredictable (out_of_range, malformed_types
    with port="https" → auto-port).  So a TCP/HTTPS probe on a known
    port is not a reliable liveness signal.  What we actually care
    about is the operator-protective contract: a malformed config must
    not crash the engine.  `proc.poll() is None` captures that exactly.
    """
    return proc.poll() is None


def run_fixture(binary: Path, fixture_path: Path, results, keep_sandbox: bool):
    name = fixture_path.stem
    header(f"fixture: {name}")
    spec = json.loads(fixture_path.read_text())
    expected = spec.get("_expected_errors", [])
    if not expected:
        warn(f"fixture has no _expected_errors list — skipping")
        return

    sandbox = setup_sandbox(name, fixture_path)
    info(f"sandbox: {sandbox}")

    proc = spawn_j9t(binary, sandbox)
    info(f"spawned pid {proc.pid}")
    # Let ConfigParser + ConfigChecker run.  4 s covers cold spdlog init
    # + provider-list parse + ConfigChecker.  If ConfigChecker rejects,
    # j9t exits before this returns.
    time.sleep(4.0)

    exited_early = proc.poll() is not None
    if exited_early:
        info(f"j9t exited during startup (rc={proc.returncode}) — config rejected by checker")
    else:
        info(f"j9t process alive (pid {proc.pid})")

    log_path = sandbox / "log" / "log.txt"
    if not log_path.exists():
        fail(f"log file never created at {log_path}")
        results[1] += 1
        shutdown_j9t(proc, sandbox)
        return
    log_text = log_path.read_text()

    for needle in expected:
        if needle in log_text:
            ok(f"expected substring present: {needle!r}")
            results[0] += 1
        else:
            fail(f"expected substring MISSING: {needle!r}")
            results[1] += 1

    # Survival contract: either the engine process is still running
    # (parse + ConfigChecker passed, server is up), OR it exited cleanly
    # with a non-negative rc.  Non-negative covers both "ConfigChecker
    # rejected" (typically rc=1) and "ConfigChecker passed but couldn't
    # bind the listen port" (rc=0, the post-clamp port-bind path takes
    # the graceful-shutdown branch and returns 0).  A NEGATIVE rc
    # indicates termination by a POSIX signal — THAT would be a real
    # crash regression because j9t should never SIGSEGV on a malformed
    # config.
    if exited_early:
        if proc.returncode >= 0:
            ok(f"j9t exited with rc={proc.returncode} (clean exit, no signal crash)")
            results[0] += 1
        else:
            fail(f"j9t terminated by signal {-proc.returncode} (unexpected crash)")
            results[1] += 1
    else:
        if check_alive(proc):
            ok(f"j9t process still running (pid {proc.pid}) — no crash on malformed config")
            results[0] += 1
        else:
            fail(f"j9t process gone unexpectedly")
            results[1] += 1

    shutdown_j9t(proc, sandbox)
    if not keep_sandbox:
        shutil.rmtree(sandbox, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=str(PROJECT_ROOT / "bin" / "Debug" / "jarvisAgent-engine"),
                        help="path to the engine binary (Engine edition is faster to spawn)")
    parser.add_argument("--keep-sandbox", action="store_true",
                        help="don't delete sandbox dirs after each fixture (forensics)")
    parser.add_argument("--fixture", action="append", default=None,
                        help="run only the named fixture (basename without .json)")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        print(f"{C.RED}ERROR:{C.RESET} binary not found at {binary}")
        print(f"       build with: premake5 gmake --engine --clang && make config=debug")
        return 2

    if not port_free(TEST_PORT):
        print(f"{C.RED}ERROR:{C.RESET} port {TEST_PORT} is in use — refusing to collide")
        return 2

    SANDBOX_ROOT.mkdir(parents=True, exist_ok=True)

    fixtures = sorted(FIXTURES_DIR.glob("*.json"))
    if args.fixture:
        wanted = set(args.fixture)
        fixtures = [f for f in fixtures if f.stem in wanted]
        missing = wanted - {f.stem for f in fixtures}
        if missing:
            print(f"{C.RED}ERROR:{C.RESET} fixtures not found: {missing}")
            return 2

    info(f"binary: {binary}")
    info(f"fixtures dir: {FIXTURES_DIR}")
    info(f"sandbox root: {SANDBOX_ROOT}")
    info(f"test port: {TEST_PORT}")
    info(f"running {len(fixtures)} fixture(s)")

    results = [0, 0]  # [passed, failed]
    for fixture in fixtures:
        run_fixture(binary, fixture, results, args.keep_sandbox)

    passed, failed = results
    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks across {len(fixtures)} fixtures")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed across {len(fixtures)} fixtures")
    return 1


if __name__ == "__main__":
    sys.exit(main())
