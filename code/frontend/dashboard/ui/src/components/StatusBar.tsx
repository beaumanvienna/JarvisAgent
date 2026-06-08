import { useState, useMemo, useRef } from "react";
import type {
  RunSnapshot,
  ConnectionHealthEntry,
  ProviderHealth,
  ProviderErrorCategory,
} from "../types";

type Tab = "dashboard" | "log";

interface Props {
  connected: boolean;
  runs: RunSnapshot[];
  aiCallsInflight: number;
  pythonRunning: boolean;
  mcpConnected: boolean;
  connectionHealth?: ConnectionHealthEntry[];
  providersHealth: ProviderHealth[];
  totalCompleted: number;
  totalFailed: number;
  onQuit: () => void;
  activeTab: Tab;
  onTabChange: (tab: Tab) => void;
  isStudio: boolean;
  onLogout?: () => void;
  authUser?: string | null;
  authRole?: string | null;
  onOpenSettings?: () => void;
}

// Sitting-8 Workstream D: severe categories that flip the AI Health LED red
// for 30s after a fresh failure.  Matches §4-D severity-source-(b).
const RED_CATEGORIES = new Set<ProviderErrorCategory>([
  "BillingExhausted",
  "AuthFailure",
  "ServiceOverload",
  "ModelNotFound",
]);

// Window after an actual 429 during which the interface is shown amber
// ("throttled").  Matches the controller's idle-recovery window — the cap has
// recovered to the ceiling by the time this lapses.  An interface is throttled
// ONLY if its controller saw a real 429 within this window; a cap merely below
// the ceiling (last_429_at_ms == 0) is healthy AIMD ramping, not throttling.
const THROTTLE_RECENT_MS = 60_000;

// Per-interface pin tracking — client-side safety net.  The backend's
// cap_pinned_at_floor_since_ms (Sitting-8 close-out) is preferred when
// non-zero; the client tracker only kicks in for the edge case where the
// backend lags or the dispatcher is unreachable.  Walks the snapshot:
// cap==floor + no tracked pin → set pin = now; cap > floor + tracked pin
// → clear it.  Pure function over (health, prevPins) → next pins.
function updatePinTimes(
  health: ProviderHealth[],
  prevPins: Map<string, number>,
): Map<string, number> {
  const nowMs = Date.now();
  const next = new Map(prevPins);
  const seen = new Set<string>();
  for (const h of health) {
    seen.add(h.interface_name);
    const atFloor = h.current_cap > 0 && h.current_cap <= h.floor_cap;
    if (atFloor && !next.has(h.interface_name)) {
      next.set(h.interface_name, nowMs);
    } else if (!atFloor && next.has(h.interface_name)) {
      next.delete(h.interface_name);
    }
  }
  // Drop pins for interfaces that left the snapshot (config reload, etc.).
  for (const key of Array.from(next.keys())) {
    if (!seen.has(key)) next.delete(key);
  }
  return next;
}

// Resolve the effective pin timestamp for an interface: backend's
// cap_pinned_at_floor_since_ms wins when non-zero (survives cross-refresh);
// fallback to the locally-tracked pin map only for the rare case where the
// backend hasn't recorded one yet.
function effectivePinAt(
  h: ProviderHealth,
  localPins: Map<string, number>,
): number | undefined {
  if (h.cap_pinned_at_floor_since_ms > 0) return h.cap_pinned_at_floor_since_ms;
  return localPins.get(h.interface_name);
}

