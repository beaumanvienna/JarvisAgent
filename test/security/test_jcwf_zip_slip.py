#!/usr/bin/env python3
"""JCWF container extraction hardening — Zip-Slip / symlink-race / zip-bomb e2e.

Crafts hostile `.jcwf` zip containers, drops them into the running server's
`workflows/` directory, triggers `POST /api/workflows/reload` (the real vector:
`.jcwf` files are portable artifacts, and one shared by a third party is
extracted on reload), and asserts every hostile container is rejected without
writing a byte outside its extraction directory.

Covers `JcwfContainer::Extract` / `ReadFile` (code/backend/application/workflow/
jcwfContainer.cpp):

  Validation-pass rejections (no disk I/O):
    - parent-directory ('..') traversal entry
    - absolute-path entry
    - backslash-traversal entry
    - Unix symlink entry (S_IFLNK external attr)
    - over-count archive (entry count cap)

  Symlink ancestor in the extraction tree:
    - a symlink ancestor that exists when extraction starts must not redirect a
      clean-named entry out of the tree.  We pre-plant
      `<extracted>/sub -> /tmp/claude/<target>` and drop a container whose entry
      is `sub/payload.json`; the payload must never land in the target.  (The
      pre-existing link is caught by the validation pass — weakly_canonical
      resolves it — with the write-time EnsureSafeDirs walk as the backstop for
      a link planted in the race window between validation and the write, which
      a static fixture cannot deterministically trigger.)

  Not exercised here (verified by code inspection): the encrypted-entry guard
  (`stat.m_is_encrypted`) — Python's stdlib zipfile zeroes the general-purpose
  flag bits on write, so a genuine encrypted entry can't be forged with it; and
  the per-entry / total uncompressed-size caps, which would need multi-hundred-MB
  fixtures to trip.

Reload skips a broken container and continues (records it as a BrokenWorkflow),
so the invariant is: the set of *valid* loaded workflows is unchanged by adding
hostile containers, and no escape path is ever created.

Usage:
  python3 test/security/test_jcwf_zip_slip.py --admin-key "$J9T_TOKEN"

Needs a running Studio instance (reload is Studio-only, admin-gated).
"""

import argparse
import os
import shutil
import stat
import sys
import uuid
import zipfile
from pathlib import Path

try:
    import requests
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    print("ERROR: 'requests' package missing (pip install requests)")
    sys.exit(1)


class C:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    CYAN = "\033[96m"
    YELLOW = "\033[93m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'-'*64}\n  {msg}\n{'-'*64}{C.RESET}")


PROJECT_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = PROJECT_ROOT / "workflows"
SCRATCH = Path("/tmp/claude")


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify_ssl = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}",
                            timeout=30, headers=headers, verify=verify_ssl, **kw)


def expect(cond, msg, results):
    if cond:
        ok(msg)
        results[0] += 1
    else:
        fail(msg)
        results[1] += 1
    return cond


def reload_workflows(base, key):
    r = http("POST", base, "/api/workflows/reload", key=key)
    return r


def loaded_ids(base, key):
    r = http("GET", base, "/api/workflows", key=key)
    if r.status_code != 200:
        return None
    return {w.get("id") for w in r.json().get("workflows", [])}


# ----------------------------------------------------------------------------
# Hostile-container crafting.  Each returns the list of on-disk paths it created
# (for cleanup) and the escape path that must never come to exist.
# ----------------------------------------------------------------------------

