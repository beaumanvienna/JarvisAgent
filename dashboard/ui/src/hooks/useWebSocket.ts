import { useEffect, useRef, useState, useCallback } from "react";
import type { RunSnapshot, SessionStatus, LastRunInfo } from "../types";

interface WebSocketState {
  connected: boolean;
  runs: RunSnapshot[];
  lastRuns: LastRunInfo[];
  totalCompleted: number;
  totalFailed: number;
  sessions: Map<string, SessionStatus>;
  pythonRunning: boolean;
}

export function useWebSocket() {
  const [state, setState] = useState<WebSocketState>({
    connected: false,
    runs: [],
    lastRuns: [],
    totalCompleted: 0,
    totalFailed: 0,
    sessions: new Map(),
    pythonRunning: true,
  });

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<number | null>(null);
  const logCallbackRef = useRef<((lines: string[]) => void) | null>(null);

  const registerLogCallback = useCallback((cb: ((lines: string[]) => void) | null) => {
    logCallbackRef.current = cb;
  }, []);

  const connect = useCallback(() => {
    if (wsRef.current) return;

    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);
    wsRef.current = ws;

    ws.onopen = () => {
      setState((prev) => ({ ...prev, connected: true }));
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
      reconnectTimer.current = window.setTimeout(connect, 2000);
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
      } else if (msg.type === "status") {
        setState((prev) => {
          const next = new Map(prev.sessions);
          next.set(msg.name, {
            name: msg.name,
            state: msg.state,
            outputs: msg.outputs,
            inflight: msg.inflight,
            completed: msg.completed,
            failed: msg.failed ?? 0,
            last_error_code: msg.last_error_code,
            last_error_message: msg.last_error_message,
          });
          return { ...prev, sessions: next };
        });
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

  return { ...state, registerLogCallback };
}
