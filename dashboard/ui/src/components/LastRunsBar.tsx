import type { LastRunInfo } from "../types";

interface Props {
  lastRuns: LastRunInfo[];
}

function formatAgo(iso: string): string {
  if (!iso) return "";
  const t = new Date(iso).getTime();
  if (!Number.isFinite(t)) return iso;
  const s = Math.max(0, Math.round((Date.now() - t) / 1000));
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.round(s / 60)}m ago`;
  if (s < 86400) return `${Math.round(s / 3600)}h ago`;
  return `${Math.round(s / 86400)}d ago`;
}

function statusGlyph(state: string): string {
  if (state === "succeeded") return "✓";
  if (state === "failed") return "✗";
  if (state === "cancelled") return "⊘";
  if (state === "running" || state === "pending" || state === "queued") return "…";
  return "·";
}

function statusColor(state: string): string {
  if (state === "succeeded") return "#22c55e";
  if (state === "failed") return "#ef4444";
  if (state === "cancelled") return "#64748b";
  return "#eab308";
}

function isAdhoc(run: LastRunInfo): boolean {
  return run.runId.startsWith("adhoc_") || run.workflowId.startsWith("_adhoc_");
}

function displayId(run: LastRunInfo): string {
  if (!isAdhoc(run)) return run.workflowId;
  // For adhoc, collapse the long _adhoc_<ts>_<counter> to "adhoc <counter>".
  const match = run.workflowId.match(/^_adhoc_\d{8}T\d{6}_(\d+)$/);
  return match ? `adhoc #${Number(match[1])}` : run.workflowId;
}

export default function LastRunsBar({ lastRuns }: Props) {
  if (!lastRuns || lastRuns.length === 0) return null;

  // Sort by completedAt descending, take the most recent 3.
  const top = [...lastRuns]
    .sort((a, b) => (a.completedAt < b.completedAt ? 1 : -1))
    .slice(0, 3);

  return (
    <div
      style={{
        display: "flex",
        gap: 12,
        alignItems: "center",
        padding: "6px 14px",
        background: "#141619",
        borderBottom: "1px solid rgba(255,255,255,0.06)",
        fontSize: 12,
        color: "rgba(255,255,255,0.8)",
        flexWrap: "wrap",
      }}
    >
      <span style={{ color: "rgba(255,255,255,0.5)", letterSpacing: "0.04em" }}>Last runs:</span>
      {top.map((r) => {
        const adhoc = isAdhoc(r);
        return (
          <span
            key={r.runId}
            style={{
              display: "inline-flex",
              alignItems: "center",
              gap: 6,
              fontStyle: adhoc ? "italic" : "normal",
            }}
            title={r.runId}
          >
            <span style={{ color: statusColor(r.state), fontWeight: 600 }}>{statusGlyph(r.state)}</span>
            <span>{displayId(r)}</span>
            {adhoc && (
              <span
                style={{
                  fontSize: 10,
                  padding: "1px 5px",
                  borderRadius: 2,
                  background: "#7c3aed",
                  color: "#fff",
                  fontStyle: "normal",
                }}
              >
                adhoc
              </span>
            )}
            <span style={{ color: "rgba(255,255,255,0.5)" }}>{formatAgo(r.completedAt)}</span>
          </span>
        );
      })}
    </div>
  );
}
