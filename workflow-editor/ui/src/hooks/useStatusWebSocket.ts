import { useCallback, useEffect, useRef, useState } from "react";

export interface StatusState {
  connected: boolean;
  anyInflight: boolean;
  anyRunning: boolean;
  pythonRunning: boolean;
  totalCompleted: number;
  totalFailed: number;
}

function buildWsUrl(path: string): string
{
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}${path}`;
}

export function useStatusWebSocket(): StatusState
{
  const [state, setState] = useState<StatusState>({
    connected: false,
    anyInflight: false,
    anyRunning: false,
    pythonRunning: true,
    totalCompleted: 0,
    totalFailed: 0,
  });

  const wsRef = useRef<WebSocket | null>(null);
  const sessionsRef = useRef<Map<string, number>>(new Map());

  const connect = useCallback(() => {
    if (wsRef.current) return;

    const ws = new WebSocket(buildWsUrl("/ws"));
    wsRef.current = ws;

    ws.onopen = () => {
      setState((prev) => ({ ...prev, connected: true }));
      const pingId = window.setInterval(() => {
        if (ws.readyState === WebSocket.OPEN)
        {
          ws.send(JSON.stringify({ type: "ping" }));
        }
      }, 500);
      (ws as any)._pingId = pingId;
    };

    ws.onclose = () => {
      if ((ws as any)._pingId) clearInterval((ws as any)._pingId);
      wsRef.current = null;
      setState((prev) => ({ ...prev, connected: false }));
      setTimeout(connect, 2000);
    };

    ws.onerror = () => { ws.close(); };

    const processOne = (msg: Record<string, unknown>) => {
      const type = typeof msg.type === "string" ? msg.type : "";

      if (type === "batch")
      {
        const msgs = msg.messages;
        if (Array.isArray(msgs))
        {
          for (const sub of msgs)
          {
            if (sub && typeof sub === "object" && !Array.isArray(sub))
            {
              processOne(sub as Record<string, unknown>);
            }
          }
        }
        return;
      }

      if (type === "workflowRunsSnapshot")
      {
        const runs = msg.runs;
        if (Array.isArray(runs))
        {
          const anyRunning = runs.some((r: any) => r.state === "running");
          setState((prev) => ({ ...prev, anyRunning }));
        }
      }
      else if (type === "workflowRunsLastSnapshot")
      {
        setState((prev) => ({
          ...prev,
          totalCompleted: typeof msg.totalCompleted === "number" ? msg.totalCompleted : prev.totalCompleted,
          totalFailed: typeof msg.totalFailed === "number" ? msg.totalFailed : prev.totalFailed,
        }));
      }
      else if (type === "status")
      {
        const name = typeof msg.name === "string" ? msg.name : "";
        const inflight = typeof msg.inflight === "number" ? msg.inflight : 0;
        if (name)
        {
          sessionsRef.current.set(name, inflight);
          const anyInflight = Array.from(sessionsRef.current.values()).some((v) => v > 0);
          setState((prev) => ({ ...prev, anyInflight }));
        }
      }
      else if (type === "python-status")
      {
        setState((prev) => ({ ...prev, pythonRunning: msg.running === true }));
      }
    };

    ws.onmessage = (event) => {
      try
      {
        processOne(JSON.parse(event.data) as Record<string, unknown>);
      }
      catch
      {
        // ignore
      }
    };
  }, []);

  useEffect(() => {
    connect();
    return () => {
      if (wsRef.current)
      {
        wsRef.current.close();
        wsRef.current = null;
      }
    };
  }, [connect]);

  return state;
}
