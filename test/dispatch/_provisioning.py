"""
Shared AI-interface provisioning for the dispatch test suite.

The keystore refactor put a master-password re-auth gate on AI-interface
create / update / delete: every provisioning POST/DELETE must now carry the
master password in the request body or it returns 401 `reauth_required` at
setup.  This module is the single home for that injection — build the interface
body however a test needs, then route the HTTP through `create_interface` /
`delete_interface`, which merge the master password in.

`MASTER_PASSWORD` defaults to $JARVIS_MASTER_PASSWORD at import, so every test
that provisions an interface — directly or via the sibling helper modules
(`_tier_b_helpers`, `_stress_tui_helpers`, `_per_api_fault_helpers`) — inherits
the gate fix with no argparse change.

The provider (`/api/settings/providers`) and connection-`/test` routes are NOT
re-auth-gated, so they don't go through here.

NOT a test runner — `import _provisioning` from a sibling test_*.py / helper.
"""

from __future__ import annotations

import os

import requests

# Master password for the re-auth gate on AI-interface CRUD.  Defaults to the
# env var JC's dev shell exports so helper-module callers pick it up for free.
MASTER_PASSWORD = os.environ.get("JARVIS_MASTER_PASSWORD", "")


def with_reauth(body: dict | None = None) -> dict:
    """Copy `body` (or start fresh) and merge in `master_password` when one is
    configured.  Use for the body of any re-auth-gated mutation."""
    out = dict(body) if body else {}
    if MASTER_PASSWORD:
        out["master_password"] = MASTER_PASSWORD
    return out


def create_interface(base_url: str, headers: dict, body: dict, *, timeout: int = 10):
    """POST /api/settings/ai-interfaces with the master password merged in."""
    return requests.post(f"{base_url}/api/settings/ai-interfaces",
                         json=with_reauth(body), headers=headers, verify=False, timeout=timeout)


def delete_interface(base_url: str, headers: dict, name: str, *, timeout: int = 10):
    """DELETE /api/settings/ai-interfaces/<name>.  crow reads master_password
    from the DELETE body, so the re-auth password goes there."""
    return requests.delete(f"{base_url}/api/settings/ai-interfaces/{name}",
                           json=with_reauth() or None, headers=headers, verify=False, timeout=timeout)
