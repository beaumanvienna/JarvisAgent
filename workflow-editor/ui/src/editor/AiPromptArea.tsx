import React, { useCallback, useEffect, useImperativeHandle, useRef, useState, forwardRef } from "react";
import type { JcwfFile } from "../jcwf/types";
import ScriptReviewPanel from "./ScriptReviewPanel";
import type { GeneratedScript } from "./ScriptReviewPanel";

export type AiPromptAreaProps = {
  getCurrentJcwf: () => JcwfFile | null;
  onJcwfGenerated: (jcwf: JcwfFile) => void;
  webSocketRef: React.RefObject<WebSocket | null>;
  isWebSocketConnected: boolean;
};

export type AiPromptAreaHandle = {
  flushPendingScripts: () => Promise<void>;
};

type AiStatus = "idle" | "explaining" | "generating" | "error";

type ProgressInfo = {
  stage?: number;
  totalStages?: number;
  message: string;
};

const AiPromptArea = forwardRef<AiPromptAreaHandle, AiPromptAreaProps>(function AiPromptArea(props, ref)
{
  const { getCurrentJcwf, onJcwfGenerated, webSocketRef, isWebSocketConnected } = props;

  const [promptText, setPromptText] = useState<string>("");
  const [status, setStatus] = useState<AiStatus>("idle");
  const [progress, setProgress] = useState<ProgressInfo | null>(null);
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [collapsed, setCollapsed] = useState<boolean>(false);
  const [pendingScripts, setPendingScripts] = useState<GeneratedScript[] | null>(null);
  const pendingScriptsRef = useRef<GeneratedScript[] | null>(null);
  pendingScriptsRef.current = pendingScripts;
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  useImperativeHandle(ref, () => ({
    flushPendingScripts: (): Promise<void> => {
      const scripts = pendingScriptsRef.current;
      if (!scripts || scripts.length === 0) return Promise.resolve();

      const socket = webSocketRef.current;
      if (!socket || socket.readyState !== WebSocket.OPEN) return Promise.resolve();

      return new Promise<void>((resolve) => {
        const handler = (event: MessageEvent) => {
          let msg: Record<string, unknown>;
          try { msg = JSON.parse(String(event.data)) as Record<string, unknown>; }
          catch { return; }

          if (msg?.type === "batch" && Array.isArray(msg.messages))
          {
            for (const sub of msg.messages as Record<string, unknown>[])
            {
              if (sub?.type === "ai-write-scripts-result") { done(); return; }
            }
            return;
          }
          if (msg?.type === "ai-write-scripts-result") { done(); }
        };

        const done = () => {
          socket.removeEventListener("message", handler);
          setPendingScripts(null);
          resolve();
        };

        socket.addEventListener("message", handler);
        socket.send(JSON.stringify({
          type: "ai-write-scripts",
          scripts: scripts.map((s) => ({ path: s.path, content: s.content, executable: s.executable })),
        }));
      });
    },
  }), [webSocketRef]);

  // Listen for AI-related WebSocket messages.
  useEffect(() => {
    const socket = webSocketRef.current;
    if (!socket) return;

    const handler = (event: MessageEvent) => {
      let msg: Record<string, unknown>;
      try
      {
        msg = JSON.parse(String(event.data)) as Record<string, unknown>;
      }
      catch
      {
        return;
      }

      if (!msg || typeof msg !== "object" || Array.isArray(msg)) return;

      const msgType = typeof msg.type === "string" ? msg.type : "";

      // Handle batch envelope.
      if (msgType === "batch" && Array.isArray(msg.messages))
      {
        for (const sub of msg.messages as Record<string, unknown>[])
        {
          if (sub && typeof sub === "object" && !Array.isArray(sub))
          {
            handleAiMessage(sub);
          }
        }
        return;
      }

      handleAiMessage(msg);
    };

    function handleAiMessage(msg: Record<string, unknown>)
    {
      const msgType = typeof msg.type === "string" ? msg.type : "";

      if (msgType === "ai-explain-progress")
      {
        setProgress({
          message: typeof msg.message === "string" ? msg.message : "Generating explanation...",
        });
      }
      else if (msgType === "ai-explain-result")
      {
        const ok = msg.ok === true;
        if (ok && typeof msg.summary === "string")
        {
          setPromptText(msg.summary);
          setStatus("idle");
          setProgress(null);
          setErrorMessage(null);
        }
        else
        {
          setStatus("error");
          setErrorMessage(typeof msg.error === "string" ? msg.error : "Explanation failed.");
          setProgress(null);
        }
      }
      else if (msgType === "ai-generate-progress")
      {
        setProgress({
          stage: typeof msg.stage === "number" ? msg.stage : undefined,
          totalStages: typeof msg.totalStages === "number" ? msg.totalStages : undefined,
          message: typeof msg.message === "string" ? msg.message : "Generating...",
        });
      }
      else if (msgType === "ai-generate-result")
      {
        const ok = msg.ok === true;
        if (ok && msg.jcwf && typeof msg.jcwf === "object")
        {
          onJcwfGenerated(msg.jcwf as JcwfFile);
          setStatus("idle");
          setProgress(null);
          setErrorMessage(null);
          const retries = typeof msg.retries === "number" ? msg.retries : 0;
          if (retries > 0)
          {
            setErrorMessage(`Generated with ${retries} validation fix ${retries === 1 ? "retry" : "retries"}.`);
          }

          // Show script review panel if scripts were generated
          if (Array.isArray(msg.scripts) && msg.scripts.length > 0)
          {
            const scripts = (msg.scripts as Record<string, unknown>[]).map((s) => ({
              path: typeof s.path === "string" ? s.path : "",
              content: typeof s.content === "string" ? s.content : "",
              executable: s.executable === true,
            }));
            setPendingScripts(scripts);
          }
        }
        else
        {
          setStatus("error");
          setErrorMessage(typeof msg.error === "string" ? msg.error : "Generation failed.");
          setProgress(null);
        }
      }
    }

    socket.addEventListener("message", handler);
    return () => { socket.removeEventListener("message", handler); };
  }, [webSocketRef, isWebSocketConnected, onJcwfGenerated]);

  const onExplain = useCallback(() => {
    const jcwf = getCurrentJcwf();
    if (!jcwf)
    {
      setErrorMessage("No workflow to explain. Add some tasks first.");
      setStatus("error");
      return;
    }

    const socket = webSocketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN)
    {
      setErrorMessage("WebSocket not connected.");
      setStatus("error");
      return;
    }

    setStatus("explaining");
    setProgress({ message: "Sending workflow to AI..." });
    setErrorMessage(null);

    socket.send(JSON.stringify({
      type: "ai-explain-jcwf",
      jcwf: JSON.stringify(jcwf),
    }));
  }, [getCurrentJcwf, webSocketRef]);

  const onGenerate = useCallback(() => {
    const prompt = promptText.trim();
    if (prompt.length === 0)
    {
      setErrorMessage("Enter a description of the workflow you want to generate.");
      setStatus("error");
      return;
    }

    const socket = webSocketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN)
    {
      setErrorMessage("WebSocket not connected.");
      setStatus("error");
      return;
    }

    setStatus("generating");
    setPendingScripts(null);
    setProgress({ stage: 1, totalStages: 5, message: "Starting generation pipeline..." });
    setErrorMessage(null);

    const currentJcwf = getCurrentJcwf();
    const hasExisting = currentJcwf && Object.keys(currentJcwf.tasks ?? {}).length > 0;
    socket.send(JSON.stringify({
      type: "ai-generate-jcwf",
      prompt,
      currentJcwf: hasExisting ? JSON.stringify(currentJcwf) : "",
    }));
  }, [promptText, getCurrentJcwf, webSocketRef]);

  const isBusy = status === "explaining" || status === "generating";
  const canvasHasTasks = (() => {
    const jcwf = getCurrentJcwf();
    return jcwf !== null && typeof jcwf.tasks === "object" && jcwf.tasks !== null && Object.keys(jcwf.tasks).length > 0;
  })();

  if (collapsed)
  {
    return (
      <div className="aiPromptAreaCollapsed">
        <button
          className="btn aiPromptToggle"
          type="button"
          onClick={() => setCollapsed(false)}
          title="Expand AI Prompt Area"
        >
          AI Prompt
        </button>
      </div>
    );
  }

  return (
    <div className={`aiPromptArea${pendingScripts && pendingScripts.length > 0 ? " hasScriptReview" : ""}`}>
      <div className="aiPromptHeader">
        <span className="aiPromptTitle">AI Prompt</span>
        <div className="aiPromptHeaderRight">
          {!isWebSocketConnected && (
            <span className="aiPromptDisconnected">disconnected</span>
          )}
          <button
            className="btn aiPromptToggle"
            type="button"
            onClick={() => setCollapsed(true)}
            title="Collapse"
          >
            _
          </button>
        </div>
      </div>

      <textarea
        ref={textareaRef}
        className="input aiPromptTextarea"
        value={promptText}
        onChange={(e) => { setPromptText(e.target.value); setErrorMessage(null); setStatus("idle"); }}
        placeholder="Describe a workflow in natural language, or click Explain to describe the current workflow..."
        rows={4}
        disabled={isBusy}
      />

      <div className="aiPromptButtons">
        <button
          className="btn aiPromptBtn"
          type="button"
          onClick={onExplain}
          disabled={isBusy || !isWebSocketConnected || !canvasHasTasks}
          title={!canvasHasTasks ? "Add tasks to the canvas first" : "Generate a natural language summary of the current workflow"}
        >
          {status === "explaining" ? "Explaining..." : "Explain"}
        </button>
        <button
          className="btn aiPromptBtn aiPromptBtnPrimary"
          type="button"
          onClick={onGenerate}
          disabled={isBusy || !isWebSocketConnected || promptText.trim().length === 0}
          title="Generate a JCWF workflow from the prompt description"
        >
          {status === "generating" ? "Generating..." : "Generate"}
        </button>
      </div>

      {isBusy && progress && (
        <div className="aiPromptProgress">
          {progress.stage && progress.totalStages
            ? <span className="aiPromptStage">Stage {progress.stage}/{progress.totalStages}</span>
            : null}
          <span>{progress.message}</span>
        </div>
      )}

      {errorMessage && (
        <div className={status === "error" ? "aiPromptError" : "aiPromptInfo"}>
          {errorMessage}
        </div>
      )}

      {pendingScripts && pendingScripts.length > 0 && (
        <ScriptReviewPanel
          scripts={pendingScripts}
          webSocketRef={webSocketRef}
          onDone={() => setPendingScripts(null)}
        />
      )}
    </div>
  );
});

export default AiPromptArea;
