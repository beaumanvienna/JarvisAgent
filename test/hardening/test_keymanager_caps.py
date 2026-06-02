#!/usr/bin/env python3
"""
KeyManager keystore parse-cap synthetic tests (encrypted-path).

Locks in the two load-time caps that bound OOM / allocation from a hostile or
corrupt keystore, exercised through the ONLY credential path j9t has — the
AES-256-GCM `keys.json.enc`, decrypted at runtime via POST /api/settings/keys/unlock
(there is no plaintext-keystore or env-var credential path in any edition):

  T1  kMaxKeysFileBytes — a keys.json.enc larger than 4 MB is refused by `Load`
      BEFORE decryption (the size check is on the raw blob), logging
      LOG_CORE_ERROR "exceeds kMaxKeysFileBytes"; j9t must not crash.

  T2  kMaxProviders — a VALID encrypted keystore that decrypts to > 1024
      providers is aborted by `ParseProvidersJson` with LOG_CORE_ERROR
      "provider count exceeds kMaxProviders=1024"; j9t must not crash.

T2 crafts a real `keys.json.enc` in Python, reproducing the on-disk format
(`engine/keys/keyEncryption.h`): a 33-byte header `MAGIC "JKEY" | version 0x02 |
16-byte salt | 12-byte IV`, then AES-256-GCM(key, iv, plaintext, aad=header) whose
output is ciphertext||tag — appended after the header.  The key is
PBKDF2-HMAC-SHA256(password, salt, 600000, 32).  NOTE: this couples the test to
that wire format; if `keyEncryption` bumps to V3 or changes the KDF, regenerate
the fixture to match (the test will fail loudly via a decrypt error if it drifts).

Both caps live in the shared `Load` → `ParseProvidersJson` chain reached only via
the encrypted store, so this runs against any j9t binary (studio or engine).

  T3  (optional, needs a running host j9t) — the set-default REST surface fails
      closed for whitespace / unknown provider names.

Usage:
  python3 test/hardening/test_keymanager_caps.py
  python3 test/hardening/test_keymanager_caps.py --binary bin/Debug/jarvisAgent-studio
  python3 test/hardening/test_keymanager_caps.py --admin-key "$J9T_TOKEN"   # adds T3
"""

import argparse
import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
SANDBOX_ROOT = Path("/tmp/claude/j9t-keymgr-sandbox")
# Reserved test port — must NOT collide with the host j9t (8443) or the
# malformed-config harness (8444).
TEST_PORT = 8445

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False


class C:
    RESET = "\033[0m"; BOLD = "\033[1m"
    RED = "\033[91m"; GREEN = "\033[92m"; CYAN = "\033[96m"; YELLOW = "\033[93m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}⚠{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {msg}\n{'─'*70}{C.RESET}")


# ── keys.json.enc fixture builder (mirrors engine/keys/keyEncryption.cpp) ──

KEY_MAGIC = b"JKEY"
KEY_VERSION_V2 = 0x02
KEY_SALT = bytes(range(16))          # fixed salt — stored in the header, so j9t derives the same key
KEY_IV = bytes(range(16, 28))        # fixed 12-byte IV
PBKDF2_ITERS_V2 = 600000


def make_encrypted_keystore(plaintext: bytes, password: str) -> bytes:
    """Encrypt `plaintext` into the j9t keys.json.enc wire format."""
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), KEY_SALT, PBKDF2_ITERS_V2, 32)
    header = KEY_MAGIC + bytes([KEY_VERSION_V2]) + KEY_SALT + KEY_IV
    ct_and_tag = AESGCM(key).encrypt(KEY_IV, plaintext, header)  # ciphertext || 16-byte tag
    return header + ct_and_tag


def keystore_json_with_n_providers(n: int) -> bytes:
    """Decrypted keystore body with `n` providers — enough to trip kMaxProviders.
    The count check fires at the top of the parse loop before each provider object
    is inspected, so minimal `{}` entries are sufficient."""
    providers = {f"p{i}": {} for i in range(n)}
    return json.dumps({"version": 1, "default_provider": "p0", "providers": providers}).encode()


# ── sandbox spawn (mirrors test/config/test_malformed_configs.py) ──

def minimal_valid_config() -> dict:
    return {
        "queue folder": "queue",
        "workflows folder": "workflows",
        "API interfaces": [
            {"name": "canary", "url": "https://api.openai.com/v1/chat/completions",
             "model": "gpt-4", "API": "API1", "key_name": "openai"}
        ],
        "port": TEST_PORT,
    }


def setup_sandbox(name: str, keys_enc_bytes: bytes) -> Path:
    sandbox = SANDBOX_ROOT / name
    if sandbox.exists():
        shutil.rmtree(sandbox)
    sandbox.mkdir(parents=True)
    for d in ("queue", "workflows", "log", "scripts"):
        (sandbox / d).mkdir()
    (sandbox / "config.json").write_text(json.dumps(minimal_valid_config(), indent=2))
    (sandbox / "keys.json.enc").write_bytes(keys_enc_bytes)
    return sandbox


def spawn_j9t(binary: Path, sandbox: Path):
    stdout = (sandbox / "log" / "spawn-stdout.log").open("w")
    return subprocess.Popen([str(binary)], cwd=str(sandbox), stdout=stdout,
                            stderr=subprocess.STDOUT, start_new_session=True)


def shutdown_j9t(proc, timeout=10.0):
    if proc.poll() is not None:
        return proc.returncode
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait(timeout=5.0)
    return proc.returncode


def wait_for_listen(port, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.3)
    return False


