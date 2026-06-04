import { useEffect, useRef, useState, useCallback } from "react";
import type {
  RunSnapshot,
  LastRunInfo,
  ProviderAlertEntry,
  ProviderErrorCategory,
} from "../types";

// Categories that warrant a dashboard banner.  ThrottleRateLimit + Unknown +
// InvalidRequest are deliberately excluded: AIMD handles throttling silently
// (Workstream D / Sitting 8 surfaces the cap drop via the AI Health LED),
// Unknown is too vague to show in a banner copy template, and InvalidRequest
// indicates a caller bug rather than provider state — workflow author sees
// it in the task output already.
const BANNER_CATEGORIES = new Set<ProviderErrorCategory>([
  "BillingExhausted",
  "AuthFailure",
  "ServiceOverload",
  "ModelNotFound",
]);

interface WebSocketState {
  connected: boolean;
  runs: RunSnapshot[];
  lastRuns: LastRunInfo[];
  totalCompleted: number;
  totalFailed: number;
  pythonRunning: boolean;
  // Active per-(interface_name, category) alerts.  Map key shape:
  // `${interfaceName}|${category}`.  Banners dismissed via X are removed
  // from the map; banners auto-clear on the next ai-call-completed from the
  // same interface.  See ProviderAlertBanner in WorkflowsPanel.
  providerAlerts: Map<string, ProviderAlertEntry>;
}

function alertKey(interfaceName: string, category: ProviderErrorCategory): string {
  return `${interfaceName}|${category}`;
}

