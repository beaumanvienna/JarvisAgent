import React, { useCallback, useMemo, useState } from "react";
import WorkflowEditorView from "./editor/WorkflowEditorView";
import WorkflowListView, { type WorkflowListItem } from "./views/WorkflowListView";

type RouteKey = "workflows" | "editor";

export default function App(): JSX.Element
{
  const [route, setRoute] = useState<RouteKey>("workflows");
  const [selectedWorkflow, setSelectedWorkflow] = useState<WorkflowListItem | null>(null);

  const onOpenWorkflow = useCallback((workflow: WorkflowListItem) => {
    setSelectedWorkflow(workflow);
    setRoute("editor");
  }, []);

  const content = useMemo(() => {
    if (route === "editor")
    {
      return (
        <WorkflowEditorView
          workflowId={selectedWorkflow?.id ?? null}
          onNavigateBack={() => { setRoute("workflows"); }}
        />
      );
    }

    return <WorkflowListView onOpenWorkflow={onOpenWorkflow} />;
  }, [route, selectedWorkflow, onOpenWorkflow]);

  return (
    <div className="appShell">
      <header className="topBar">
        <div className="brand">
          <div className="brandTitle">JarvisAgent</div>
          <div className="brandSub">Workflow Editor</div>
        </div>

        <nav className="navButtons">
          <button
            className={`btn ${route === "workflows" ? "btnActive" : ""}`}
            onClick={() => { setRoute("workflows"); }}
            type="button"
          >
            Workflows
          </button>

          <button
            className={`btn ${route === "editor" ? "btnActive" : ""}`}
            onClick={() => { setRoute("editor"); }}
            type="button"
            disabled={selectedWorkflow === null}
            title={selectedWorkflow === null ? "Select a workflow first." : undefined}
          >
            Editor
          </button>
        </nav>
      </header>

      <main className="main">{content}</main>
    </div>
  );
}