def post_unlock(port, password):
    """POST the master password to the sandbox j9t to trigger KeyManager::Load.
    The sandbox has no TLS cert, so j9t serves plain HTTP; tolerate either."""
    body = json.dumps({"master_password": password}).encode()
    for scheme, ctx in (("http", None),
                        ("https", ssl._create_unverified_context())):
        url = f"{scheme}://127.0.0.1:{port}/api/settings/keys/unlock"
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=8, context=ctx) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code  # 4xx/5xx still means the request reached the unlock handler
        except Exception:
            continue
    return None


def run_cap_fixture(binary, name, keys_enc_bytes, password, needle, results, keep_sandbox):
    header(name)
    sandbox = setup_sandbox(name, keys_enc_bytes)
    info(f"sandbox: {sandbox}  (keys.json.enc = {len(keys_enc_bytes)} bytes)")
    proc = spawn_j9t(binary, sandbox)
    info(f"spawned pid {proc.pid}")

    if not wait_for_listen(TEST_PORT, timeout=20):
        # Server may have refused the config or bound elsewhere; still try the log.
        warn(f"port {TEST_PORT} not listening within 20 s — checking log anyway")
    else:
        code = post_unlock(TEST_PORT, password)
        info(f"unlock POST → {code} (a non-200 is expected — Load rejects the keystore)")
    time.sleep(1.5)  # let the LOG_CORE_ERROR flush

    log_path = sandbox / "log" / "log.txt"
    log_text = log_path.read_text() if log_path.exists() else ""
    if not log_text:
        alt = sandbox / "log" / "spawn-stdout.log"
        log_text = alt.read_text() if alt.exists() else ""

    if needle in log_text:
        ok(f"LOG_CORE_ERROR present: {needle!r}")
        results[0] += 1
    else:
        fail(f"expected log substring MISSING: {needle!r}")
        results[1] += 1

    if proc.poll() is not None and proc.returncode < 0:
        fail(f"j9t terminated by signal {-proc.returncode} (crash on bad keystore)")
        results[1] += 1
    else:
        ok("j9t did not crash on the rejected keystore")
        results[0] += 1

    shutdown_j9t(proc)
    if not keep_sandbox:
        shutil.rmtree(sandbox, ignore_errors=True)


def run_set_default_fail_closed(base_url, admin_key, results):
    header("T3. POST /api/settings/providers/<name>/default fails closed")
    try:
        import requests
        import urllib3
        urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
    except ImportError:
        warn("requests not installed — skipping T3"); return
    verify = not base_url.startswith("https://localhost")
    headers = {"Authorization": f"Bearer {admin_key}"}

    def post_default(seg):
        return requests.post(f"{base_url.rstrip('/')}/api/settings/providers/{seg}/default",
                             headers=headers, verify=verify, timeout=10)

    r = post_default("%20")
    if r.status_code in (404, 400):
        ok(f"whitespace provider name → {r.status_code} (fail-closed)"); results[0] += 1
    else:
        fail(f"whitespace provider name → {r.status_code} (expected 404/400)"); results[1] += 1
    r = post_default(f"ghost_{os.getpid()}_nope")
    if r.status_code == 404:
        ok("unknown provider name → 404 not_found"); results[0] += 1
    else:
        fail(f"unknown provider name → {r.status_code} (expected 404)"); results[1] += 1
    info("internal SetDefaultProvider('') WARN guard is defense-in-depth below this gate "
         "(unreachable via external input — REST gates on HasCredential; load assigns "
         "m_DefaultProviderName directly).")


def detect_binary(explicit):
    if explicit:
        p = Path(explicit)
        return p.resolve() if p.exists() else None
    for rel in ("bin/Release/jarvisAgent-studio", "bin/Debug/jarvisAgent-studio",
                "bin/Release/jarvisAgent-engine", "bin/Debug/jarvisAgent-engine"):
        p = PROJECT_ROOT / rel
        if p.exists():
            return p.resolve()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=None, help="j9t binary (default: auto-detect)")
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    parser.add_argument("--admin-key", default=None, help="enable T3 against the running host j9t")
    parser.add_argument("--keep-sandbox", action="store_true")
    args = parser.parse_args()

    results = [0, 0]
    binary = detect_binary(args.binary)
    if binary is None:
        fail("no j9t binary found (build bin/{Release,Debug}/jarvisAgent-*)")
        return 2
    info(f"binary: {binary}")

    if not HAVE_CRYPTO:
        warn("python 'cryptography' not installed — cannot craft the T2 encrypted fixture; "
             "running T1 only (pip install cryptography to enable T2)")

    # T1 — size cap: > 4 MB keys.json.enc, refused pre-decrypt.  Doesn't need valid
    # encryption (the size check precedes decryption), so a 4 MB+ blob suffices.
    oversized = make_encrypted_keystore(b"x", "testpw") + b"\x00" * (4 * 1024 * 1024 + 16)
    run_cap_fixture(binary, "T1_kMaxKeysFileBytes", oversized, "testpw",
                    "exceeds kMaxKeysFileBytes", results, args.keep_sandbox)

    # T2 — provider-count cap: a VALID encrypted keystore that decrypts to 1030 providers.
    if HAVE_CRYPTO:
        body = keystore_json_with_n_providers(1030)
        keystore = make_encrypted_keystore(body, "testpw")
        run_cap_fixture(binary, "T2_kMaxProviders", keystore, "testpw",
                        "provider count exceeds kMaxProviders=1024", results, args.keep_sandbox)

    if args.admin_key:
        run_set_default_fail_closed(args.base_url, args.admin_key, results)
    else:
        info("T3 skipped (no --admin-key); cap tests are self-contained")

    passed, failed = results
    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
