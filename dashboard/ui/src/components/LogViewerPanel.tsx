import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import { fetchLog, fetchSecurityLog, fetchLogAnalyzeLastRun } from "../api";
import type { AnalyzeLastRunResponse } from "../types";

const LINE_HEIGHT = 18;
const OVERSCAN = 40;
const MAX_LINES = 100_000;
const INITIAL_TAIL = 10_000;
const SECURITY_POLL_MS = 3000;

type LogTab = "application" | "security";

interface LogViewerPanelProps {
  registerLogCallback: (cb: ((lines: string[]) => void) | null) => void;
}

export default function LogViewerPanel({ registerLogCallback }: LogViewerPanelProps) {
  const [logTab, setLogTab] = useState<LogTab>("application");
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
  const [errorMsg, setErrorMsg] = useState<string | null>(null);

  // Security log state (separate from app log)
  const [secLines, setSecLines] = useState<string[]>([]);
  const [secByteOffset, setSecByteOffset] = useState<number>(0);
  const [secLoading, setSecLoading] = useState(true);
  const [secErrorMsg, setSecErrorMsg] = useState<string | null>(null);
  const secOffsetRef = useRef(0);

  const containerRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [containerHeight, setContainerHeight] = useState(600);
  const offsetRef = useRef(0);
  const linesRef = useRef<string[]>([]);
  const autoScrollRef = useRef(true);

  // Absolute line offset: the 1-indexed line number of the first loaded line.
  // e.g. if the file has 41000 lines and we loaded the last 10000,
  // lineNumOffset = 31001 (first loaded line is file line 31001).
  const [lineNumOffset, setLineNumOffset] = useState(1);

  // Keep refs in sync
  useEffect(() => { offsetRef.current = byteOffset; }, [byteOffset]);
  useEffect(() => { linesRef.current = lines; }, [lines]);
  useEffect(() => { autoScrollRef.current = autoScroll; }, [autoScroll]);
  useEffect(() => { secOffsetRef.current = secByteOffset; }, [secByteOffset]);

  // Active lines for rendering (switches between app and security)
  const activeLines = logTab === "application" ? lines : secLines;
  const activeLoading = logTab === "application" ? loading : secLoading;
  const activeErrorMsg = logTab === "application" ? errorMsg : secErrorMsg;

  // Scroll to bottom when auto-scroll is on and lines change
  useEffect(() => {
    if (autoScroll && containerRef.current) {
      containerRef.current.scrollTop = containerRef.current.scrollHeight;
    }
  }, [activeLines, autoScroll]);

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

  // Initial load — application log
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const data = await fetchLog({ tail: INITIAL_TAIL });
        if (cancelled) return;
        if (!data.ok) {
          setErrorMsg(data.error || "Log file not available");
          setLoading(false);
          return;
        }
        setLines(data.lines);
        setByteOffset(data.byteOffset);
        if (data.totalLines !== undefined)
        {
          setLineNumOffset(data.totalLines - data.lines.length + 1);
        }
        setLoading(false);
      } catch {
        setErrorMsg("Could not connect to log API");
        setLoading(false);
      }
    })();
    return () => { cancelled = true; };
  }, []);

  // Live log streaming via WebSocket (application log only)
  useEffect(() => {
    if (loading) return;
    registerLogCallback((newLines: string[]) => {
      setLines((prev) => {
        const next = [...prev, ...newLines];
        return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
      });
    });
    return () => registerLogCallback(null);
  }, [loading, registerLogCallback]);

  // Security log: initial load + delta polling
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const data = await fetchSecurityLog({ tail: INITIAL_TAIL });
        if (cancelled) return;
        if (!data.ok) {
          setSecErrorMsg(data.error || "Security log not available");
          setSecLoading(false);
          return;
        }
        setSecLines(data.lines);
        setSecByteOffset(data.byteOffset);
        setSecLoading(false);
      } catch {
        setSecErrorMsg("Could not connect to security log API");
        setSecLoading(false);
      }
    })();
    return () => { cancelled = true; };
  }, []);

  // Security log delta polling (only when security tab is active)
  useEffect(() => {
    if (logTab !== "security" || secLoading) return;
    const interval = setInterval(async () => {
      try {
        const data = await fetchSecurityLog({ offset: secOffsetRef.current });
        if (data.ok && data.lines.length > 0) {
          setSecLines((prev) => {
            const next = [...prev, ...data.lines];
            return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
          });
          setSecByteOffset(data.byteOffset);
        }
      } catch { /* ignore polling errors */ }
    }, SECURITY_POLL_MS);
    return () => clearInterval(interval);
  }, [logTab, secLoading]);

  // Search: compute match indices when searchTerm or activeLines change
  useEffect(() => {
    if (!searchTerm) {
      setSearchMatchIndices([]);
      setCurrentMatchIdx(-1);
      return;
    }
    const lower = searchTerm.toLowerCase();
    const matches: number[] = [];
    for (let i = 0; i < activeLines.length; i++) {
      if (activeLines[i].toLowerCase().includes(lower)) {
        matches.push(i);
      }
    }
    setSearchMatchIndices(matches);
    setCurrentMatchIdx(matches.length > 0 ? 0 : -1);
  }, [searchTerm, activeLines]);

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
    // Convert absolute 1-indexed file line number to array index.
    const idx = lineNum - lineNumOffset;
    if (idx < 0 || idx >= activeLines.length) return;
    el.scrollTop = Math.max(0, idx * LINE_HEIGHT - containerHeight / 3);
    setAutoScroll(false);
  }, [containerHeight, lineNumOffset, activeLines.length]);

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
  const totalHeight = activeLines.length * LINE_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / LINE_HEIGHT) - OVERSCAN);
  const endIdx = Math.min(activeLines.length, Math.ceil((scrollTop + containerHeight) / LINE_HEIGHT) + OVERSCAN);

  const visibleLines = useMemo(() => {
    const searchLower = searchTerm.toLowerCase();
    const currentHighlightLine = currentMatchIdx >= 0 ? searchMatchIndices[currentMatchIdx] : -1;
    const result: JSX.Element[] = [];

    for (let i = startIdx; i < endIdx; i++) {
      const line = activeLines[i];
      const isCurrentMatch = i === currentHighlightLine;
      const isMatchLine = searchTerm && searchMatchIndices.includes(i);

      // Determine log level color
      let levelClass = "";
      if (line.includes("[error]") || line.includes("[critical]")) levelClass = "log-error";
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
          <span className="log-line-num">{i + lineNumOffset}</span>
          <span className="log-line-text">{content}</span>
        </div>
      );
    }
    return result;
  }, [startIdx, endIdx, activeLines, searchTerm, currentMatchIdx, searchMatchIndices, lineNumOffset]);

  const handleTabChange = useCallback((tab: LogTab) => {
    setLogTab(tab);
    setSearchTerm("");
    setShowAnalyze(false);
    setAutoScroll(true);
    // Reset scroll to top on tab switch
    if (containerRef.current) {
      containerRef.current.scrollTop = 0;
    }
    setScrollTop(0);
  }, []);

  const activeByteOffset = logTab === "application" ? byteOffset : secByteOffset;

  return (
    <div className="log-viewer">
      {/* Toolbar */}
      <div className="log-toolbar">
        <div className="log-toolbar-left">
          <span className="log-tab-group">
            <button
              className={`log-tab-btn ${logTab === "application" ? "log-tab-btn-active" : ""}`}
              onClick={() => handleTabChange("application")}
            >
              Application
            </button>
            <button
              className={`log-tab-btn ${logTab === "security" ? "log-tab-btn-active" : ""}`}
              onClick={() => handleTabChange("security")}
            >
              Security
            </button>
          </span>
          <span className="log-stats">
            {activeLines.length.toLocaleString()} lines | {(activeByteOffset / 1024).toFixed(0)} KB
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
          {logTab === "application" && (
            <button
              className={`log-btn ${showAnalyze ? "log-btn-active" : ""}`}
              onClick={() => { if (showAnalyze) { setShowAnalyze(false); } else { handleAnalyze(); } }}
              title="keyboard shortcut: 1"
            >
              Analyze
            </button>
          )}
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
            {autoScroll ? "Follow" : "Paused"}
          </button>
        </div>
      </div>

      {/* Analyze overlay (application log only) */}
      {showAnalyze && logTab === "application" && (
        <div className="log-analyze-overlay">
          <div className="log-analyze-panel">
            <div className="log-analyze-header">
              <span className="log-analyze-title">Run Analysis</span>
              {analyzeData?.found && (analyzeData.totalRuns ?? 0) > 1 && (
                <span className="log-analyze-run-nav">
                  <button className="log-nav-btn" onClick={() => navigateRun(-1)} title="Newer run">◀</button>
                  <span className="log-search-count">
                    {(analyzeData.runIndex ?? 0) + 1}/{analyzeData.totalRuns}
                  </span>
                  <button className="log-nav-btn" onClick={() => navigateRun(1)} title="Older run">▶</button>
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
        {activeLoading ? (
          <div className="log-loading">Loading log...</div>
        ) : activeErrorMsg ? (
          <div className="log-loading" style={{ color: "#f87171", padding: 24, textAlign: "center" }}>
            <div style={{ fontSize: 16, marginBottom: 8 }}>{activeErrorMsg}</div>
            <div style={{ color: "#9ca3af", fontSize: 13 }}>
              {logTab === "application"
                ? "The log file (log/log.txt) may not exist yet. Start a workflow or check that JarvisAgent has write access to its log directory."
                : "The security log (log/security.txt) may not exist yet. Security events are logged in Engine edition when authentication is active."}
            </div>
          </div>
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