// Returns one of "green" / "amber" / "red" / "grey" based on §4-D's LED color
// rules.  Reads the locally-tracked pin map for the sustained-floor safety net
// (the backend snapshot field stays at 0 today — frontend computes the pin
// time on each polling tick).
function aiHealthSeverity(
  health: ProviderHealth[],
  pinTimes: Map<string, number>,
): "green" | "amber" | "red" | "grey" {
  if (health.length === 0) return "grey";
  const nowMs = Date.now();
  let anyAmber = false;
  for (const h of health) {
    // Source-(b): recent severe failure.
    if (h.last_error_at_ms > 0
        && nowMs - h.last_error_at_ms < 30_000
        && RED_CATEGORIES.has(h.last_error_category)) {
      return "red";
    }
    // Source-(a): cap pinned at floor for >30s.  Safety net — fires even when
    // classification can't recognize the discriminator (m_Category=Unknown).
    // Prefers the backend's cap_pinned_at_floor_since_ms (survives
    // cross-refresh); falls back to the locally-tracked pin map when the
    // backend hasn't recorded one yet.  Aligned with the source-(b) 30s
    // recent-severe window: dev plan §4-D had this at 60s but the resulting
    // 30-60s amber dip when cap is pinned is misleading (provider is still
    // failing).  Tuned to 30s so the LED stays red continuously.
    const pinAt = effectivePinAt(h, pinTimes);
    if (pinAt !== undefined && nowMs - pinAt > 30_000) {
      return "red";
    }
    // Amber: an ACTUAL 429 within the recent window — the provider is rate-
    // limiting us.  Keyed on the real throttle signal, NOT on cap < max_cap
    // (which is just AIMD ramping below the ceiling and not throttling at all).
    if (h.last_429_at_ms > 0 && nowMs - h.last_429_at_ms < THROTTLE_RECENT_MS) {
      anyAmber = true;
    }
  }
  return anyAmber ? "amber" : "green";
}

function aiHealthLabel(
  health: ProviderHealth[],
  severity: "green" | "amber" | "red" | "grey",
  pinTimes: Map<string, number>,
): string {
  if (severity === "grey") return "AI: no interfaces";
  if (severity === "red") {
    const nowMs = Date.now();
    const redCount = health.filter((h) => {
      const recentSevere =
        h.last_error_at_ms > 0
        && nowMs - h.last_error_at_ms < 30_000
        && RED_CATEGORIES.has(h.last_error_category);
      const pinAt = effectivePinAt(h, pinTimes);
      const sustainedPin = pinAt !== undefined && nowMs - pinAt > 30_000;
      return recentSevere || sustainedPin;
    }).length;
    return `AI: ${redCount} unavailable`;
  }
  if (severity === "amber") {
    const nowMs = Date.now();
    const throttledCount = health.filter(
      (h) => h.last_429_at_ms > 0 && nowMs - h.last_429_at_ms < THROTTLE_RECENT_MS,
    ).length;
    return `AI: ${throttledCount} throttled`;
  }
  return "AI: healthy";
}

function formatRelativeTime(ms: number): string {
  if (ms <= 0) return "never";
  const deltaSec = Math.floor((Date.now() - ms) / 1000);
  if (deltaSec < 5) return "just now";
  if (deltaSec < 60) return `${deltaSec}s ago`;
  if (deltaSec < 3600) return `${Math.floor(deltaSec / 60)}m ago`;
  if (deltaSec < 86400) return `${Math.floor(deltaSec / 3600)}h ago`;
  return `${Math.floor(deltaSec / 86400)}d ago`;
}

function categoryBadgeClass(category: ProviderErrorCategory): string {
  if (RED_CATEGORIES.has(category)) return "cat-badge cat-badge-red";
  if (category === "ThrottleRateLimit") return "cat-badge cat-badge-amber";
  return "cat-badge cat-badge-muted";
}

