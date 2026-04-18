import { useCallback, useEffect, useState } from "react";
import type { ScriptEntry } from "../api";
import { fetchScripts } from "../api";

type TypeFilter = "all" | "shell" | "python";

/**
 * Settings → Scripts tab.
 *
 * Read-only catalog of the scripts under scripts/ that adhoc JCWFs can
 * reference. The data comes from GET /api/scripts which parses the
 * `@jarvis-script` metadata header each script carries. The panel's job
 * is to (a) make that catalog human-readable, and (b) offer a "Refresh"
 * button for admins who just dropped a new script onto disk — the backend
 * scans on startup, but refresh triggers an on-demand rescan without
 * requiring a j9t restart.
 */
export default function ScriptsPanel() {
  const [entries, setEntries] = useState<ScriptEntry[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [typeFilter, setTypeFilter] = useState<TypeFilter>("all");

  const load = useCallback(async (refresh: boolean) => {
    setLoading(true);
    setError(null);
    try {
      const resp = await fetchScripts({
        type: typeFilter === "all" ? undefined : typeFilter,
        refresh,
      });
      if (resp.ok) {
        setEntries(resp.scripts ?? []);
      } else {
        setError(resp.error || "Failed to load scripts");
        setEntries([]);
      }
    } catch {
      setError("Network error loading scripts");
      setEntries([]);
    } finally {
      setLoading(false);
    }
  }, [typeFilter]);

  useEffect(() => {
    load(false);
  }, [load]);

  const cell: React.CSSProperties = {
    padding: "6px 10px",
    borderBottom: "1px solid rgba(255,255,255,0.08)",
    fontSize: 12,
    color: "#e8eef5",
    verticalAlign: "top",
  };
  const headCell: React.CSSProperties = {
    ...cell,
    fontWeight: 600,
    color: "rgba(255,255,255,0.65)",
    background: "#181b20",
    position: "sticky",
    top: 0,
    fontSize: 12,
  };
  const badge = (color: string): React.CSSProperties => ({
    display: "inline-block",
    padding: "1px 6px",
    borderRadius: 3,
    fontSize: 10,
    background: color,
    color: "#fff",
    marginRight: 4,
  });

  return (
    <section
      style={{
        padding: 16,
        display: "flex",
        flexDirection: "column",
        gap: 12,
        background: "#141619",
        borderRadius: 6,
        border: "1px solid rgba(255,255,255,0.08)",
      }}
    >
      <header
        style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: 10 }}
      >
        <div>
          <h2 style={{ margin: 0, fontSize: 16, color: "#e8eef5" }}>Scripts</h2>
          <p style={{ margin: "4px 0 0", fontSize: 12, color: "rgba(255,255,255,0.5)" }}>
            Scripts under <code>scripts/</code> that adhoc JCWFs and MCP agents can reference.
            Metadata parsed from the <code>@jarvis-script</code> header each script carries.
          </p>
        </div>
        <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
          <select
            value={typeFilter}
            onChange={(e) => setTypeFilter(e.target.value as TypeFilter)}
            style={{
              background: "#2a2a2a",
              color: "#e8eef5",
              border: "1px solid rgba(255,255,255,0.15)",
              borderRadius: 4,
              padding: "6px 10px",
              fontSize: 13,
            }}
          >
            <option value="all">All types</option>
            <option value="shell">Shell</option>
            <option value="python">Python</option>
          </select>
          <button
            type="button"
            onClick={() => load(true)}
            disabled={loading}
            title="Re-scan scripts/ and reload the catalog. Use when you just dropped a new script on disk."
            style={{
              background: loading ? "#333" : "#2563eb",
              color: "#fff",
              border: "none",
              borderRadius: 4,
              padding: "6px 14px",
              fontSize: 13,
              cursor: loading ? "wait" : "pointer",
            }}
          >
            {loading ? "Refreshing…" : "Refresh"}
          </button>
        </div>
      </header>

      {error && (
        <div
          style={{
            padding: 10,
            background: "rgba(239,68,68,0.12)",
            border: "1px solid rgba(239,68,68,0.4)",
            color: "#ff6b6b",
            fontSize: 13,
            borderRadius: 4,
          }}
        >
          {error}
        </div>
      )}

      <div style={{ fontSize: 12, color: "rgba(255,255,255,0.55)" }}>
        {entries === null
          ? "Loading…"
          : `${entries.length} script${entries.length === 1 ? "" : "s"} indexed.`}
      </div>

      <div
        style={{
          maxHeight: "56vh",
          overflowY: "auto",
          border: "1px solid rgba(255,255,255,0.08)",
          borderRadius: 4,
        }}
      >
        <table style={{ width: "100%", borderCollapse: "collapse" }}>
          <thead>
            <tr>
              <th style={headCell}>Script</th>
              <th style={{ ...headCell, width: 80 }}>Type</th>
              <th style={headCell}>Description</th>
              <th style={{ ...headCell, width: 180 }}>Params</th>
              <th style={{ ...headCell, width: 140 }}>Flags</th>
            </tr>
          </thead>
          <tbody>
            {entries !== null && entries.length === 0 && (
              <tr>
                <td colSpan={5} style={{ ...cell, color: "rgba(255,255,255,0.5)" }}>
                  No scripts indexed{typeFilter !== "all" ? ` for type '${typeFilter}'` : ""}.
                </td>
              </tr>
            )}
            {entries?.map((e) => {
              const ident = e.type === "python" && e.module ? e.module : e.path;
              const subIdent = e.type === "python" && e.module ? e.path : null;
              const flags: React.ReactNode[] = [];
              if (e.has_jarvis_marker === false)
                flags.push(
                  <span key="nj" style={badge("#b45309")}>
                    no @jarvis
                  </span>
                );
              if (e.executable === false && e.type === "shell")
                flags.push(
                  <span key="ne" style={badge("#7c3aed")}>
                    not exec
                  </span>
                );
              return (
                <tr key={e.path}>
                  <td style={{ ...cell, fontFamily: "monospace", fontSize: 11 }}>
                    <div>{ident}</div>
                    {subIdent && (
                      <div style={{ color: "rgba(255,255,255,0.45)", fontSize: 10 }}>
                        {subIdent}
                      </div>
                    )}
                  </td>
                  <td style={cell}>
                    <span
                      style={{
                        padding: "2px 6px",
                        borderRadius: 3,
                        fontSize: 11,
                        background: e.type === "python" ? "#2563eb" : "#16a34a",
                        color: "#fff",
                      }}
                    >
                      {e.type}
                    </span>
                  </td>
                  <td style={cell}>
                    <div>{e.short || <span style={{ color: "rgba(255,255,255,0.4)" }}>—</span>}</div>
                    {e.outputs && (
                      <div style={{ marginTop: 4, fontSize: 11, color: "rgba(255,255,255,0.55)" }}>
                        <strong style={{ fontWeight: 500 }}>outputs:</strong> {e.outputs}
                      </div>
                    )}
                  </td>
                  <td style={{ ...cell, fontFamily: "monospace", fontSize: 11 }}>
                    {e.params && e.params.length > 0 ? (
                      e.params.join(" ")
                    ) : (
                      <span style={{ color: "rgba(255,255,255,0.4)" }}>—</span>
                    )}
                  </td>
                  <td style={cell}>
                    {flags.length > 0 ? flags : <span style={{ color: "rgba(255,255,255,0.4)" }}>—</span>}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </section>
  );
}
