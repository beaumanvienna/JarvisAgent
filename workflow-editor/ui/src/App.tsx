import React, { useCallback, useEffect, useMemo, useState } from "react";
import WorkflowEditorView, { type WorkflowPersistEvent } from "./editor/WorkflowEditorView";
import WorkflowListView, { type WorkflowListItem } from "./views/WorkflowListView";
import ProvidersSettingsView from "./views/ProvidersSettingsView";
import AiManagerView from "./views/AiManagerView";
import AssistantView from "./views/AssistantView";
import SettingsModal from "./components/SettingsModal";
import MasterPasswordDialog from "./components/MasterPasswordDialog";
import StatusLeds from "./components/StatusLeds";
import { useStatusWebSocket } from "./hooks/useStatusWebSocket";
import { getKeysStatus, type KeysStatusResponse } from "./api/keys";
import { listAiInterfaces } from "./api/aiInterfaces";
import type { JcwfFile } from "./jcwf/types";

export type EditorSettings = {
  hideTierDWarnings: boolean;
};

const DEFAULT_SETTINGS: EditorSettings = {
  hideTierDWarnings: false,
};

function loadSettings(): EditorSettings {
  try {
    const stored = localStorage.getItem("jarvis-editor-settings");
    if (stored) {
      return { ...DEFAULT_SETTINGS, ...JSON.parse(stored) };
    }
  } catch {
    // ignore
  }
  return DEFAULT_SETTINGS;
}

function saveSettings(settings: EditorSettings): void {
  try {
    localStorage.setItem("jarvis-editor-settings", JSON.stringify(settings));
  } catch {
    // ignore
  }
}

type RouteKey = "workflows" | "editor" | "ai-manager" | "settings" | "assistant";

export default function App(): JSX.Element
{
  const [route, setRoute] = useState<RouteKey>("workflows");
  const [selectedWorkflow, setSelectedWorkflow] = useState<WorkflowListItem | null>(null);
  const [editorDirty, setEditorDirty] = useState<boolean>(false);
  const [workflowListRefreshToken, setWorkflowListRefreshToken] = useState<number>(0);
  const [initialJcwf, setInitialJcwf] = useState<JcwfFile | null>(null);
  const [settings, setSettings] = useState<EditorSettings>(loadSettings);
  const [showSettingsModal, setShowSettingsModal] = useState<boolean>(false);
  const [keysStatus, setKeysStatus] = useState<KeysStatusResponse | null>(null);
  const [masterPassword, setMasterPassword] = useState<string | null>(null);
  const [aiManagerDirty, setAiManagerDirty] = useState<boolean>(false);
  const [keysDirty, setKeysDirty] = useState<boolean>(false);
  const [hasAiProvider, setHasAiProvider] = useState<boolean>(true);
  const statusWs = useStatusWebSocket();

  // Check master password / keys status on mount
  useEffect(() => {
    getKeysStatus()
      .then((status) => setKeysStatus(status))
      .catch(() => {
        // Server not reachable — don't block the UI
      });
    listAiInterfaces()
      .then((resp) => setHasAiProvider(resp.ok && resp.api_index >= 0 && resp.interfaces.length > 0))
      .catch(() => setHasAiProvider(false));
  }, []);

  const onSettingsChange = useCallback((newSettings: EditorSettings) => {
    setSettings(newSettings);
    saveSettings(newSettings);
  }, []);

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
    if (route === "ai-manager" && nextRoute !== "ai-manager")
    {
      listAiInterfaces()
        .then((resp) => setHasAiProvider(resp.ok && resp.api_index >= 0 && resp.interfaces.length > 0))
        .catch(() => {});
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
          hideTierDWarnings={settings.hideTierDWarnings}
        />
      );
    }

    if (route === "assistant")
    {
      return <AssistantView />;
    }

    if (route === "ai-manager")
    {
      return <AiManagerView onDirtyStateChange={setAiManagerDirty} />;
    }

    if (route === "settings")
    {
      return <ProvidersSettingsView appMasterPassword={masterPassword} onDirtyStateChange={setKeysDirty} />;
    }

    return (
      <WorkflowListView
        refreshToken={workflowListRefreshToken}
        onOpenWorkflow={onOpenWorkflow}
        onCreateNew={onCreateNew}
        onCreateFromTemplate={onCreateFromTemplate}
      />
    );
  }, [route, selectedWorkflow, initialJcwf, onWorkflowCreated, onWorkflowPersisted, navigate, workflowListRefreshToken, onOpenWorkflow, onCreateNew, onCreateFromTemplate, settings.hideTierDWarnings]);

  return (
    <div className="appShell">
      <header className="topBar">
        <button
          className="btn settingsBtn"
          type="button"
          onClick={() => setShowSettingsModal(true)}
          title="Settings"
          style={{ marginRight: 12, fontSize: 18, padding: "4px 10px" }}
        >
          ⚙
        </button>

        <div className="brand">
          <div className="brandTitle">JarvisAgent</div>
          <div className="brandSub">Workflow Editor</div>
        </div>

        <StatusLeds status={statusWs} />

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

          <button
            className={`btn ${route === "ai-manager" ? "btnActive" : ""}`}
            onClick={() => { navigate("ai-manager"); }}
            type="button"
          >
            AI Manager{aiManagerDirty ? "*" : ""}
          </button>

          <button
            className={`btn ${route === "settings" ? "btnActive" : ""}`}
            onClick={() => { navigate("settings"); }}
            type="button"
          >
            AI Keys{keysDirty ? "*" : ""}
          </button>

          <button
            className={`btn ${route === "assistant" ? "btnActive" : ""}`}
            onClick={() => { navigate("assistant"); }}
            type="button"
            disabled={!hasAiProvider && route !== "assistant"}
            title={!hasAiProvider && route !== "assistant" ? "No AI provider configured. Add one in AI Manager." : undefined}
          >
            Assistant
          </button>

          <a
            className="btn"
            href="/?tab=log"
            style={{ textDecoration: "none" }}
          >
            Log
          </a>

          <a
            className="btn"
            href="/"
            style={{ textDecoration: "none" }}
          >
            Dashboard
          </a>
        </nav>
      </header>

      <main className="main">{content}</main>

      {showSettingsModal && (
        <SettingsModal
          settings={settings}
          onSettingsChange={onSettingsChange}
          onClose={() => setShowSettingsModal(false)}
        />
      )}

      {keysStatus &&
        (keysStatus.status === "no_password" || keysStatus.status === "wrong_password") && (
          <MasterPasswordDialog
            reason={keysStatus.status}
            onUnlocked={(pw) => {
              setMasterPassword(pw);
              setKeysStatus({ ...keysStatus, status: "ok" });
            }}
          />
        )}
    </div>
  );
}
