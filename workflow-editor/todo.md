# JarvisAgent — Workflow Editor TODO

Scope: frontend (React workflow editor UI in `workflow-editor/ui/`).

**See also:**
- `todo.md` (project root) — global TODO list; default home for new items unless they're genuinely scoped to this area and would clutter the global list.
- `doc/misc/hand-off.md` — session hand-off log; read latest entry first when picking up.
- `doc/misc/workflow-editor-todo.md` — archived history of closed items.

---

### Master-password unlock + MCP login parity with the dashboard
See `todo.md` (project root) "Loose follow-ups" for the full description.  Frontend-only work; backend endpoints already in place.  Lift `dashboard/ui/src/components/MasterPasswordDialog.tsx` and the dashboard's `POST /api/auth/login` flow into a shared location, or duplicate into the editor.  Acceptance: restart j9t → open editor → master-password prompt appears with the same UX as the dashboard.
