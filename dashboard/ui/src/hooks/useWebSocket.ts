import { useEffect, useRef, useState, useCallback } from "react";
import type { RunSnapshot, LastRunInfo } from "../types";

interface WebSocketState {
  connected: boolean;
  runs: RunSnapshot[];
  lastRuns: LastRunInfo[];
  totalCompleted: number;
  totalFailed: number;
  pythonRunning: boolean;
}

export function useWebSocket() {
  const [state, setState] = useState<WebSocketState>({
    connected: false,
    runs: [],
    lastRuns: [],
    totalCompleted: 0,
    totalFailed: 0,
    pythonRunning: true,
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
