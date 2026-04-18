import React, { useCallback, useEffect, useMemo, useState } from "react";
import WorkflowEditorView, { type WorkflowPersistEvent } from "./editor/WorkflowEditorView";
import WorkflowListView, { type WorkflowListItem } from "./views/WorkflowListView";
import AssistantView from "./views/AssistantView";
import SettingsModal, { type SettingsTab } from "./components/SettingsModal";
import MasterPasswordDialog from "./components/MasterPasswordDialog";
import StatusLeds from "./components/StatusLeds";
import { useStatusWebSocket } from "./hooks/useStatusWebSocket";
import { getKeysStatus, type KeysStatusResponse } from "./api/keys";
import { listAiInterfaces } from "./api/aiInterfaces";
import { whoami } from "./api/auth";
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

type RouteKey = "workflows" | "editor" | "assistant";

export default function App(): JSX.Element
{
  const [route, setRoute] = useState<RouteKey>("workflows");
  const [selectedWorkflow, setSelectedWorkflow] = useState<WorkflowListItem | null>(null);
  const [editorDirty, setEditorDirty] = useState<boolean>(false);
  const [workflowListRefreshToken, setWorkflowListRefreshToken] = useState<number>(0);
  const [initialJcwf, setInitialJcwf] = useState<JcwfFile | null>(null);
  const [settings, setSettings] = useState<EditorSettings>(loadSettings);
  const [showSettingsModal, setShowSettingsModal] = useState<boolean>(false);
  const [initialSettingsTab, setInitialSettingsTab] = useState<SettingsTab>("general");
  const [keysStatus, setKeysStatus] = useState<KeysStatusResponse | null>(null);
  const [masterPassword, setMasterPassword] = useState<string | null>(null);
  const [aiManagerDirty, setAiManagerDirty] = useState<boolean>(false);
  const [keysDirty, setKeysDirty] = useState<boolean>(false);
  const [connectionsDirty, setConnectionsDirty] = useState<boolean>(false);
  const [hasAiProvider, setHasAiProvider] = useState<boolean>(true);
  // Default to "admin" so Studio (which has no auth gate) gets full UI.
  // whoami overrides this when Engine + MCP-key auth reports a real role.
  const [role, setRole] = useState<"admin" | "operator" | "viewer">("admin");

  const openSettings = useCallback((tab: SettingsTab = "general") => {
    setInitialSettingsTab(tab);
    setShowSettingsModal(true);
  }, []);
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
    whoami().then((resp) => {
      if (resp?.ok && resp.role) setRole(resp.role);
    });
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
    setRoute(nextRoute);
  }, [route, confirmLoseChanges]);

  // Re-check AI provider availability whenever the Settings modal closes —
  // the user may have added or removed an interface on the AI Interfaces tab.
  useEffect(() => {
    if (showSettingsModal) return;
    listAiInterfaces()
      .then((resp) => setHasAiProvider(resp.ok && resp.api_index >= 0 && resp.interfaces.length > 0))
      .catch(() => {});
  }, [showSettingsModal]);

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

    return (
      <WorkflowListView
        refreshToken={workflowListRefreshToken}
        onOpenWorkflow={onOpenWorkflow}
        onCreateNew={onCreateNew}
        onCreateFromTemplate={onCreateFromTemplate}
      />
    );
  }, [route, selectedWorkflow, initialJcwf, onWorkflowCreated, onWorkflowPersisted, workflowListRefreshToken, onOpenWorkflow, onCreateNew, onCreateFromTemplate, settings.hideTierDWarnings]);

  return (
    <div className="appShell">
      <header className="topBar">
        <button
          className="btn settingsBtn"
          type="button"
          onClick={() => openSettings("general")}
          title={
            aiManagerDirty || keysDirty || connectionsDirty
              ? "Settings (unsaved changes)"
              : "Settings"
          }
          style={{ marginRight: 12, fontSize: 18, padding: "4px 10px" }}
        >
          {aiManagerDirty || keysDirty || connectionsDirty ? "⚙*" : "⚙"}
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
            className={`btn ${route === "assistant" ? "btnActive" : ""}`}
            onClick={() => { navigate("assistant"); }}
            type="button"
            disabled={!hasAiProvider && route !== "assistant"}
            title={!hasAiProvider && route !== "assistant" ? "No AI provider configured. Open Settings > AI Interfaces." : undefined}
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
          initialTab={initialSettingsTab}
          masterPassword={masterPassword}
          role={role}
          onAiManagerDirty={setAiManagerDirty}
          onKeysDirty={setKeysDirty}
          onConnectionsDirty={setConnectionsDirty}
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
