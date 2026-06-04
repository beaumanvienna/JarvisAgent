import React, { useCallback, useEffect, useRef, useState } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { useAssistantWebSocket } from "../hooks/useAssistantWebSocket";
import type { AssistantTurn } from "../hooks/useAssistantWebSocket";
import "@xterm/xterm/css/xterm.css";

// ANSI color helpers
const RESET = "\x1b[0m";
const BOLD = "\x1b[1m";
const DIM = "\x1b[2m";
const GREEN = "\x1b[32m";
const CYAN = "\x1b[36m";
const YELLOW = "\x1b[33m";
const RED = "\x1b[31m";
const MAGENTA = "\x1b[35m";

const PROMPT = `${BOLD}${GREEN}> ${RESET}`;
const ASSISTANT_PREFIX = `${BOLD}${CYAN}assistant${RESET}${DIM}: ${RESET}`;
const USER_PREFIX = `${BOLD}${GREEN}you${RESET}${DIM}: ${RESET}`;
const TOOL_PREFIX = `${DIM}${MAGENTA}tool${RESET}${DIM}: ${RESET}`;
const THINKING = `${DIM}${YELLOW}thinking...${RESET}`;
const APPROVAL_PREFIX = `${BOLD}${YELLOW}\u26a0 ${RESET}`;

