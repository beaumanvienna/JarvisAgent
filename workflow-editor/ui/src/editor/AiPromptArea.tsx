import React, { useCallback, useEffect, useRef, useState } from "react";
import type { JcwfFile } from "../jcwf/types";

export type AiPromptAreaProps = {
  getCurrentJcwf: () => JcwfFile | null;
  onJcwfGenerated: (jcwf: JcwfFile) => void;
  webSocketRef: React.RefObject<WebSocket | null>;
  isWebSocketConnected: boolean;
};

type AiStatus = "idle" | "explaining" | "generating" | "error";

type ProgressInfo = {
  stage?: number;
  totalStages?: number;
  message: string;
};

export default function AiPromptArea(props: AiPromptAreaProps): React.ReactElement
{
  const { getCurrentJcwf, onJcwfGenerated, webSocketRef, isWebSocketConnected } = props;

  const [promptText, setPromptText] = useState<string>("");
  const [status, setStatus] = useState<AiStatus>("idle");
  const [progress, setProgress] = useState<ProgressInfo | null>(null);
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [collapsed, setCollapsed] = useState<boolean>(false);
  const textareaRef = useRef<HTMLTextAreaElement>(null);

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
    setProgress({ stage: 1, totalStages: 4, message: "Starting generation pipeline..." });
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
    <div className="aiPromptArea">
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
          disabled={isBusy || !isWebSocketConnected}
          title="Generate a natural language summary of the current workflow"
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
    </div>
  );
}