function AiHealthPopover({ health }: { health: ProviderHealth[] }) {
  // Detect quotaKeys shared by 2+ interfaces — §5.2-(c) "shared cap" footnote.
  const sharedKeys = useMemo(() => {
    const counts = new Map<string, number>();
    for (const h of health) {
      if (!h.quota_key) continue;
      counts.set(h.quota_key, (counts.get(h.quota_key) ?? 0) + 1);
    }
    return new Set(Array.from(counts.entries()).filter(([, n]) => n > 1).map(([k]) => k));
  }, [health]);

  if (health.length === 0) {
    return <div className="ai-health-popover">No AI interfaces configured.</div>;
  }
  return (
    <div className="ai-health-popover">
      <table className="ai-health-table">
        <thead>
          <tr>
            <th>Interface</th>
            <th>Cap</th>
            <th>Last error</th>
            <th>Retry in</th>
          </tr>
        </thead>
        <tbody>
          {health.map((h) => {
            const capCell =
              h.current_cap >= 0 && h.max_cap > 0
                ? `${h.current_cap}/${h.max_cap}`
                : "—";
            const sharedFootnote = sharedKeys.has(h.quota_key);
            return (
              <tr key={h.interface_name}>
                <td className="mono">
                  {h.interface_name}
                  {h.is_mock && <span className="ai-health-mock">(mocked)</span>}
                </td>
                <td className="mono" title={sharedFootnote ? `Shared with other interfaces on ${h.quota_key}` : undefined}>
                  {capCell}
                  {sharedFootnote && <span className="ai-health-shared">*</span>}
                </td>
                <td>
                  {h.last_error_at_ms > 0 ? (
                    <>
                      <span className={categoryBadgeClass(h.last_error_category)}>
                        {h.last_error_category}
                      </span>
                      {h.last_error_code && <code>{h.last_error_code}</code>}
                      <span className="ai-health-when">{formatRelativeTime(h.last_error_at_ms)}</span>
                    </>
                  ) : (
                    <span className="muted">—</span>
                  )}
                </td>
                <td className="mono">
                  {h.retry_after_seconds !== undefined ? `${h.retry_after_seconds}s` : "—"}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      {sharedKeys.size > 0 && (
        <div className="ai-health-footnote">
          * Cap is shared across interfaces routing to the same provider quota key.
        </div>
      )}
    </div>
  );
}

function Led({ color, label }: { color: string; label: string }) {
  return (
    <span className="led-group">
      <span
        className="led"
        style={{
          background: color,
          boxShadow: color === "#334155" ? "none" : `0 0 6px ${color}, 0 0 12px ${color}`,
        }}
      />
      <span className="led-label">{label}</span>
    </span>
  );
}

export default function StatusBar({
  connected,
  runs,
  aiCallsInflight,
  pythonRunning,
  mcpConnected,
  connectionHealth,
  providersHealth,
  totalCompleted,
  totalFailed,
  onQuit,
  activeTab,
  onTabChange,
  isStudio,
  onLogout,
  authUser,
  authRole,
  onOpenSettings,
}: Props) {
  const [showAiHealthPopover, setShowAiHealthPopover] = useState(false);
  // Per-interface pin-tracking state.  Mutated each render via updatePinTimes
  // — useRef avoids spurious re-renders + survives across polling ticks.
  // Mutating refs during render is acceptable here: the value is purely
  // derived from inputs (providersHealth + the previous ref content) and
  // doesn't feed into JSX directly, only into the severity functions.
  const pinTimesRef = useRef<Map<string, number>>(new Map<string, number>());
  pinTimesRef.current = updatePinTimes(providersHealth, pinTimesRef.current);

  const aiHealthSeverityValue = aiHealthSeverity(providersHealth, pinTimesRef.current);
  const aiHealthColor =
    aiHealthSeverityValue === "green" ? "#22c55e"
    : aiHealthSeverityValue === "amber" ? "#eab308"
    : aiHealthSeverityValue === "red" ? "#ef4444"
    : "#334155";
  const aiHealthLabelText = aiHealthLabel(providersHealth, aiHealthSeverityValue, pinTimesRef.current);
  const anyInflight = aiCallsInflight > 0;
  const anyRunning = runs.some(
    (r) => r.state === "running" || r.state === "queued" || r.state === "pending"
  );

  const connectionColor = connected ? "#22c55e" : "#ef4444";
  const inflightColor = anyInflight ? "#eab308" : "#334155";
  const workflowColor = anyRunning ? "#3b82f6" : "#334155";
  const mcpColor = mcpConnected ? "#a855f7" : "#334155";
  const hasOpenCircuit = connectionHealth?.some((c) => c.circuit_state === "open") ?? false;
  const hasHalfOpen = connectionHealth?.some((c) => c.circuit_state === "half_open") ?? false;
  // Only count connections that have actually been proved healthy (Test click
  // or JCWF success).  Merely configured → grey/unknown, not green.
  const confirmedCount = connectionHealth?.filter((c) => c.confirmed_healthy).length ?? 0;
  const cloudHealthColor = hasOpenCircuit
    ? "#ef4444"
    : hasHalfOpen
    ? "#eab308"
    : confirmedCount > 0
    ? "#22c55e"
    : "#334155";
  const cloudHealthLabel = hasOpenCircuit
    ? "Cloud: circuit open"
    : hasHalfOpen
    ? "Cloud: recovering"
    : confirmedCount > 0
    ? `Cloud: ${confirmedCount} healthy`
    : "Cloud: no connections";

  // Show the authenticated identity in both editions — post-§5i, Studio also
  // requires a real MCP login, so the identity always corresponds to a real user.
  const showAuthIdentity = !!authUser;

  return (
    <header className="status-bar">
      <div className="status-bar-left">
        <span className="title">
          JarvisAgent Dashboard
          <span
            style={{
              marginLeft: 8,
              padding: "1px 6px",
              borderRadius: 2,
              background: "#475569",
              color: "#fff",
              fontSize: 10,
              fontWeight: 600,
              letterSpacing: 0.3,
              textTransform: "uppercase",
            }}
            title="j9t edition"
          >
            {isStudio ? "studio" : "engine"}
          </span>
          {showAuthIdentity && (
            <span
              style={{
                marginLeft: 10,
                fontSize: 12,
                fontWeight: 400,
                color: "rgba(255,255,255,0.6)",
              }}
              title="Signed-in identity"
            >
              {authUser}
              {authRole && (
                <span
                  style={{
                    marginLeft: 6,
                    padding: "1px 5px",
                    borderRadius: 2,
                    background: authRole === "admin" ? "#7c3aed" : "#334155",
                    color: "#fff",
                    fontSize: 10,
                  }}
                >
                  {authRole}
                </span>
              )}
            </span>
          )}
        </span>
        <div className="led-row">
          <Led
            color={connectionColor}
            label={connected ? "Connected" : "Disconnected"}
          />
          <Led
            color={inflightColor}
            label={anyInflight ? `AI queries in flight (${aiCallsInflight})` : "No queries"}
          />
          <Led
            color={workflowColor}
            label={anyRunning ? "Workflow running" : "No active runs"}
          />
          <Led
            color={mcpColor}
            label={mcpConnected ? "MCP connected" : "MCP offline"}
          />
          <Led color={cloudHealthColor} label={cloudHealthLabel} />
          {/* Sitting-8 Workstream D: 6th LED — AI Health.  Click toggles a
              popover listing per-interface cap + last error + category badge. */}
          <span
            className="led-group ai-health-led-wrapper"
            onClick={() => setShowAiHealthPopover((v) => !v)}
            style={{ cursor: "pointer", position: "relative" }}
            title="Click for per-interface AIMD cap + last-error detail"
          >
            <span
              className="led"
              style={{
                background: aiHealthColor,
                boxShadow: aiHealthColor === "#334155" ? "none" : `0 0 6px ${aiHealthColor}, 0 0 12px ${aiHealthColor}`,
              }}
            />
            <span className="led-label">{aiHealthLabelText}</span>
            {showAiHealthPopover && (
              <AiHealthPopover health={providersHealth} />
            )}
          </span>
          {!pythonRunning && (
            <span className="python-warning">Python Offline</span>
          )}
          <span className="run-counters">
            <span className="counter-ok">{totalCompleted} succeeded</span>
            {totalFailed > 0 && (
              <span className="counter-fail">{totalFailed} failed</span>
            )}
          </span>
        </div>
      </div>
      <div className="status-bar-right">
        <div className="tab-bar">
          {activeTab !== "dashboard" && (
            <button
              className="tab-btn"
              onClick={() => onTabChange("dashboard")}
            >
              Dashboard
            </button>
          )}
          {activeTab !== "log" && (
            <button
              className="tab-btn"
              onClick={() => onTabChange("log")}
            >
              Log
            </button>
          )}
        </div>
        {onOpenSettings && (
          <button
            className="btn"
            type="button"
            onClick={onOpenSettings}
            title="Settings"
            style={{ fontSize: 18, padding: "4px 10px" }}
          >
            ⚙
          </button>
        )}
        {isStudio && (
          <a href="/editor" className="btn btn-editor">
            Workflow Editor
          </a>
        )}
        <a
          href="https://github.com/beaumanvienna/JarvisAgent"
          className="btn btn-star"
          target="_blank"
          rel="noopener noreferrer"
          title="Star JarvisAgent on GitHub"
        >
          ★ Star
        </a>
        <button className="btn btn-quit" onClick={onQuit}>
          Quit
        </button>
        {onLogout && (
          <button className="btn btn-quit" onClick={onLogout} title="Sign out">
            Logout
          </button>
        )}
      </div>
    </header>
  );
}
