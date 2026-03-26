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

export default function AssistantView(): JSX.Element {
  const [state, actions] = useAssistantWebSocket();
  const termRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<Terminal | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const inputBufferRef = useRef<string>("");
  const cursorPosRef = useRef<number>(0);
  const renderedTurnsRef = useRef<number>(0);
  const thinkingLineRef = useRef<boolean>(false);

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
      `${DIM}Type /help for available commands.${RESET}`
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

  // Redraw the current input line (prompt + buffer) with cursor repositioned.
  const redrawInputLine = useCallback((term: Terminal) => {
    // Move to start of line, clear it, write prompt + buffer, reposition cursor.
    const buf = inputBufferRef.current;
    const pos = cursorPosRef.current;
    term.write("\r\x1b[2K" + PROMPT + buf);
    // Move cursor back from end to correct position.
    const moveBack = buf.length - pos;
    if (moveBack > 0) {
      term.write(`\x1b[${moveBack}D`);
    }
  }, []);

  const handleTerminalInput = useCallback(
    (data: string) => {
      const term = xtermRef.current;
      if (!term) return;

      // xterm.js delivers escape sequences as multi-char strings.
      // Process them as a single unit.
      let i = 0;
      while (i < data.length) {
        // Check for escape sequences (\x1b[...)
        if (data[i] === "\x1b" && i + 2 < data.length && data[i + 1] === "[") {
          const code = data[i + 2];
          if (code === "D") {
            // Left arrow
            if (cursorPosRef.current > 0) {
              cursorPosRef.current--;
              term.write("\x1b[D");
            }
          } else if (code === "C") {
            // Right arrow
            if (cursorPosRef.current < inputBufferRef.current.length) {
              cursorPosRef.current++;
              term.write("\x1b[C");
            }
          }
          i += 3;
          continue;
        }

        const ch = data[i];
        i++;

        if (ch === "\r" || ch === "\n") {
          // Enter
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
          if (cursorPosRef.current > 0) {
            const buf = inputBufferRef.current;
            inputBufferRef.current =
              buf.slice(0, cursorPosRef.current - 1) + buf.slice(cursorPosRef.current);
            cursorPosRef.current--;
            redrawInputLine(term);
          }
        } else if (ch === "\x03") {
          // Ctrl+C
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
          inputBufferRef.current = "";
          cursorPosRef.current = 0;
          redrawInputLine(term);
        } else if (ch >= " ") {
          // Printable character
          const buf = inputBufferRef.current;
          inputBufferRef.current =
            buf.slice(0, cursorPosRef.current) + ch + buf.slice(cursorPosRef.current);
          cursorPosRef.current++;

          if (cursorPosRef.current === inputBufferRef.current.length) {
            // Appending at end — just write the character
            term.write(ch);
          } else {
            // Inserting in middle — redraw
            redrawInputLine(term);
          }
        }
      }
    },
    [actions, redrawInputLine]
  );

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
