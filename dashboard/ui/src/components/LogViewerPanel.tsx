import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import { fetchLog, fetchLogAnalyzeLastRun } from "../api";
import type { AnalyzeLastRunResponse } from "../types";

const LINE_HEIGHT = 18;
const OVERSCAN = 40;
const MAX_LINES = 100_000;
const POLL_INTERVAL_MS = 500;
const INITIAL_TAIL = 10_000;

export default function LogViewerPanel() {
  const [lines, setLines] = useState<string[]>([]);
  const [byteOffset, setByteOffset] = useState<number>(0);
  const [autoScroll, setAutoScroll] = useState(true);
  const [searchTerm, setSearchTerm] = useState("");
  const [searchMatchIndices, setSearchMatchIndices] = useState<number[]>([]);
  const [currentMatchIdx, setCurrentMatchIdx] = useState(-1);
  const [analyzeData, setAnalyzeData] = useState<AnalyzeLastRunResponse | null>(null);
  const [showAnalyze, setShowAnalyze] = useState(false);
  const [analyzeIssueIdx, setAnalyzeIssueIdx] = useState(-1);
  const [analyzeRunIndex, setAnalyzeRunIndex] = useState(0);
  const [loading, setLoading] = useState(true);

  const containerRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [containerHeight, setContainerHeight] = useState(600);
  const offsetRef = useRef(0);
  const linesRef = useRef<string[]>([]);
  const autoScrollRef = useRef(true);

  // Keep refs in sync
  useEffect(() => { offsetRef.current = byteOffset; }, [byteOffset]);
  useEffect(() => { linesRef.current = lines; }, [lines]);
  useEffect(() => { autoScrollRef.current = autoScroll; }, [autoScroll]);

  // Scroll to bottom when auto-scroll is on and lines change
  useEffect(() => {
    if (autoScroll && containerRef.current) {
      containerRef.current.scrollTop = containerRef.current.scrollHeight;
    }
  }, [lines, autoScroll]);

  // Measure container height
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const ro = new ResizeObserver((entries) => {
      for (const e of entries) {
        setContainerHeight(e.contentRect.height);
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // Initial load
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const data = await fetchLog({ tail: INITIAL_TAIL });
        if (cancelled) return;
        setLines(data.lines);
        setByteOffset(data.byteOffset);
        setLoading(false);
      } catch {
        setLoading(false);
      }
    })();
    return () => { cancelled = true; };
  }, []);

  // Delta polling
  useEffect(() => {
    if (loading) return;
    const id = setInterval(async () => {
      try {
        const data = await fetchLog({ offset: offsetRef.current });
        if (data.lines.length === 0) return;
        offsetRef.current = data.byteOffset;
        setByteOffset(data.byteOffset);
        setLines((prev) => {
          const next = [...prev, ...data.lines];
          return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
        });
      } catch {
        // ignore transient errors
      }
    }, POLL_INTERVAL_MS);
    return () => clearInterval(id);
  }, [loading]);

  // Search: compute match indices when searchTerm or lines change
  useEffect(() => {
    if (!searchTerm) {
      setSearchMatchIndices([]);
      setCurrentMatchIdx(-1);
      return;
    }
    const lower = searchTerm.toLowerCase();
    const matches: number[] = [];
    for (let i = 0; i < lines.length; i++) {
      if (lines[i].toLowerCase().includes(lower)) {
        matches.push(i);
      }
    }
    setSearchMatchIndices(matches);
    setCurrentMatchIdx(matches.length > 0 ? 0 : -1);
  }, [searchTerm, lines]);

  // Scroll to current match
  useEffect(() => {
    if (currentMatchIdx < 0 || searchMatchIndices.length === 0) return;
    const lineIdx = searchMatchIndices[currentMatchIdx];
    const el = containerRef.current;
    if (el) {
      el.scrollTop = lineIdx * LINE_HEIGHT - containerHeight / 2;
      setAutoScroll(false);
    }
  }, [currentMatchIdx, searchMatchIndices, containerHeight]);

  const navigateMatch = useCallback((delta: number) => {
    setCurrentMatchIdx((prev) => {
      if (searchMatchIndices.length === 0) return -1;
      const next = (prev + delta + searchMatchIndices.length) % searchMatchIndices.length;
      return next;
    });
  }, [searchMatchIndices]);

  // Keyboard shortcuts
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      // Don't capture when typing in input
      if (e.target instanceof HTMLInputElement) {
        if (e.key === "Enter") {
          e.preventDefault();
          navigateMatch(e.shiftKey ? -1 : 1);
        }
        if (e.key === "Escape") {
          (e.target as HTMLInputElement).blur();
          setSearchTerm("");
        }
        return;
      }
      if (e.key === "1") {
        e.preventDefault();
        handleAnalyze();
      }
      if (e.key === "Escape") {
        setShowAnalyze(false);
        setSearchTerm("");
      }
      if (e.key === "/" || (e.ctrlKey && e.key === "f")) {
        e.preventDefault();
        const input = document.getElementById("log-search-input");
        if (input) (input as HTMLInputElement).focus();
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [navigateMatch]);

  const scrollToLine = useCallback((lineNum: number) => {
    const el = containerRef.current;
    if (!el) return;
    const idx = lineNum - 1; // convert 1-indexed to 0-indexed
    el.scrollTop = Math.max(0, idx * LINE_HEIGHT - containerHeight / 3);
    setAutoScroll(false);
  }, [containerHeight]);

  const handleAnalyze = useCallback(async (index = 0) => {
    try {
      const data = await fetchLogAnalyzeLastRun(index);
      setAnalyzeData(data);
      setShowAnalyze(true);
      setAnalyzeIssueIdx(-1);
      setAnalyzeRunIndex(data.runIndex ?? 0);
      if (data.found && data.startLine) {
        scrollToLine(data.startLine);
      }
    } catch {
      setAnalyzeData(null);
      setShowAnalyze(true);
    }
  }, [scrollToLine]);

  const navigateRun = useCallback((delta: number) => {
    const total = analyzeData?.totalRuns ?? 0;
    if (total === 0) return;
    const next = (analyzeRunIndex + delta + total) % total;
    handleAnalyze(next);
  }, [analyzeData, analyzeRunIndex, handleAnalyze]);

  const navigateIssue = useCallback((delta: number) => {
    if (!analyzeData?.issues?.length) return;
    setAnalyzeIssueIdx((prev) => {
      const next = (prev + delta + analyzeData.issues!.length) % analyzeData.issues!.length;
      scrollToLine(analyzeData.issues![next].line);
      return next;
    });
  }, [analyzeData, scrollToLine]);

  // Scroll handler
  const handleScroll = useCallback(() => {
    const el = containerRef.current;
    if (!el) return;
    setScrollTop(el.scrollTop);
    // Disable auto-scroll if user scrolls up
    const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 40;
    setAutoScroll(atBottom);
  }, []);

  // Virtual scroll computation
  const totalHeight = lines.length * LINE_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / LINE_HEIGHT) - OVERSCAN);
  const endIdx = Math.min(lines.length, Math.ceil((scrollTop + containerHeight) / LINE_HEIGHT) + OVERSCAN);

  const visibleLines = useMemo(() => {
    const searchLower = searchTerm.toLowerCase();
    const currentHighlightLine = currentMatchIdx >= 0 ? searchMatchIndices[currentMatchIdx] : -1;
    const result: JSX.Element[] = [];

    for (let i = startIdx; i < endIdx; i++) {
      const line = lines[i];
      const isCurrentMatch = i === currentHighlightLine;
      const isMatchLine = searchTerm && searchMatchIndices.includes(i);

      // Determine log level color
      let levelClass = "";
      if (line.includes("[error]")) levelClass = "log-error";
      else if (line.includes("[warning]") || line.includes("[warn]")) levelClass = "log-warn";

      let content: React.ReactNode = line;
      if (searchTerm && line.toLowerCase().includes(searchLower)) {
        content = highlightSearch(line, searchTerm);
      }

      result.push(
        <div
          key={i}
          className={`log-line ${levelClass} ${isCurrentMatch ? "log-current-match" : ""} ${isMatchLine && !isCurrentMatch ? "log-match-line" : ""}`}
          style={{
            position: "absolute",
            top: i * LINE_HEIGHT,
            height: LINE_HEIGHT,
            left: 0,
            right: 0,
          }}
        >
          <span className="log-line-num">{i + 1}</span>
          <span className="log-line-text">{content}</span>
        </div>
      );
    }
    return result;
  }, [startIdx, endIdx, lines, searchTerm, currentMatchIdx, searchMatchIndices]);

  return (
    <div className="log-viewer">
      {/* Toolbar */}
      <div className="log-toolbar">
        <div className="log-toolbar-left">
          <span className="log-title">Log Viewer</span>
          <span className="log-stats">
            {lines.length.toLocaleString()} lines | {(byteOffset / 1024).toFixed(0)} KB
          </span>
        </div>
        <div className="log-toolbar-center">
          <input
            id="log-search-input"
            className="log-search-input"
            type="text"
            placeholder="Search (/ or Ctrl+F)..."
            value={searchTerm}
            onChange={(e) => setSearchTerm(e.target.value)}
          />
          {searchTerm && (
            <span className="log-search-nav">
              <span className="log-search-count">
                {searchMatchIndices.length > 0
                  ? `${currentMatchIdx + 1}/${searchMatchIndices.length}`
                  : "0 matches"}
              </span>
              <button className="log-nav-btn" onClick={() => navigateMatch(-1)} title="Previous (Shift+Enter)">▲</button>
              <button className="log-nav-btn" onClick={() => navigateMatch(1)} title="Next (Enter)">▼</button>
            </span>
          )}
        </div>
        <div className="log-toolbar-right">
          <button
            className={`log-btn ${showAnalyze ? "log-btn-active" : ""}`}
            onClick={() => { if (showAnalyze) { setShowAnalyze(false); } else { handleAnalyze(); } }}
            title="keyboard shortcut: 1"
          >
            ⓘ Analyze
          </button>
          <button
            className={`log-btn ${autoScroll ? "log-btn-active" : ""}`}
            onClick={() => {
              setAutoScroll(!autoScroll);
              if (!autoScroll && containerRef.current) {
                containerRef.current.scrollTop = containerRef.current.scrollHeight;
              }
            }}
            title="Auto-scroll to bottom"
          >
            {autoScroll ? "⏬ Follow" : "⏸ Paused"}
          </button>
        </div>
      </div>

      {/* Analyze overlay */}
      {showAnalyze && (
        <div className="log-analyze-overlay">
          <div className="log-analyze-panel">
            <div className="log-analyze-header">
              <span className="log-analyze-title">Run Analysis</span>
              {analyzeData?.found && (analyzeData.totalRuns ?? 0) > 1 && (
                <span className="log-analyze-run-nav">
                  <button className="log-nav-btn" onClick={() => navigateRun(1)} title="Older run">◀</button>
                  <span className="log-search-count">
                    {(analyzeData.runIndex ?? 0) + 1}/{analyzeData.totalRuns}
                  </span>
                  <button className="log-nav-btn" onClick={() => navigateRun(-1)} title="Newer run">▶</button>
                </span>
              )}
              <button className="log-nav-btn" onClick={() => setShowAnalyze(false)}>✕</button>
            </div>
            {analyzeData === null ? (
              <div className="log-analyze-body">Error fetching analysis.</div>
            ) : !analyzeData.found ? (
              <div className="log-analyze-body">{analyzeData.message || "No workflow run start found in log."}</div>
            ) : (
              <div className="log-analyze-body">
                <div className="log-analyze-row">
                  <span className="log-analyze-label">Workflow</span>
                  <span>{analyzeData.workflowId}</span>
                </div>
                <div className="log-analyze-row">
                  <span className="log-analyze-label">Run ID</span>
                  <span className="mono">{analyzeData.runId}</span>
                </div>
                <div className="log-analyze-row">
                  <span className="log-analyze-label">State</span>
                  <span className={`state-${analyzeData.state}`}>{analyzeData.state}</span>
                </div>
                <div className="log-analyze-row">
                  <span className="log-analyze-label">Started</span>
                  <span>{analyzeData.startedAt}</span>
                </div>
                <div className="log-analyze-row">
                  <span className="log-analyze-label">Completed</span>
                  <span>{analyzeData.completedAt || <span className="muted">(still running)</span>}</span>
                </div>
                <div className="log-analyze-row">
                  <span className="log-analyze-label">Log region</span>
                  <span>
                    <button className="log-link-btn" onClick={() => scrollToLine(analyzeData.startLine!)}>
                      L{analyzeData.startLine}
                    </button>
                    {" — "}
                    {(analyzeData.endLine ?? -1) > 0 ? (
                      <button className="log-link-btn" onClick={() => scrollToLine(analyzeData.endLine!)}>
                        L{analyzeData.endLine}
                      </button>
                    ) : (
                      <span className="muted">end not found</span>
                    )}
                  </span>
                </div>

                {(analyzeData.issueCount ?? 0) > 0 ? (
                  <div className="log-analyze-section">
                    <div className="log-analyze-section-title" style={{ display: "flex", alignItems: "center", gap: 8 }}>
                      <span>Issues ({analyzeData.issueCount})</span>
                      <button className="log-nav-btn" onClick={() => navigateIssue(-1)} title="Previous issue">▲</button>
                      <button className="log-nav-btn" onClick={() => navigateIssue(1)} title="Next issue">▼</button>
                      {analyzeIssueIdx >= 0 && (
                        <span className="log-search-count">
                          {analyzeIssueIdx + 1}/{analyzeData.issues!.length}
                        </span>
                      )}
                    </div>
                    {analyzeData.issues!.slice(0, 100).map((issue, idx) => (
                      <div
                        key={idx}
                        className={`log-analyze-issue ${issue.severity === "error" ? "log-error" : "log-warn"} ${idx === analyzeIssueIdx ? "log-analyze-issue-active" : ""}`}
                        onClick={() => { setAnalyzeIssueIdx(idx); scrollToLine(issue.line); }}
                      >
                        <span className="log-analyze-issue-line">L{issue.line}</span>
                        <span className="log-analyze-issue-text">{issue.text}</span>
                      </div>
                    ))}
                    {analyzeData.issues!.length > 100 && (
                      <div className="muted" style={{ padding: "4px 0" }}>...and {analyzeData.issues!.length - 100} more</div>
                    )}
                  </div>
                ) : (
                  <div className="log-analyze-section">
                    <div className="log-analyze-section-title state-succeeded">No issues found</div>
                  </div>
                )}
              </div>
            )}
          </div>
        </div>
      )}

      {/* Log content */}
      <div
        ref={containerRef}
        className="log-content"
        onScroll={handleScroll}
      >
        {loading ? (
          <div className="log-loading">Loading log...</div>
        ) : (
          <div style={{ position: "relative", height: totalHeight, minHeight: "100%" }}>
            {visibleLines}
          </div>
        )}
      </div>
    </div>
  );
}

function highlightSearch(text: string, term: string): React.ReactNode {
  const lower = text.toLowerCase();
  const termLower = term.toLowerCase();
  const parts: React.ReactNode[] = [];
  let lastIdx = 0;
  let pos = lower.indexOf(termLower);
  let key = 0;
  while (pos !== -1) {
    if (pos > lastIdx) {
      parts.push(text.slice(lastIdx, pos));
    }
    parts.push(
      <mark key={key++} className="log-highlight">{text.slice(pos, pos + term.length)}</mark>
    );
    lastIdx = pos + term.length;
    pos = lower.indexOf(termLower, lastIdx);
  }
  if (lastIdx < text.length) {
    parts.push(text.slice(lastIdx));
  }
  return <>{parts}</>;
}
