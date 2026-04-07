import type { SessionStatus } from "../types";

interface Props {
  sessions: Map<string, SessionStatus>;
}

export default function SessionManagersPanel({ sessions }: Props) {
  const entries = Array.from(sessions.values()).sort((a, b) =>
    b.inflight - a.inflight || b.outputs - a.outputs || a.name.localeCompare(b.name)
  );

  if (entries.length === 0) {
    return (
      <section className="panel">
        <h2>Session Managers</h2>
        <p className="muted">No session managers active.</p>
      </section>
    );
  }

  return (
    <section className="panel">
      <h2>Session Managers</h2>
      <table className="data-table">
        <thead>
          <tr>
            <th>Name</th>
            <th>State</th>
            <th>Outputs</th>
            <th>In Flight</th>
            <th>Completed</th>
            <th>Failed</th>
          </tr>
        </thead>
        <tbody>
          {entries.map((s) => {
            const isFailed = s.state === "Failed";
            const isPartial = s.state === "PartiallyCompleted";
            const errorTooltip =
              (isFailed || isPartial) && s.last_error_code
                ? `Error ${s.last_error_code}: ${s.last_error_message ?? "unknown"}`
                : undefined;
            return (
              <tr key={s.name}>
                <td className="mono">{s.name}</td>
                <td>
                  {isFailed ? (
                    <span className="state-failed" title={errorTooltip}>
                      {s.state}
                    </span>
                  ) : isPartial ? (
                    <span className="state-warning" title={errorTooltip}>
                      {s.state}
                    </span>
                  ) : (
                    s.state
                  )}
                </td>
                <td className="num">{s.outputs}</td>
                <td className="num">
                  {s.inflight > 0 ? (
                    <span className="state-running">{s.inflight}</span>
                  ) : (
                    "0"
                  )}
                </td>
                <td className="num">{s.completed}</td>
                <td className="num">
                  {s.failed > 0 ? (
                    <span className="state-failed" title={errorTooltip}>
                      {s.failed}
                    </span>
                  ) : (
                    "0"
                  )}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </section>
  );
}
