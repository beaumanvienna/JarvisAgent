import React, { useMemo, useState } from "react";
import WorkflowEditorView from "./editor/WorkflowEditorView";

type RouteKey = "dashboard" | "editor";

export default function App(): JSX.Element {
  const [route, setRoute] = useState<RouteKey>("editor");

  const content = useMemo(() => {
    if (route === "dashboard") {
      return (
        <div className="panel">
          <h2>Dashboard (legacy placeholder)</h2>
          <p className="muted">
            This is a placeholder route so we can later migrate the existing
            <code> web/index.html</code> dashboard into React incrementally.
          </p>
          <div className="card">
            <div className="small">Next</div>
            <div>
              Add API client + workflow list view, then wire to Crow endpoints.
            </div>
          </div>
        </div>
      );
    }

    return <WorkflowEditorView />;
  }, [route]);

  return (
    <div className="appShell">
      <header className="topBar">
        <div className="brand">
          <div className="brandTitle">JarvisAgent</div>
          <div className="brandSub">Workflow Editor (React Flow)</div>
        </div>

        <nav className="navButtons">
          <button
            className={`btn ${route === "dashboard" ? "btnActive" : ""}`}
            onClick={() => { setRoute("dashboard"); }}
            type="button"
          >
            Dashboard
          </button>
          <button
            className={`btn ${route === "editor" ? "btnActive" : ""}`}
            onClick={() => { setRoute("editor"); }}
            type="button"
          >
            Workflow Editor
          </button>
        </nav>
      </header>

      <main className="main">{content}</main>
    </div>
  );
}
