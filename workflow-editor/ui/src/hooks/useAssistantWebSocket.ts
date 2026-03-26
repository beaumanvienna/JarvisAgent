import { useCallback, useEffect, useRef, useState } from "react";

export interface AssistantTurn {
  role: "user" | "assistant" | "tool";
  text: string;
  ts?: string;
  toolName?: string;
  toolOk?: boolean;
}

export interface AssistantState {
  connected: boolean;
  sessionId: string | null;
  turns: AssistantTurn[];
  thinking: boolean;
}

export interface AssistantActions {
  sendMessage: (text: string) => void;
  newSession: () => void;
  resumeSession: (sessionId: string) => void;
  listSessions: () => void;
}

type MessageHandler = (msg: Record<string, unknown>) => void;

function buildWsUrl(path: string): string {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}${path}`;
}

export function useAssistantWebSocket(): [AssistantState, AssistantActions] {
  const [state, setState] = useState<AssistantState>({
    connected: false,
    sessionId: null,
    turns: [],
    thinking: false,
  });

  const wsRef = useRef<WebSocket | null>(null);
  const sessionIdRef = useRef<string | null>(null);

  const processOne: MessageHandler = useCallback((msg) => {
    const type = typeof msg.type === "string" ? msg.type : "";

    if (type === "batch") {
      const msgs = msg.messages;
      if (Array.isArray(msgs)) {
        for (const sub of msgs) {
          if (sub && typeof sub === "object" && !Array.isArray(sub)) {
            processOne(sub as Record<string, unknown>);
          }
        }
      }
      return;
    }

    if (type === "session_active") {
      const sid = typeof msg.sessionId === "string" ? msg.sessionId : null;
      sessionIdRef.current = sid;
      setState((prev) => ({ ...prev, sessionId: sid }));
    } else if (type === "assistant_thinking") {
      setState((prev) => ({ ...prev, thinking: true }));
    } else if (type === "assistant_done") {
      const text = typeof msg.text === "string" ? msg.text : "";
      setState((prev) => ({
        ...prev,
        thinking: false,
        turns: [...prev.turns, { role: "assistant", text }],
      }));
    } else if (type === "error") {
      const message = typeof msg.message === "string" ? msg.message : "Unknown error";
      setState((prev) => ({
        ...prev,
        thinking: false,
        turns: [...prev.turns, { role: "assistant", text: `Error: ${message}` }],
      }));
    } else if (type === "session_history") {
      const rawTurns = msg.turns;
      if (Array.isArray(rawTurns)) {
        const turns: AssistantTurn[] = rawTurns.map((t: any) => ({
          role: t.role === "user" ? "user" : "assistant",
          text: typeof t.text === "string" ? t.text : "",
          ts: typeof t.ts === "string" ? t.ts : undefined,
        }));
        setState((prev) => ({ ...prev, turns }));
      }
    } else if (type === "tool_status") {
      const tool = typeof msg.tool === "string" ? msg.tool : "";
      const status = typeof msg.status === "string" ? msg.status : "";
      const detail = typeof msg.detail === "string" ? msg.detail : "";
      const statusText = detail ? `[${tool}] ${status}: ${detail}` : `[${tool}] ${status}...`;
      setState((prev) => ({
        ...prev,
        turns: [...prev.turns, { role: "tool", text: statusText, toolName: tool }],
      }));
    } else if (type === "tool_result") {
      const tool = typeof msg.tool === "string" ? msg.tool : "";
      const ok = msg.ok === true;
      const summary = typeof msg.summary === "string" ? msg.summary : "";
      const resultText = ok ? `[${tool}] done` : `[${tool}] failed: ${summary}`;
      setState((prev) => ({
        ...prev,
        turns: [...prev.turns, { role: "tool", text: resultText, toolName: tool, toolOk: ok }],
      }));
    } else if (type === "clear") {
      setState((prev) => ({ ...prev, turns: [] }));
    }
  }, []);

  const connect = useCallback(() => {
    if (wsRef.current) return;

    const ws = new WebSocket(buildWsUrl("/ws/assistant"));
    wsRef.current = ws;

    ws.onopen = () => {
      setState((prev) => ({ ...prev, connected: true }));
      // Send periodic pings to drain pending messages from the server.
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
      setState((prev) => ({ ...prev, connected: false, thinking: false }));
      setTimeout(connect, 2000);
    };

    ws.onerror = () => {
      ws.close();
    };

    ws.onmessage = (event) => {
      try {
        processOne(JSON.parse(event.data) as Record<string, unknown>);
      } catch {
        // ignore parse errors
      }
    };
  }, [processOne]);

  useEffect(() => {
    connect();
    return () => {
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
      }
    };
  }, [connect]);

  const sendMessage = useCallback((text: string) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) return;

    // Add user turn immediately for responsiveness.
    setState((prev) => ({
      ...prev,
      turns: [...prev.turns, { role: "user", text }],
    }));

    wsRef.current.send(
      JSON.stringify({
        type: "user_message",
        sessionId: sessionIdRef.current ?? "",
        text,
      })
    );
  }, []);

  const newSession = useCallback(() => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) return;
    setState((prev) => ({ ...prev, turns: [], sessionId: null }));
    wsRef.current.send(JSON.stringify({ type: "new_session" }));
  }, []);

  const resumeSession = useCallback((sessionId: string) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) return;
    wsRef.current.send(JSON.stringify({ type: "resume_session", sessionId }));
  }, []);

  const listSessions = useCallback(() => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) return;
    wsRef.current.send(JSON.stringify({ type: "list_sessions" }));
  }, []);

  const actions: AssistantActions = { sendMessage, newSession, resumeSession, listSessions };

  return [state, actions];
}