export function useWebSocket() {
  const [state, setState] = useState<WebSocketState>({
    connected: false,
    runs: [],
    lastRuns: [],
    totalCompleted: 0,
    totalFailed: 0,
    pythonRunning: true,
    providerAlerts: new Map<string, ProviderAlertEntry>(),
  });

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<number | null>(null);
  const logCallbackRef = useRef<((lines: string[]) => void) | null>(null);
  // Reconnect backoff: doubles on each consecutive failed connect, capped at
  // 30s.  Prevents the dashboard from flooding /ws with reconnect attempts
  // before the user has logged in (each attempt emits a [security]
  // ws_upgrade_rejected line on the server).  Reset to base on successful
  // connect.
  const reconnectDelayMs = useRef<number>(2000);
  const kBaseReconnectMs = 2000;
  const kMaxReconnectMs = 30000;

  const registerLogCallback = useCallback((cb: ((lines: string[]) => void) | null) => {
    logCallbackRef.current = cb;
  }, []);

  // Banner X button → drop one (interface, category) entry from the alert map.
  // The dismissal lasts until either the same combo fires another ai-call-failed
  // (banner re-appears with count starting at 1) or the page is refreshed (the
  // map is in-memory only — Sitting 8's /api/providers/health endpoint will
  // hydrate cross-refresh).
  const dismissProviderAlert = useCallback((key: string) => {
    setState((prev) => {
      if (!prev.providerAlerts.has(key)) return prev;
      const next = new Map(prev.providerAlerts);
      next.delete(key);
      return { ...prev, providerAlerts: next };
    });
  }, []);

  // Sitting-8 Workstream D close-out: external listener for cap-changed
  // wake signals.  Registered by App.tsx; on receipt, App calls the polling
  // hook's refresh().  This is the only way to share the WS state across
  // hooks without context — keeps useWebSocket from depending on usePolling.
  const capChangedCallbackRef = useRef<(() => void) | null>(null);
  const registerCapChangedCallback = useCallback((cb: (() => void) | null) => {
    capChangedCallbackRef.current = cb;
  }, []);

  const connectRef = useRef<(() => void) | null>(null);

  // Called by App.tsx after a successful login.  Without this, the user sees a
  // "Disconnected" LED for up to 30s post-login: pre-auth WS upgrade attempts
  // bump the backoff (2 → 4 → 8 → 16 → 30s), and the next attempt is still
  // scheduled at the grown delay even though the cookie is now valid.  Force a
  // reset + immediate retry so the LED flips to green within a frame.
  const forceReconnect = useCallback(() => {
    if (reconnectTimer.current !== null) {
      clearTimeout(reconnectTimer.current);
      reconnectTimer.current = null;
    }
    reconnectDelayMs.current = kBaseReconnectMs;
    if (!wsRef.current) {
      connectRef.current?.();
    }
  }, []);

  const connect = useCallback(() => {
    if (wsRef.current) return;

    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);
    wsRef.current = ws;

    ws.onopen = () => {
      setState((prev) => ({ ...prev, connected: true }));
      // Successful connect — reset the reconnect backoff to base.
      reconnectDelayMs.current = kBaseReconnectMs;
      // Engine validates the session cookie at the WebSocket upgrade handshake
      // (.onaccept); by the time we are connected, auth is already done. Studio
      // has no auth. Either way the client does not send an auth message.
      const pingId = window.setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: "ping" }));
        }
      }, 500);
      (ws as any)._pingId = pingId;
    };

    ws.onclose = () => {
      if ((ws as any)._pingId) clearInterval((ws as any)._pingId);
      wsRef.current = null;
      setState((prev) => ({ ...prev, connected: false }));
      reconnectTimer.current = window.setTimeout(connect, reconnectDelayMs.current);
      // Double for next attempt, capped.  A successful onopen resets to base.
      reconnectDelayMs.current = Math.min(reconnectDelayMs.current * 2, kMaxReconnectMs);
    };

    ws.onerror = () => {
      ws.close();
    };

    const processMessage = (msg: any) => {
      if (msg.type === "batch") {
        for (const sub of msg.messages ?? []) {
          processMessage(sub);
        }
        return;
      }
      if (msg.type === "workflowRunsSnapshot") {
        setState((prev) => ({ ...prev, runs: msg.runs ?? [] }));
      } else if (msg.type === "workflowRunsLastSnapshot") {
        setState((prev) => ({
          ...prev,
          lastRuns: msg.runs ?? [],
          totalCompleted: msg.totalCompleted ?? prev.totalCompleted,
          totalFailed: msg.totalFailed ?? prev.totalFailed,
        }));
      } else if (msg.type === "python-status") {
        setState((prev) => ({ ...prev, pythonRunning: msg.running }));
      } else if (msg.type === "log") {
        logCallbackRef.current?.(msg.lines ?? []);
      } else if (msg.type === "ai-call-failed") {
        // Dedup at insert time: one banner per (interface, category) burst.
        // ThrottleRateLimit is AIMD's job — silent here, surfaced via the AI
        // Health LED in Sitting 8.
        const category = msg.category as ProviderErrorCategory;
        const interfaceName = msg.interface_name as string;
        if (!BANNER_CATEGORIES.has(category) || !interfaceName) return;
        const key = alertKey(interfaceName, category);
        const now = Date.now();
        setState((prev) => {
          const next = new Map(prev.providerAlerts);
          const existing = next.get(key);
          next.set(key, {
            interfaceName,
            category,
            count: (existing?.count ?? 0) + 1,
            firstSeenAt: existing?.firstSeenAt ?? now,
            lastSeenAt: now,
            errorCode: (msg.provider_error_code as string) ?? "",
            errorType: (msg.provider_error_type as string) ?? "",
            message: (msg.error_message as string) ?? "",
            retryAfterSeconds:
              typeof msg.retry_after_seconds === "number"
                ? msg.retry_after_seconds
                : undefined,
            httpStatus: (msg.http_status as number) ?? 0,
          });
          return { ...prev, providerAlerts: next };
        });
      } else if (msg.type === "cap-changed") {
        // Sitting-8 Workstream D close-out: payload-free wake signal — refetch
        // /api/providers/health for authoritative state.  The callback is set
        // by App.tsx to call usePolling.refresh, which re-hits every polling
        // endpoint (including /api/providers/health).  Cheap enough not to
        // worry about debouncing — AIMD cap mutations are bounded.
        capChangedCallbackRef.current?.();
      } else if (msg.type === "ai-call-completed") {
        // Auto-dismiss every alert for this interface — a successful call means
        // the provider is healthy again from j9t's point of view.  User-dismissed
        // banners are already gone from the map; this clears the ones still up.
        // Wire field is `interface_name` (matches ai-call-failed) — `interface`
        // on ai-call-started uses a different shape and isn't relevant here.
        const interfaceLabel = (msg.interface_name as string) ?? "";
        if (!interfaceLabel) return;
        setState((prev) => {
          let changed = false;
          const next = new Map(prev.providerAlerts);
          for (const key of Array.from(next.keys())) {
            const [iface] = key.split("|");
            if (iface === interfaceLabel) {
              next.delete(key);
              changed = true;
            }
          }
          return changed ? { ...prev, providerAlerts: next } : prev;
        });
      }
    };

    ws.onmessage = (event) => {
      try {
        processMessage(JSON.parse(event.data));
      } catch {
        // ignore malformed messages
      }
    };
  }, []);

  // Bridge for forceReconnect (declared above connect to keep hook ordering
  // simple; resolve the cycle via a ref).
  connectRef.current = connect;

  useEffect(() => {
    connect();
    return () => {
      if (reconnectTimer.current !== null) {
        clearTimeout(reconnectTimer.current);
      }
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
      }
    };
  }, [connect]);

  return { ...state, registerLogCallback, dismissProviderAlert, registerCapChangedCallback, forceReconnect };
}
