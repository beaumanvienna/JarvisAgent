import React, { useCallback, useEffect, useMemo, useState } from "react";
import WorkflowEditorView, { type WorkflowPersistEvent } from "./editor/WorkflowEditorView";
import WorkflowListView, { type WorkflowListItem } from "./views/WorkflowListView";
import type { JcwfFile } from "./jcwf/types";

type RouteKey = "workflows" | "editor";

export default function App(): JSX.Element
{
  const [route, setRoute] = useState<RouteKey>("workflows");
  const [selectedWorkflow, setSelectedWorkflow] = useState<WorkflowListItem | null>(null);
  const [editorDirty, setEditorDirty] = useState<boolean>(false);
  const [workflowListRefreshToken, setWorkflowListRefreshToken] = useState<number>(0);
  const [initialJcwf, setInitialJcwf] = useState<JcwfFile | null>(null);

  const confirmLoseChanges = useCallback((): boolean => {
    if (!editorDirty)
    {
      return true;
    }
    return window.confirm("You have unsaved changes. Discard them?");
  }, [editorDirty]);

  const navigate = useCallback((nextRoute: RouteKey): void => {
    if (route === "editor" && nextRoute !== "editor")
    {
      if (!confirmLoseChanges())
      {
        return;
      }
    }
    setRoute(nextRoute);
  }, [route, confirmLoseChanges]);

  useEffect(() => {
    if (!editorDirty)
    {
      return;
    }

    const onBeforeUnload = (event: BeforeUnloadEvent): void => {
      // Standard browser behavior: setting returnValue triggers a confirm dialog.
      event.preventDefault();
      event.returnValue = "";
    };

    window.addEventListener("beforeunload", onBeforeUnload);
    return () => {
      window.removeEventListener("beforeunload", onBeforeUnload);
    };
  }, [editorDirty]);

  const onOpenWorkflow = useCallback((workflow: WorkflowListItem) => {
    if (route === "editor" && !confirmLoseChanges())
    {
      return;
    }
    setSelectedWorkflow(workflow);
    setRoute("editor");
  }, [route, confirmLoseChanges]);

  const onCreateNew = useCallback(() => {
    if (route === "editor" && !confirmLoseChanges())
    {
      return;
    }
    setSelectedWorkflow(null);
    setInitialJcwf(null);
    setRoute("editor");
  }, [route, confirmLoseChanges]);

  const onCreateFromTemplate = useCallback((workflowId: string, jcwf: JcwfFile) => {
    if (route === "editor" && !confirmLoseChanges())
    {
      return;
    }
    // Set the intended workflow ID so Save knows what to create
    setSelectedWorkflow({ id: workflowId });
    setInitialJcwf(jcwf);
    setRoute("editor");
  }, [route, confirmLoseChanges]);

  const onWorkflowCreated = useCallback((workflowId: string) => {
    setSelectedWorkflow({ id: workflowId });
    setInitialJcwf(null);
  }, []);

  const onWorkflowPersisted = useCallback((event: WorkflowPersistEvent) => {
    // Any successful create/save-as should refresh the list when the user returns.
    if (event.kind === "create" || event.kind === "saveAs")
    {
      setWorkflowListRefreshToken((v) => v + 1);
    }
  }, []);

  const content = useMemo(() => {
    if (route === "editor")
    {
      return (
        <WorkflowEditorView
          workflowId={selectedWorkflow?.id ?? null}
          initialJcwf={initialJcwf}
          onWorkflowCreated={onWorkflowCreated}
          onWorkflowPersisted={onWorkflowPersisted}
          onDirtyStateChange={setEditorDirty}
          onNavigateBack={() => {
            setWorkflowListRefreshToken((v) => v + 1);
            navigate("workflows");
          }}
        />
      );
    }

    return (
      <WorkflowListView
        refreshToken={workflowListRefreshToken}
        onOpenWorkflow={onOpenWorkflow}
        onCreateNew={onCreateNew}
        onCreateFromTemplate={onCreateFromTemplate}
      />
    );
  }, [route, selectedWorkflow, initialJcwf, onWorkflowCreated, onWorkflowPersisted, navigate, workflowListRefreshToken, onOpenWorkflow, onCreateNew, onCreateFromTemplate]);

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
            onClick={() => { navigate("workflows"); }}
            type="button"
          >
            Workflows
          </button>

          <button
            className={`btn ${route === "editor" ? "btnActive" : ""}`}
            onClick={() => { navigate("editor"); }}
            type="button"
            disabled={selectedWorkflow === null && route !== "editor"}
            title={selectedWorkflow === null && route !== "editor" ? "Select a workflow first." : undefined}
          >
            Editor{editorDirty ? "*" : ""}
          </button>
        </nav>
      </header>

      <main className="main">{content}</main>
    </div>
  );
}