export default function AssistantView(): JSX.Element {
  const [state, actions] = useAssistantWebSocket();
  const termRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<Terminal | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const inputBufferRef = useRef<string>("");
  const cursorPosRef = useRef<number>(0);
  const renderedTurnsRef = useRef<number>(0);
  const thinkingLineRef = useRef<boolean>(false);
  const approvalActiveRef = useRef<boolean>(false);
  const approvalRequestIdRef = useRef<string>("");

  // Ghost-text auto-completion
  const ghostSuffixRef = useRef<string>("");
  const ghostCandidateIndexRef = useRef<number>(-1);
  const completionTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Reverse history search (Ctrl+R)
  const searchModeRef = useRef<boolean>(false);
  const searchQueryRef = useRef<string>("");
  const searchMatchIndexRef = useRef<number>(0);
  const searchMatchesRef = useRef<string[]>([]);
  const savedInputRef = useRef<string>("");
  const historyRef = useRef<string[]>([]);
  const historyFetchedRef = useRef<boolean>(false);

  // Arrow-key history navigation (Up/Down)
  const historyIndexRef = useRef<number>(-1); // -1 = not browsing history
  const savedInputBeforeHistoryRef = useRef<string>(""); // saves current input when entering history

  // Initialize xterm.js
  useEffect(() => {
    if (!termRef.current || xtermRef.current) return;

    const term = new Terminal({
      cursorBlink: true,
      fontSize: 14,
      fontFamily: "'JetBrains Mono', 'Fira Code', 'Cascadia Code', monospace",
      theme: {
        background: "#1a1b26",
        foreground: "#c0caf5",
        cursor: "#c0caf5",
        selectionBackground: "#33467c",
        black: "#15161e",
        red: "#f7768e",
        green: "#9ece6a",
        yellow: "#e0af68",
        blue: "#7aa2f7",
        magenta: "#bb9af7",
        cyan: "#7dcfff",
        white: "#a9b1d6",
        brightBlack: "#414868",
        brightRed: "#f7768e",
        brightGreen: "#9ece6a",
        brightYellow: "#e0af68",
        brightBlue: "#7aa2f7",
        brightMagenta: "#bb9af7",
        brightCyan: "#7dcfff",
        brightWhite: "#c0caf5",
      },
      convertEol: true,
      scrollback: 5000,
    });

    const fitAddon = new FitAddon();
    term.loadAddon(fitAddon);
    term.open(termRef.current);

    // Fit after a short delay to let the container render, then auto-focus.
    requestAnimationFrame(() => {
      fitAddon.fit();
      term.focus();
    });

    xtermRef.current = term;
    fitAddonRef.current = fitAddon;

    // Welcome banner
    term.writeln(
      `${BOLD}${MAGENTA}╔═════════════════════════════════════════╗${RESET}`
    );
    term.writeln(
      `${BOLD}${MAGENTA}║   JarvisAgent AI Assistant Terminal     ║${RESET}`
    );
    term.writeln(
      `${BOLD}${MAGENTA}╚═════════════════════════════════════════╝${RESET}`
    );
    term.writeln("");
    term.writeln(
      `${DIM}Type a message and press Enter to chat.${RESET}`
    );
    term.writeln(
      `${DIM}Type /help for available commands.  Tab: autocomplete  Ctrl+R: search history${RESET}`
    );
    term.writeln("");
    term.write(PROMPT);

    // Handle keyboard input
    term.onData((data) => {
      handleTerminalInput(data);
    });

    return () => {
      term.dispose();
      xtermRef.current = null;
      fitAddonRef.current = null;
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  // Resize handler
  useEffect(() => {
    const handleResize = () => {
      if (fitAddonRef.current) {
        fitAddonRef.current.fit();
      }
    };
    window.addEventListener("resize", handleResize);
    return () => window.removeEventListener("resize", handleResize);
  }, []);

  // Render new turns as they arrive
  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;

    const newTurns = state.turns.slice(renderedTurnsRef.current);
    for (const turn of newTurns) {
      // Clear thinking indicator if present
      if (thinkingLineRef.current) {
        term.write("\r\x1b[2K"); // clear current line
        thinkingLineRef.current = false;
      }

      if (turn.role === "user") {
        // User messages are already echoed at input time, but if the turn
        // came from session history replay, we need to show it.
        if (renderedTurnsRef.current < state.turns.length - newTurns.length + newTurns.indexOf(turn)) {
          // This is a history replay turn
          term.write("\r\x1b[2K");
          writeWrapped(term, USER_PREFIX, turn.text);
          term.writeln("");
        }
        // If it's the turn we just sent, it was already echoed — skip rendering.
      } else if (turn.role === "tool") {
        // Tool status/result — show in dim magenta, no prompt after
        if (thinkingLineRef.current) {
          term.write("\r\x1b[2K");
          thinkingLineRef.current = false;
        }
        term.writeln(`${DIM}${MAGENTA}  ⚙ ${turn.text}${RESET}`);
      } else {
        writeWrapped(term, ASSISTANT_PREFIX, turn.text);
        term.writeln("");
        term.writeln("");
        term.write(PROMPT);
        inputBufferRef.current = "";
        cursorPosRef.current = 0;
      }
    }
    renderedTurnsRef.current = state.turns.length;
  }, [state.turns]);

  // Show thinking indicator
  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;

    if (state.thinking && !thinkingLineRef.current) {
      term.write("\r\x1b[2K" + THINKING);
      thinkingLineRef.current = true;
    }
  }, [state.thinking]);

  // Show approval prompt when a tool requires user approval
  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;

    if (state.pendingApproval && !approvalActiveRef.current) {
      // Clear thinking indicator if present
      if (thinkingLineRef.current) {
        term.write("\r\x1b[2K");
        thinkingLineRef.current = false;
      }
      approvalActiveRef.current = true;
      approvalRequestIdRef.current = state.pendingApproval.requestId;
      term.writeln(`${APPROVAL_PREFIX}${BOLD}${YELLOW}Tool requires approval:${RESET} ${state.pendingApproval.description}`);
      term.write(`${BOLD}${YELLOW}  Allow? [Y/n] ${RESET}`);
    } else if (!state.pendingApproval && approvalActiveRef.current) {
      approvalActiveRef.current = false;
      approvalRequestIdRef.current = "";
    }
  }, [state.pendingApproval]);

  // Replay history when resuming a session
  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;

    // When session changes and we have turns from history, render them all.
    if (state.sessionId && state.turns.length > 0 && renderedTurnsRef.current === 0) {
      term.write("\r\x1b[2K");
      term.writeln(`${DIM}--- Session: ${state.sessionId} ---${RESET}`);
      term.writeln("");
      for (const turn of state.turns) {
        if (turn.role === "user") {
          writeWrapped(term, USER_PREFIX, turn.text);
        } else {
          writeWrapped(term, ASSISTANT_PREFIX, turn.text);
        }
        term.writeln("");
      }
      term.writeln("");
      term.write(PROMPT);
      renderedTurnsRef.current = state.turns.length;
    }
  }, [state.sessionId, state.turns]);

  // Clear ghost text state (does not redraw).
  const clearGhost = useCallback(() => {
    ghostSuffixRef.current = "";
    ghostCandidateIndexRef.current = -1;
  }, []);

  // Redraw the current input line (prompt + buffer + optional ghost suffix).
  const redrawInputLine = useCallback((term: Terminal) => {
    const buf = inputBufferRef.current;
    const pos = cursorPosRef.current;
    const ghost = ghostSuffixRef.current;
    term.write("\r\x1b[2K" + PROMPT + buf);
    if (ghost) {
      term.write(DIM + ghost + RESET);
    }
    const moveBack = (buf.length - pos) + ghost.length;
    if (moveBack > 0) {
      term.write(`\x1b[${moveBack}D`);
    }
  }, []);

  // Redraw the search-mode prompt: (reverse-i-search)'query': matchedText
  const redrawSearchLine = useCallback((term: Terminal) => {
    const query = searchQueryRef.current;
    const matches = searchMatchesRef.current;
    const idx = searchMatchIndexRef.current;
    const matched = matches.length > 0 ? matches[idx] ?? "" : "";
    term.write(
      "\r\x1b[2K" +
        `${DIM}(reverse-i-search)${RESET}${YELLOW}'${query}'${RESET}${DIM}: ${RESET}${matched}`
    );
  }, []);

  // Schedule a debounced completion request after typing.
  const scheduleCompletion = useCallback(
    (prefix: string) => {
      if (completionTimerRef.current) clearTimeout(completionTimerRef.current);
      if (prefix.length < 1) {
        clearGhost();
        return;
      }
      completionTimerRef.current = setTimeout(() => {
        actions.requestCompletion(prefix);
      }, 150);
    },
    [actions, clearGhost]
  );

  // Exit search mode and restore normal prompt.
  const exitSearchMode = useCallback(
    (term: Terminal, placeText?: string) => {
      searchModeRef.current = false;
      searchQueryRef.current = "";
      searchMatchIndexRef.current = 0;
      searchMatchesRef.current = [];
      if (placeText !== undefined) {
        inputBufferRef.current = placeText;
        cursorPosRef.current = placeText.length;
      } else {
        inputBufferRef.current = savedInputRef.current;
        cursorPosRef.current = savedInputRef.current.length;
      }
      savedInputRef.current = "";
      redrawInputLine(term);
    },
    [redrawInputLine]
  );

  // Filter history entries by search query and update matches.
  const updateSearchMatches = useCallback(() => {
    const q = searchQueryRef.current.toLowerCase();
    if (!q) {
      searchMatchesRef.current = historyRef.current;
    } else {
      searchMatchesRef.current = historyRef.current.filter((e) =>
        e.toLowerCase().includes(q)
      );
    }
    searchMatchIndexRef.current = 0;
  }, []);

  const handleTerminalInput = useCallback(
    (data: string) => {
      const term = xtermRef.current;
      if (!term) return;

      // If an approval prompt is active, intercept keystrokes for Y/n.
      if (approvalActiveRef.current && approvalRequestIdRef.current) {
        const ch = data[0];
        if (ch === "y" || ch === "Y" || ch === "\r" || ch === "\n") {
          term.writeln(`${GREEN}yes${RESET}`);
          term.writeln(`${DIM}${MAGENTA}  \u2713 Approved${RESET}`);
          actions.respondToApproval(approvalRequestIdRef.current, true);
        } else if (ch === "n" || ch === "N" || ch === "\x1b" || ch === "\x03") {
          term.writeln(`${RED}no${RESET}`);
          term.writeln(`${DIM}${MAGENTA}  \u2717 Denied${RESET}`);
          actions.respondToApproval(approvalRequestIdRef.current, false);
        }
        return;
      }

      // --- Reverse history search mode ---
      if (searchModeRef.current) {
        for (let si = 0; si < data.length; si++) {
          const sch = data[si];

          if (sch === "\x12") {
            // Ctrl+R again — cycle to next match
            if (searchMatchesRef.current.length > 1) {
              searchMatchIndexRef.current =
                (searchMatchIndexRef.current + 1) % searchMatchesRef.current.length;
              redrawSearchLine(term);
            }
          } else if (sch === "\r" || sch === "\n") {
            // Enter — send matched text
            const matches = searchMatchesRef.current;
            const matched = matches.length > 0 ? matches[searchMatchIndexRef.current] ?? "" : "";
            exitSearchMode(term, matched);
            if (matched.length > 0) {
              term.writeln("");
              actions.sendMessage(matched);
              inputBufferRef.current = "";
              cursorPosRef.current = 0;
            }
            return;
          } else if (sch === "\t" || (sch === "\x1b" && si + 2 < data.length && data[si + 1] === "[" && data[si + 2] === "C")) {
            // Tab or Right arrow — place on input line, exit search
            const matches = searchMatchesRef.current;
            const matched = matches.length > 0 ? matches[searchMatchIndexRef.current] ?? "" : "";
            exitSearchMode(term, matched);
            if (sch !== "\t") si += 2; // skip rest of escape sequence
            return;
          } else if (sch === "\x1b" || sch === "\x03") {
            // Esc or Ctrl+C — cancel search
            exitSearchMode(term);
            return;
          } else if (sch === "\x7f" || sch === "\b") {
            // Backspace — remove last char from search query
            if (searchQueryRef.current.length > 0) {
              searchQueryRef.current = searchQueryRef.current.slice(0, -1);
              updateSearchMatches();
              redrawSearchLine(term);
            }
          } else if (sch >= " ") {
            // Printable — add to search query
            searchQueryRef.current += sch;
            updateSearchMatches();
            redrawSearchLine(term);
          }
        }
        return;
      }

      // --- Normal input mode ---
      let i = 0;
      while (i < data.length) {
        // Check for escape sequences (\x1b[...)
        if (data[i] === "\x1b" && i + 2 < data.length && data[i + 1] === "[") {
          const code = data[i + 2];
          if (code === "D") {
            // Left arrow
            clearGhost();
            if (cursorPosRef.current > 0) {
              cursorPosRef.current--;
              redrawInputLine(term);
            }
          } else if (code === "A") {
            // Up arrow — previous history entry
            clearGhost();
            const history = historyRef.current;
            if (history.length > 0) {
              if (historyIndexRef.current === -1) {
                // Entering history — save current input
                savedInputBeforeHistoryRef.current = inputBufferRef.current;
                historyIndexRef.current = history.length - 1;
              } else if (historyIndexRef.current > 0) {
                historyIndexRef.current--;
              }
              inputBufferRef.current = history[historyIndexRef.current];
              cursorPosRef.current = inputBufferRef.current.length;
              redrawInputLine(term);
            }
          } else if (code === "B") {
            // Down arrow — next history entry
            clearGhost();
            if (historyIndexRef.current !== -1) {
              const history = historyRef.current;
              if (historyIndexRef.current < history.length - 1) {
                historyIndexRef.current++;
                inputBufferRef.current = history[historyIndexRef.current];
              } else {
                // Past the end — restore saved input
                historyIndexRef.current = -1;
                inputBufferRef.current = savedInputBeforeHistoryRef.current;
              }
              cursorPosRef.current = inputBufferRef.current.length;
              redrawInputLine(term);
            }
          } else if (code === "C") {
            // Right arrow
            if (
              cursorPosRef.current >= inputBufferRef.current.length &&
              ghostSuffixRef.current.length > 0
            ) {
              // Accept one character from ghost text
              const accepted = ghostSuffixRef.current[0];
              inputBufferRef.current += accepted;
              cursorPosRef.current++;
              ghostSuffixRef.current = ghostSuffixRef.current.slice(1);
              if (ghostSuffixRef.current.length === 0) ghostCandidateIndexRef.current = -1;
              redrawInputLine(term);
            } else if (cursorPosRef.current < inputBufferRef.current.length) {
              cursorPosRef.current++;
              clearGhost();
              redrawInputLine(term);
            }
          }
          i += 3;
          continue;
        }

        // Bare Esc — dismiss ghost text
        if (data[i] === "\x1b") {
          if (ghostSuffixRef.current) {
            clearGhost();
            redrawInputLine(term);
          }
          i++;
          continue;
        }

        const ch = data[i];
        i++;

        if (ch === "\t") {
          // Tab — accept ghost completion or cycle candidates
          if (ghostSuffixRef.current.length > 0) {
            // Accept full ghost text
            inputBufferRef.current += ghostSuffixRef.current;
            cursorPosRef.current = inputBufferRef.current.length;
            clearGhost();
            redrawInputLine(term);
          } else if (
            state.completionCandidates.length > 0 &&
            state.completionPrefix === inputBufferRef.current
          ) {
            // Cycle to next candidate
            ghostCandidateIndexRef.current =
              (ghostCandidateIndexRef.current + 1) % state.completionCandidates.length;
            const candidate = state.completionCandidates[ghostCandidateIndexRef.current];
            ghostSuffixRef.current = candidate.slice(inputBufferRef.current.length);
            redrawInputLine(term);
          } else {
            // Request completions for current input
            scheduleCompletion(inputBufferRef.current);
          }
        } else if (ch === "\r" || ch === "\n") {
          // Enter
          clearGhost();
          historyIndexRef.current = -1; // reset history navigation
          redrawInputLine(term); // remove ghost text from terminal buffer
          const input = inputBufferRef.current.trim();
          term.writeln("");

          if (input.length > 0) {
            actions.sendMessage(input);
          } else {
            term.write(PROMPT);
          }

          inputBufferRef.current = "";
          cursorPosRef.current = 0;
        } else if (ch === "\x7f" || ch === "\b") {
          // Backspace
          clearGhost();
          if (cursorPosRef.current > 0) {
            const buf = inputBufferRef.current;
            inputBufferRef.current =
              buf.slice(0, cursorPosRef.current - 1) + buf.slice(cursorPosRef.current);
            cursorPosRef.current--;
            redrawInputLine(term);
            scheduleCompletion(inputBufferRef.current);
          }
        } else if (ch === "\x12") {
          // Ctrl+R — enter reverse history search
          if (!historyFetchedRef.current) {
            actions.requestHistory();
            historyFetchedRef.current = true;
          }
          clearGhost();
          savedInputRef.current = inputBufferRef.current;
          searchModeRef.current = true;
          searchQueryRef.current = "";
          searchMatchesRef.current = historyRef.current;
          searchMatchIndexRef.current = 0;
          redrawSearchLine(term);
        } else if (ch === "\x03") {
          // Ctrl+C
          clearGhost();
          term.writeln("^C");
          inputBufferRef.current = "";
          cursorPosRef.current = 0;
          term.write(PROMPT);
        } else if (ch === "\x0c") {
          // Ctrl+L — clear terminal
          term.clear();
          redrawInputLine(term);
        } else if (ch === "\x15") {
          // Ctrl+U — clear line
          clearGhost();
          inputBufferRef.current = "";
          cursorPosRef.current = 0;
          redrawInputLine(term);
        } else if (ch >= " ") {
          // Printable character
          clearGhost();
          const buf = inputBufferRef.current;
          inputBufferRef.current =
            buf.slice(0, cursorPosRef.current) + ch + buf.slice(cursorPosRef.current);
          cursorPosRef.current++;

          if (cursorPosRef.current === inputBufferRef.current.length) {
            term.write(ch);
          } else {
            redrawInputLine(term);
          }
          scheduleCompletion(inputBufferRef.current);
        }
      }
    },
    [actions, state.completionCandidates, state.completionPrefix, clearGhost, redrawInputLine, redrawSearchLine, exitSearchMode, updateSearchMatches, scheduleCompletion]
  );

  // When completion candidates arrive, show the first one as ghost text.
  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;
    if (searchModeRef.current) return; // don't show ghost in search mode
    if (
      state.completionCandidates.length > 0 &&
      state.completionPrefix === inputBufferRef.current
    ) {
      ghostCandidateIndexRef.current = 0;
      const candidate = state.completionCandidates[0];
      ghostSuffixRef.current = candidate.slice(state.completionPrefix.length);
      redrawInputLine(term);
    } else {
      if (ghostSuffixRef.current) {
        clearGhost();
        redrawInputLine(term);
      }
    }
  }, [state.completionCandidates, state.completionPrefix, clearGhost, redrawInputLine]);

  // Sync history entries from state to ref (for use in search mode callbacks).
  useEffect(() => {
    historyRef.current = state.historyEntries;
  }, [state.historyEntries]);

  // Request history when connected (pre-fetch for Ctrl+R).
  useEffect(() => {
    if (state.connected && !historyFetchedRef.current) {
      actions.requestHistory();
      historyFetchedRef.current = true;
    }
  }, [state.connected, actions]);

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", background: "#1a1b26" }}>
      <div
        style={{
          display: "flex",
          alignItems: "center",
          justifyContent: "space-between",
          padding: "8px 16px",
          borderBottom: "1px solid #292e42",
          background: "#1a1b26",
        }}
      >
        <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
          <span style={{ fontWeight: 600, color: "#c0caf5", fontSize: 14 }}>
            AI Assistant
          </span>
          <span
            style={{
              width: 8,
              height: 8,
              borderRadius: "50%",
              background: state.connected ? "#9ece6a" : "#f7768e",
              display: "inline-block",
            }}
          />
          {state.sessionId && (
            <span style={{ color: "#565f89", fontSize: 12 }}>
              {state.sessionId}
            </span>
          )}
        </div>
        <div style={{ display: "flex", gap: 8 }}>
          <button
            className="btn"
            onClick={actions.newSession}
            style={{ fontSize: 12, padding: "2px 10px" }}
            title="Start a new session"
          >
            New Session
          </button>
        </div>
      </div>
      <div
        ref={termRef}
        style={{ flex: 1, padding: "4px 0" }}
      />
    </div>
  );
}

// -- Helpers --

function writeWrapped(term: Terminal, prefix: string, text: string): void {
  const lines = text.split("\n");
  for (let i = 0; i < lines.length; i++) {
    if (i === 0) {
      term.write(prefix + lines[i]);
    } else {
      term.writeln("");
      term.write("  " + lines[i]);
    }
  }
}