def write_zip(path, entries):
    """entries: list of (arcname, data, is_symlink)."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, data, is_symlink in entries:
            zi = zipfile.ZipInfo(arcname)
            zi.create_system = 3  # Unix
            zi.external_attr = ((stat.S_IFLNK | 0o777) if is_symlink else 0o644) << 16
            z.writestr(zi, data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    parser.add_argument("--admin-key", default=os.environ.get("J9T_TOKEN"),
                        help="Admin MCP key (Bearer).  Required.")
    args = parser.parse_args()

    if not args.admin_key:
        print("ERROR: --admin-key or $J9T_TOKEN required.")
        return 2

    base, key = args.base_url, args.admin_key
    suffix = uuid.uuid4().hex[:8]
    tag = f"ziptest_{suffix}"

    probe = http("GET", base, "/api/status", key=key)
    if probe.status_code != 200:
        print(f"ERROR: {base}/api/status returned {probe.status_code}.")
        return 2

    SCRATCH.mkdir(parents=True, exist_ok=True)
    results = [0, 0]  # [passed, failed]

    # Baseline: the set of valid workflows before we add anything hostile.
    baseline = loaded_ids(base, key)
    if baseline is None:
        print("ERROR: GET /api/workflows failed.")
        return 2
    info(f"Baseline: {len(baseline)} workflow(s) loaded.")

    created_files = []   # workflows/ paths to remove
    escape_paths = []    # absolute paths that must NOT exist after reload

    # -- Positive control: force a fresh re-extraction of a real multi-file
    # container so the happy-path write code (safe parent creation + atomic
    # write + post-write containment net) actually runs and produces a working
    # extraction.  Re-extraction reproduces the dir from the .jcwf, so removing
    # it first is safe and self-healing.
    bench = WORKFLOWS_DIR / "make-example.jcwf"
    if bench.exists() and "make-example" in baseline:
        header("Positive control: fresh re-extraction of make-example")
        shutil.rmtree(WORKFLOWS_DIR / "make-example", ignore_errors=True)
        r = reload_workflows(base, key)
        expect(r.status_code == 200, f"reload after wiping extract dir → 200 (got {r.status_code})", results)
        ext = WORKFLOWS_DIR / "make-example"
        expect((ext / "global.json").is_file() and (ext / "global.json").stat().st_size > 0,
               "global.json re-extracted (non-empty)", results)
        expect((ext / "make-example.json").is_file(), "canvas JSON re-extracted", results)
        n_files = sum(1 for _ in ext.rglob("*") if _.is_file())
        expect(n_files == 7, f"all 7 entries re-extracted (got {n_files})", results)
        expect("make-example" in (loaded_ids(base, key) or set()),
               "make-example loaded after fresh extraction", results)

    try:
        # -- Hostile container set ------------------------------------------
        esc_trav = SCRATCH / f"{tag}_escape_trav.txt"
        esc_abs = SCRATCH / f"{tag}_escape_abs.txt"
        esc_bs = SCRATCH / f"{tag}_escape_bs.txt"
        sym_target = SCRATCH / f"{tag}_symlink_target"
        for p in (esc_trav, esc_abs, esc_bs, sym_target):
            escape_paths.append(p)

        c_trav = WORKFLOWS_DIR / f"{tag}_trav.jcwf"
        write_zip(c_trav, [("global.json", b"{}", False),
                           ("../../../../tmp/claude/" + esc_trav.name, b"PWNED", False)])

        c_abs = WORKFLOWS_DIR / f"{tag}_abs.jcwf"
        write_zip(c_abs, [("global.json", b"{}", False),
                          (str(esc_abs), b"PWNED", False)])

        c_bs = WORKFLOWS_DIR / f"{tag}_bs.jcwf"
        write_zip(c_bs, [("global.json", b"{}", False),
                         ("..\\..\\..\\..\\tmp\\claude\\" + esc_bs.name, b"PWNED", False)])

        c_sym = WORKFLOWS_DIR / f"{tag}_sym.jcwf"
        write_zip(c_sym, [("global.json", b"{}", False),
                          ("evil.json", str(sym_target).encode(), True)])

        c_count = WORKFLOWS_DIR / f"{tag}_count.jcwf"
        write_zip(c_count, [(f"f{n}.json", b"{}", False) for n in range(8200)])

        for c in (c_trav, c_abs, c_bs, c_sym, c_count):
            created_files.append(c)

        # -- Write-time planted-symlink defence -----------------------------
        # Pre-plant a symlink inside the extraction dir, then drop a benign
        # container whose clean-named entry would write through it.
        planted_stem = f"{tag}_planted"
        planted_dir = WORKFLOWS_DIR / planted_stem
        planted_target = SCRATCH / f"{tag}_planted_target"
        planted_target.mkdir(parents=True, exist_ok=True)
        planted_dir.mkdir(parents=True, exist_ok=True)
        (planted_dir / "sub").symlink_to(planted_target, target_is_directory=True)
        created_files.append(planted_dir)
        escape_paths.append(planted_target / "payload.json")

        c_planted = WORKFLOWS_DIR / f"{planted_stem}.jcwf"
        write_zip(c_planted, [("global.json", b"{}", False),
                              ("sub/payload.json", b"PWNED", False)])
        created_files.append(c_planted)
        # Force staleness so reload re-extracts over the pre-planted dir.
        os.utime(c_planted, (planted_dir.stat().st_atime + 1000,
                             planted_dir.stat().st_mtime + 1000))

        # -- Trigger the real path ------------------------------------------
        header("Reload with hostile containers present")
        r = reload_workflows(base, key)
        expect(r.status_code == 200, f"reload → HTTP 200 (got {r.status_code})", results)

        after = loaded_ids(base, key)
        expect(after is not None, "GET /api/workflows after reload", results)
        after = after or set()

        # -- Assert: no hostile container produced a loaded workflow --------
        header("No hostile container loaded as a workflow")
        hostile_stems = [f"{tag}_trav", f"{tag}_abs", f"{tag}_bs", f"{tag}_sym",
                         f"{tag}_count", planted_stem]
        leaked = sorted(s for s in hostile_stems if s in after)
        expect(not leaked, f"none of the hostile stems loaded (leaked: {leaked})", results)
        expect(baseline.issubset(after),
               "all baseline workflows still loaded (reload skipped only the hostile ones)",
               results)

        # -- Assert: nothing escaped the extraction tree --------------------
        header("No file written outside the extraction directory")
        for p in escape_paths:
            expect(not p.exists(), f"escape path absent: {p}", results)

        # -- Assert: planted symlink was not written through ----------------
        # The benign entry's payload must not have landed in the symlink target.
        expect(not (planted_target / "payload.json").exists(),
               "symlink-ancestor refused (payload not written through the link)", results)
        # Belt-and-suspenders: the extracted dir must not contain an escaping file.
        escaped_in_target = list(planted_target.iterdir())
        expect(not escaped_in_target,
               f"symlink target dir still empty (contents: {escaped_in_target})", results)

    finally:
        # -- Cleanup --------------------------------------------------------
        header("Cleanup")
        for c in created_files:
            try:
                if c.is_dir() and not c.is_symlink():
                    shutil.rmtree(c, ignore_errors=True)
                else:
                    c.unlink(missing_ok=True)
            except OSError as e:
                info(f"cleanup skip {c}: {e}")
            # Remove any extracted dir left by reload for file containers.
            if c.suffix == ".jcwf":
                ext = c.with_suffix("")
                shutil.rmtree(ext, ignore_errors=True)
        for p in escape_paths:
            try:
                if p.is_dir():
                    shutil.rmtree(p, ignore_errors=True)
                else:
                    p.unlink(missing_ok=True)
            except OSError:
                pass
        for extra in (SCRATCH / f"{tag}_symlink_target", SCRATCH / f"{tag}_planted_target"):
            shutil.rmtree(extra, ignore_errors=True)
        # Reload again so the registry forgets the hostile containers.
        reload_workflows(base, key)
        info("Removed hostile containers + extracted dirs; reloaded registry.")

    passed, failed = results
    print(f"\n{C.BOLD}Results: {C.GREEN}{passed} passed{C.RESET}, "
          f"{C.RED if failed else C.GREEN}{failed} failed{C.RESET}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
