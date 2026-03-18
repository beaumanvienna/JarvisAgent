import React, { useState } from "react";

export type GeneratedScript = {
  path: string;
  content: string;
  executable: boolean;
};

export type ScriptReviewPanelProps = {
  scripts: GeneratedScript[];
  webSocketRef: React.RefObject<WebSocket | null>;
  onDone: () => void;
};

export default function ScriptReviewPanel(props: ScriptReviewPanelProps): React.ReactElement
{
  const { scripts, webSocketRef, onDone } = props;
  const [editedScripts, setEditedScripts] = useState<GeneratedScript[]>(() =>
    scripts.map((s) => ({ ...s }))
  );
  const [expandedIndex, setExpandedIndex] = useState<number | null>(0);
  const [writing, setWriting] = useState(false);
  const [writeResult, setWriteResult] = useState<string | null>(null);

  const updateContent = (index: number, newContent: string) => {
    setEditedScripts((prev) => {
      const next = [...prev];
      next[index] = { ...next[index], content: newContent };
      return next;
    });
  };

  const onAcceptAll = () => {
    const socket = webSocketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN)
    {
      setWriteResult("WebSocket not connected.");
      return;
    }

    setWriting(true);
    setWriteResult(null);

    // Listen for the write result
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

      // Handle batch envelope
      if (msg?.type === "batch" && Array.isArray(msg.messages))
      {
        for (const sub of msg.messages as Record<string, unknown>[])
        {
          if (sub?.type === "ai-write-scripts-result")
          {
            handleResult(sub);
            return;
          }
        }
        return;
      }

      if (msg?.type === "ai-write-scripts-result")
      {
        handleResult(msg);
      }
    };

    const handleResult = (msg: Record<string, unknown>) => {
      socket.removeEventListener("message", handler);
      setWriting(false);

      const written = Array.isArray(msg.written) ? msg.written as string[] : [];
      const errors = Array.isArray(msg.errors) ? msg.errors as Record<string, unknown>[] : [];

      if (errors.length === 0)
      {
        setWriteResult(`Wrote ${written.length} script${written.length !== 1 ? "s" : ""} to disk.`);
        setTimeout(onDone, 1500);
      }
      else
      {
        const errMsgs = errors.map((e) => `${e.path}: ${e.error}`).join("; ");
        setWriteResult(`Wrote ${written.length}, ${errors.length} error(s): ${errMsgs}`);
      }
    };

    socket.addEventListener("message", handler);

    socket.send(JSON.stringify({
      type: "ai-write-scripts",
      scripts: editedScripts.map((s) => ({
        path: s.path,
        content: s.content,
        executable: s.executable,
      })),
    }));
  };

  const onSkip = () => {
    onDone();
  };

  return (
    <div className="scriptReviewPanel">
      <div className="scriptReviewHeader">
        <span className="scriptReviewTitle">
          Generated Scripts ({editedScripts.length})
        </span>
      </div>

      <div className="scriptReviewList">
        {editedScripts.map((script, i) => (
          <div key={script.path} className="scriptReviewItem">
            <button
              className="scriptReviewItemHeader"
              type="button"
              onClick={() => setExpandedIndex(expandedIndex === i ? null : i)}
            >
              <span className="scriptReviewPath">{script.path}</span>
              <span className="scriptReviewBadge">
                {script.executable ? "executable" : "script"}
              </span>
              <span className="scriptReviewToggle">
                {expandedIndex === i ? "▾" : "▸"}
              </span>
            </button>

            {expandedIndex === i && (
              <textarea
                className="scriptReviewCode"
                value={script.content}
                onChange={(e) => updateContent(i, e.target.value)}
                spellCheck={false}
                rows={Math.min(20, script.content.split("\n").length + 1)}
              />
            )}
          </div>
        ))}
      </div>

      <div className="scriptReviewActions">
        <button
          className="btn scriptReviewBtn scriptReviewBtnPrimary"
          type="button"
          onClick={onAcceptAll}
          disabled={writing}
        >
          {writing ? "Writing..." : "Accept All"}
        </button>
        <button
          className="btn scriptReviewBtn"
          type="button"
          onClick={onSkip}
          disabled={writing}
        >
          Skip
        </button>
      </div>

      {writeResult && (
        <div className="scriptReviewResult">{writeResult}</div>
      )}
    </div>
  );
}
