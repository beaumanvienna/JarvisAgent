import React, { useEffect, useRef, useState } from "react";

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
  const [acceptedSet, setAcceptedSet] = useState<Set<number>>(() => new Set());
  const [writing, setWriting] = useState(false);
  const [writeResult, setWriteResult] = useState<string | null>(null);
  const panelRef = useRef<HTMLDivElement>(null);

  // Auto-scroll to the panel when it mounts
  useEffect(() => {
    panelRef.current?.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }, []);

  const updateContent = (index: number, newContent: string) => {
    setEditedScripts((prev) => {
      const next = [...prev];
      next[index] = { ...next[index], content: newContent };
      return next;
    });
  };

  const sendWriteRequest = (scriptsToWrite: GeneratedScript[], onSuccess: (written: string[]) => void) => {
    const socket = webSocketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN)
    {
      setWriteResult("WebSocket not connected.");
      return;
    }

    setWriting(true);
    setWriteResult(null);

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
        onSuccess(written);
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
      scripts: scriptsToWrite.map((s) => ({
        path: s.path,
        content: s.content,
        executable: s.executable,
      })),
    }));
  };

  const onAcceptOne = (index: number) => {
    sendWriteRequest([editedScripts[index]], (written) => {
      setAcceptedSet((prev) => {
        const next = new Set(prev);
        next.add(index);
        // If all scripts are now accepted, auto-dismiss after a short delay
        if (next.size === editedScripts.length)
        {
          setWriteResult(`Wrote ${written.length} script${written.length !== 1 ? "s" : ""} to disk.`);
          setTimeout(onDone, 1500);
        }
        else
        {
          setWriteResult(`Wrote ${editedScripts[index].path}`);
        }
        return next;
      });
    });
  };

  const onAcceptAll = () => {
    const remaining = editedScripts.filter((_, i) => !acceptedSet.has(i));
    if (remaining.length === 0)
    {
      onDone();
      return;
    }

    sendWriteRequest(remaining, (written) => {
      setWriteResult(`Wrote ${written.length} script${written.length !== 1 ? "s" : ""} to disk.`);
      setTimeout(onDone, 1500);
    });
  };

  const onSkip = () => {
    onDone();
  };

  const remainingCount = editedScripts.length - acceptedSet.size;

  return (
    <div className="scriptReviewPanel" ref={panelRef}>
      <div className="scriptReviewHeader">
        <span className="scriptReviewTitle">
          Generated Scripts ({editedScripts.length})
        </span>
      </div>

      <div className="scriptReviewList">
        {editedScripts.map((script, i) => (
          <div key={script.path} className={`scriptReviewItem${expandedIndex === i ? " expanded" : ""}${acceptedSet.has(i) ? " accepted" : ""}`}>
            <button
              className="scriptReviewItemHeader"
              type="button"
              onClick={() => setExpandedIndex(expandedIndex === i ? null : i)}
            >
              <span className="scriptReviewPath">{script.path}</span>
              {acceptedSet.has(i)
                ? <span className="scriptReviewItemAccepted">accepted</span>
                : (
                  <button
                    className="scriptReviewItemAccept"
                    type="button"
                    onClick={(e) => { e.stopPropagation(); onAcceptOne(i); }}
                    disabled={writing}
                  >
                    accept
                  </button>
                )}
              <span className="scriptReviewToggle">
                {expandedIndex === i ? "\u25BE" : "\u25B8"}
              </span>
            </button>

            {expandedIndex === i && (
              <textarea
                className="scriptReviewCode"
                value={script.content}
                onChange={(e) => updateContent(i, e.target.value)}
                spellCheck={false}
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
          disabled={writing || remainingCount === 0}
        >
          {writing ? "Writing..." : remainingCount === 0 ? "All Accepted" : remainingCount === editedScripts.length ? "Accept All" : `Accept Remaining (${remainingCount})`}
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
