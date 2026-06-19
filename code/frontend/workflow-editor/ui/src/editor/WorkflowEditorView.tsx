import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import ReactFlow, {
  Background,
  ConnectionMode,
  Controls,
  MiniMap,
  SelectionMode,
  addEdge,
  applyNodeChanges,
  useEdgesState,
  useNodesState,
  useUpdateNodeInternals,
  type Connection,
  type Node,
  type Edge,
  type ReactFlowInstance,
} from "reactflow";
import "reactflow/dist/style.css";

import TaskNode from "./TaskNode";
import FilterNode from "./FilterNode";
import BranchNode from "./BranchNode";
import FileNode from "./FileNode";
import FilePickerDialog from "./FilePickerDialog";
import WorkflowTreeView from "./WorkflowTreeView";
import { FILE_INPUT_COLORS } from "./constants";
import FilterBuilderDialog from "./FilterBuilderDialog";
import SqlFilterBuilder from "./SqlFilterBuilder";
import QueueBindingEditor from "./QueueBindingEditor";
import FanoutBuilder from "./FanoutBuilder";
import StructuredOutputEditor from "./StructuredOutputEditor";
import FilePathInput from "./FilePathInput";
import { jcwfToGraph } from "./jcwfToGraph";
import { graphToJcwf } from "./graphToJcwf";
import { validateGraph } from "./validation";
import { suggestOutputName } from "./suggestOutputName";
import { deriveUpstreamOutputPaths, fileNodeInputPath, resolveTaskDirSegments } from "./pathSynthesis";
import { acceptsWiredInput, readsFileInputs } from "./taskCapabilities";
import { classifyEdge } from "./edgeClassify";
import type { EditorControlNode, EditorControlNodeData, EditorFileNode, EditorFilterNode, EditorFilterNodeData, EditorGraph, EditorNode, EditorTaskEdge, EditorTaskNode, EditorTaskNodeData, RuntimeTaskState } from "./types";
import type { JcwfControlNode, JcwfFile, JcwfFilter, JcwfFilterSource, JcwfQueueBinding, JcwfTask, JcwfTaskMode, JcwfTaskType, JcwfTrigger } from "../jcwf/types";
import {
  cancelRun,
  cleanWorkflow,
  createWorkflowWithId,
  checkFileExists,
  checkScript,
  fetchRunDetails,
  loadWorkflow,
  pauseRun,
  resumeRun,
  runWorkflow,
  saveWorkflow,
  stopRun,
  uploadWorkflowFile,
  validateDraft,
  type WorkflowValidationFinding,
} from "../api/workflows";
import CreateWorkflowModal from "../components/CreateWorkflowModal";
import VersionHistoryModal from "../components/VersionHistoryModal";
import { listAiInterfaces, type AiInterface } from "@shared/api/aiInterfaces";
import AiPromptArea from "./AiPromptArea";
import type { AiPromptAreaHandle } from "./AiPromptArea";

const nodeTypes = { task: TaskNode, filter: FilterNode, branch: BranchNode, file: FileNode };

export type WorkflowPersistEvent = {
  kind: "save" | "create" | "saveAs";
  workflowId: string;
};

type NodeSnapshot = Record<string, string>;

type WorkflowRunListItem = {
  runId: string;
  workflowId: string;
  state: string;
  startedAt?: string;
  completedAt?: string;
};

type WorkflowRunsSnapshotMessage = {
  type: string;
  runs?: unknown;
  activeRuns?: unknown;
};

export type RuntimeTaskSnapshot = {
  taskId: string;
  state: RuntimeTaskState;
  runId: string;
  attemptCount?: number;
  lastErrorMessage?: string;
  capturedStdout?: string;
  capturedStderr?: string;
};

type RuntimeTaskSnapshotById = Record<string, RuntimeTaskSnapshot>;

function buildWebSocketUrl(pathname: string): string
{
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}${pathname}`;
}

function normalizeRuntimeState(stateText: unknown): RuntimeTaskState
{
  if (typeof stateText !== "string")
  {
    return "unknown";
  }

  const normalized = stateText.trim().toLowerCase();
  if (normalized.includes("queue") || normalized.includes("pending") || normalized === "ready")
  {
    return "queued";
  }
  if (normalized.includes("run") || normalized.includes("waiting_external"))
  {
    return "running";
  }
  if (normalized === "skipped")
  {
    return "fresh";
  }
  if (normalized.includes("success") || normalized.includes("succeed") || normalized.includes("ok") || normalized.includes("done"))
  {
    return "success";
  }
  if (normalized.includes("fail") || normalized.includes("error"))
  {
    return "failed";
  }
  if (normalized.includes("cancel"))
  {
    return "cancelled";
  }
  return "unknown";
}

const NON_TERMINAL_RUNTIME_STATES = new Set<RuntimeTaskState>(["queued", "running", "fresh", "unknown"]);

// When a run has reached a terminal state, a per_item parent task (canvas node id `<id>`) can be left
// frozen at a non-terminal state (e.g. `running`) because the fan-out aborted on a child failure and
// the parent never transitioned — so its canvas node keeps pulsing until a reload (F-41). The child
// instances (`<id>#N`) carry the real outcome, so derive the parent's terminal state from them: any
// failed → failed, else any cancelled → cancelled, else all succeeded → success. Only non-terminal
// parents are touched, so a correctly-reported parent (the common case) is never overridden.
function reconcileTerminalParentStates(byId: RuntimeTaskSnapshotById): RuntimeTaskSnapshotById
{
  const childStatesByParent = new Map<string, RuntimeTaskState[]>();
  for (const [taskId, snap] of Object.entries(byId))
  {
    const hashIdx = taskId.indexOf("#");
    if (hashIdx > 0)
    {
      const parent = taskId.slice(0, hashIdx);
      const list = childStatesByParent.get(parent) ?? [];
      list.push(snap.state);
      childStatesByParent.set(parent, list);
    }
  }
  if (childStatesByParent.size === 0)
  {
    return byId;
  }

  const next: RuntimeTaskSnapshotById = { ...byId };
  let changed = false;
  for (const [parentId, snap] of Object.entries(byId))
  {
    if (!NON_TERMINAL_RUNTIME_STATES.has(snap.state))
    {
      continue;
    }
    const children = childStatesByParent.get(parentId);
    if (!children || children.length === 0)
    {
      continue;
    }
    const derived: RuntimeTaskState = children.some((s) => s === "failed") ? "failed"
      : children.some((s) => s === "cancelled") ? "cancelled"
      : children.every((s) => s === "success") ? "success"
      : snap.state;
    if (derived !== snap.state)
    {
      next[parentId] = { ...snap, state: derived };
      changed = true;
    }
  }
  return changed ? next : byId;
}

function computeGraphSignature(nodes: EditorTaskNode[], edges: EditorTaskEdge[], controlNodes?: EditorControlNode[]): string
{
  const signatureObject = {
    nodes: nodes
      .map((n) => ({
        id: n.id,
        task: n.data.task,
      }))
      .sort((a, b) => a.id.localeCompare(b.id)),
    controlNodes: (controlNodes ?? [])
      .map((n) => ({
        id: n.id,
        controlNode: n.data.controlNode,
      }))
      .sort((a, b) => a.id.localeCompare(b.id)),
    edges: edges
      .map((e) => ({
        id: e.id,
        source: e.source,
        target: e.target,
      }))
      .sort((a, b) => {
        const keyA = `${a.source}->${a.target}:${a.id}`;
        const keyB = `${b.source}->${b.target}:${b.id}`;
        return keyA.localeCompare(keyB);
      }),
  };

  return JSON.stringify(signatureObject);
}

function computeTaskSignature(task: JcwfTask): string
{
  return JSON.stringify(task);
}

function computeNodeSnapshot(nodes: EditorTaskNode[]): NodeSnapshot
{
  const snapshot: NodeSnapshot = {};
  for (const node of nodes)
  {
    snapshot[node.id] = computeTaskSignature(node.data.task);
  }
  return snapshot;
}

function buildDefaultTask(taskId: string, taskType: JcwfTaskType): JcwfTask
{
  const task: JcwfTask = {
    id: taskId,
    type: taskType,
    label: taskId,
    params: {},
  };

  if (taskType === "sub_workflow")
  {
    task.workflow_file = "";
  }

  return task;
}

// Rendered inside <ReactFlow> (so the ReactFlow context exists): re-measures node handles whenever
// the dataflow-slot signature changes, so a newly added input/output port becomes connectable
// immediately — without needing a save + reload to re-register the handle with ReactFlow.
function NodeInternalsUpdater(props: { signature: string; nodeIds: string[] }): null
{
  const updateNodeInternals = useUpdateNodeInternals();
  useEffect(() => {
    for (const id of props.nodeIds)
    {
      updateNodeInternals(id);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [props.signature]);
  return null;
}

function nodeTitleFromTask(task: JcwfTask): { title: string; subtitle?: string }
{
  return {
    title: task.label && task.label.length > 0 ? task.label : task.id,
    subtitle: task.type,
  };
}

function nextId(existing: Set<string>, base: string): string
{
  if (!existing.has(base))
  {
    return base;
  }

  let index = 2;
  while (existing.has(`${base}_${index}`))
  {
    index += 1;
  }
  return `${base}_${index}`;
}

export default function WorkflowEditorView(props: {
  workflowId: string | null;
  initialJcwf?: JcwfFile | null;
  onWorkflowCreated: (workflowId: string) => void;
  onWorkflowPersisted?: (event: WorkflowPersistEvent) => void;
  onDirtyStateChange?: (isDirty: boolean) => void;
  hideTierDWarnings?: boolean;
}): JSX.Element
{
  const [reactFlowInstance, setReactFlowInstance] = useState<ReactFlowInstance | null>(null);
  const [currentZoom, setCurrentZoom] = useState<number>(1);
  const [shiftHeld, setShiftHeld] = useState(false);

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => { if (e.key === "Shift") setShiftHeld(true); };
    const onKeyUp = (e: KeyboardEvent) => { if (e.key === "Shift") setShiftHeld(false); };
    window.addEventListener("keydown", onKeyDown);
    window.addEventListener("keyup", onKeyUp);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      window.removeEventListener("keyup", onKeyUp);
    };
  }, []);

  const [statusText, setStatusText] = useState<string>("");
  const [errorText, setErrorText] = useState<string | null>(null);
  const [backendErrors, setBackendErrors] = useState<WorkflowValidationFinding[]>([]);
  const [backendWarnings, setBackendWarnings] = useState<WorkflowValidationFinding[]>([]);
  const [backendInfos, setBackendInfos] = useState<WorkflowValidationFinding[]>([]);
  const [clientErrors, setClientErrors] = useState<{ taskId: string; message: string }[]>([]);
  const [clientInfos, setClientInfos] = useState<{ taskId: string; message: string }[]>([]);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [loadedWorkflowId, setLoadedWorkflowId] = useState<string | null>(null);
  const [workflowNavStack, setWorkflowNavStack] = useState<string[]>([]);
  const [lastSavedSignature, setLastSavedSignature] = useState<string>("");
  const [lastSavedNodeSnapshot, setLastSavedNodeSnapshot] = useState<NodeSnapshot>({});
  const [isDirty, setIsDirty] = useState<boolean>(false);
  const [manualStartEnabled, setManualStartEnabled] = useState<boolean>(true);
  const [triggers, setTriggers] = useState<JcwfTrigger[]>([]);
  const [wfLabel, setWfLabel] = useState<string>("");
  const [wfDoc, setWfDoc] = useState<string>("");
  const [wfBaseDirectory, setWfBaseDirectory] = useState<string>("");
  const [wfDefaultTimeoutMs, setWfDefaultTimeoutMs] = useState<string>("");
  const [wfDefaultAiProvider, setWfDefaultAiProvider] = useState<string>("");
  const [wfDefaultAiModel, setWfDefaultAiModel] = useState<string>("");
  const [showCreateModal, setShowCreateModal] = useState<boolean>(false);
  const [createModalMode, setCreateModalMode] = useState<"create" | "saveAs">("create");

  const [showFilterBuilder, setShowFilterBuilder] = useState<boolean>(false);
  const [editingFilter, setEditingFilter] = useState<JcwfFilter | null>(null);
  // The filter id at the moment the dialog opened — onFilterBuilderSave matches the node by THIS,
  // because the dialog may rename the id and matching on the new id would find no node (dropping the edit).
  const editingFilterOriginalIdRef = useRef<string | null>(null);
  const [showVersionHistory, setShowVersionHistory] = useState<boolean>(false);

  const loadedJcwfRef = useRef<JcwfFile | null>(null);
  const webSocketRef = useRef<WebSocket | null>(null);
  const aiPromptAreaRef = useRef<AiPromptAreaHandle>(null);
  const [isWebSocketConnected, setIsWebSocketConnected] = useState<boolean>(false);
  const [activeRuns, setActiveRuns] = useState<WorkflowRunListItem[]>([]);
  const [selectedRunId, setSelectedRunId] = useState<string | null>(null);
  const selectedRunIdRef = useRef<string | null>(null);
  const [runtimeTasksById, setRuntimeTasksById] = useState<RuntimeTaskSnapshotById>({});
  const [pendingRunId, setPendingRunId] = useState<string | null>(null);
  const [lastRunResult, setLastRunResult] = useState<{ runId: string; state: string } | null>(null);
  const pendingRunSeenRef = useRef<boolean>(false);
  const pendingRunLastStateRef = useRef<string>("running");
  const prevActiveRunIdsRef = useRef<Set<string>>(new Set());

  const runtimeTasksByIdRef = useRef<RuntimeTaskSnapshotById>({});
  useEffect(() => {
    runtimeTasksByIdRef.current = runtimeTasksById;
  }, [runtimeTasksById]);

  // Async script existence check cache (command path → check result).
  const scriptCheckCacheRef = useRef<Record<string, { exists: boolean; executable: boolean }>>({})
  // Async file_input existence check cache (file path → exists).
  const fileCheckCacheRef = useRef<Record<string, { exists: boolean }>>({});
  const [scriptCheckTick, setScriptCheckTick] = useState(0);
  useEffect(() => {
    const timer = setInterval(() => {
      scriptCheckCacheRef.current = {};
      fileCheckCacheRef.current = {};
      setScriptCheckTick((t) => t + 1);
    }, 5000);
    return () => clearInterval(timer);
  }, []);

  // Undo/redo history (two-stack model)
  type HistoryEntry = { nodes: EditorTaskNode[]; edges: EditorTaskEdge[] };
  const [undoStack, setUndoStack] = useState<HistoryEntry[]>([]);
  const [redoStack, setRedoStack] = useState<HistoryEntry[]>([]);
  const isUndoRedoRef = useRef<boolean>(false);

  const initialGraph: EditorGraph = useMemo(() => ({ nodes: [], edges: [] }), []);

  type AnyNodeData = EditorTaskNodeData | EditorFilterNodeData | EditorControlNodeData;
  const [nodes, setNodes] = useNodesState<AnyNodeData>(
    initialGraph.nodes as Node<AnyNodeData>[]
  );

  const [edges, setEdges, onEdgesChange] = useEdgesState<EditorTaskEdge>(
    initialGraph.edges as Edge[]
  );

  // Signature of every task's connectable-port set; drives NodeInternalsUpdater so a just-added
  // port is registered with ReactFlow immediately (connectable without a save+reload). Must cover
  // EVERY port-count source: dataflow slot names (in:/out:) AND the file_inputs / depends_on /
  // file_outputs arrays — the dephandle-N / fileoutput-N ports are derived from those, so an edge
  // that auto-populates a new file_inputs entry would otherwise retarget onto an unregistered
  // handle and render as a dangling edge.
  const taskNodeIds = useMemo(
    () => (nodes as EditorNode[]).filter((n) => n.type === "task").map((n) => n.id),
    [nodes],
  );
  const slotsSignature = useMemo(
    () => (nodes as EditorNode[])
      .filter((n) => n.type === "task")
      .map((n) => {
        const t = (n as EditorTaskNode).data.task;
        const fi = Array.isArray(t.file_inputs) ? (t.file_inputs as unknown[]).length : 0;
        const dep = Array.isArray(t.depends_on) ? (t.depends_on as unknown[]).length : 0;
        const fo = Array.isArray(t.file_outputs) ? (t.file_outputs as unknown[]).length : 0;
        return `${n.id}|${Object.keys((t.inputs as object) ?? {}).join(",")}|${Object.keys((t.outputs as object) ?? {}).join(",")}|fi${fi}|dep${dep}|fo${fo}`;
      })
      .join(";"),
    [nodes],
  );

  const onNodesChangeRaw = useCallback((changes: unknown) => {
    setNodes((current) => applyNodeChanges(changes as never, current));
  }, [setNodes]);


  // Track previous state for automatic history with debouncing
  const prevStateRef = useRef<HistoryEntry | null>(null);
  const nodesJsonRef = useRef<string>("");
  const edgesJsonRef = useRef<string>("");
  const debounceTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pendingPrevStateRef = useRef<HistoryEntry | null>(null);
  const isDraggingRef = useRef<boolean>(false);
  const skipNextEffectRef = useRef<boolean>(false);

  // Helper to push to undo stack
  const pushToUndoStack = useCallback((state: HistoryEntry) => {
    setUndoStack((prev) => [...prev, state].slice(-50));
    setRedoStack([]); // Clear redo stack on new action
  }, []);

  // Effect to automatically track state changes and push to history (debounced)
  useEffect(() => {
    if (isUndoRedoRef.current) return;
    if (isDraggingRef.current) return; // Don't track during drag
    if (skipNextEffectRef.current)
    {
      skipNextEffectRef.current = false;
      // Update refs but don't push to history
      const nodesJson = JSON.stringify(nodes);
      const edgesJson = JSON.stringify(edges);
      nodesJsonRef.current = nodesJson;
      edgesJsonRef.current = edgesJson;
      prevStateRef.current = { nodes: JSON.parse(nodesJson), edges: JSON.parse(edgesJson) };
      return;
    }

    const nodesJson = JSON.stringify(nodes);
    const edgesJson = JSON.stringify(edges);

    // Skip if state hasn't actually changed
    if (nodesJson === nodesJsonRef.current && edgesJson === edgesJsonRef.current)
    {
      return;
    }

    // Capture the state BEFORE this change (only on first change in a batch)
    if (pendingPrevStateRef.current === null && prevStateRef.current !== null)
    {
      pendingPrevStateRef.current = prevStateRef.current;
    }

    // Update refs to current state immediately
    nodesJsonRef.current = nodesJson;
    edgesJsonRef.current = edgesJson;
    prevStateRef.current = {
      nodes: JSON.parse(nodesJson),
      edges: JSON.parse(edgesJson),
    };

    // Clear any pending debounce timer
    if (debounceTimerRef.current)
    {
      clearTimeout(debounceTimerRef.current);
    }

    // Debounce: push to history after 100ms of no changes
    debounceTimerRef.current = setTimeout(() => {
      if (pendingPrevStateRef.current !== null)
      {
        const stateToSave = pendingPrevStateRef.current;
        pushToUndoStack(stateToSave);
        pendingPrevStateRef.current = null;
      }
      debounceTimerRef.current = null;
    }, 100);

    return () => {
      if (debounceTimerRef.current)
      {
        clearTimeout(debounceTimerRef.current);
      }
    };
  }, [nodes, edges, pushToUndoStack]);

  // Simple pass-through for node changes
  const onNodesChange = useCallback((changes: unknown) => {
    onNodesChangeRaw(changes);
  }, [onNodesChangeRaw]);

  // Simple pass-through for edge changes
  const onEdgesChangeWithUndo = useCallback((changes: unknown) => {
    onEdgesChange(changes as never);
  }, [onEdgesChange]);

  const onNodeDragStart = useCallback(() => {
    // Capture state before drag starts (read from refs to avoid stale closure)
    isDraggingRef.current = true;
    pendingPrevStateRef.current = {
      nodes: JSON.parse(nodesJsonRef.current || "[]"),
      edges: JSON.parse(edgesJsonRef.current || "[]"),
    };
  }, []);

  const onNodeDragStop = useCallback((_event: unknown, node: Node) => {
    setNodes((current) => {
      const next = (current as Node<EditorTaskNodeData>[]).map((n) => {
        if (n.id !== node.id)
        {
          return n;
        }
        return {
          ...n,
          position: node.position,
        };
      });
      return next;
    });
    // End drag and push to history
    isDraggingRef.current = false;
    skipNextEffectRef.current = true; // Prevent effect from also pushing
    if (pendingPrevStateRef.current !== null)
    {
      const stateToSave = pendingPrevStateRef.current;
      pushToUndoStack(stateToSave);
      pendingPrevStateRef.current = null;
    }
  }, [setNodes, pushToUndoStack]);

  const undo = useCallback(() => {
    if (undoStack.length === 0)
    {
      return;
    }

    // Pop from undo stack
    const entry = undoStack[undoStack.length - 1];

    // Save current state for redo
    const currentState: HistoryEntry = {
      nodes: JSON.parse(nodesJsonRef.current || "[]"),
      edges: JSON.parse(edgesJsonRef.current || "[]"),
    };

    isUndoRedoRef.current = true;
    setUndoStack((prev) => prev.slice(0, -1)); // Pop
    setRedoStack((prev) => [...prev, currentState]); // Push current to redo
    setNodes(entry.nodes as Node<EditorTaskNodeData>[]);
    setEdges(entry.edges);
    // Update refs to match restored state
    nodesJsonRef.current = JSON.stringify(entry.nodes);
    edgesJsonRef.current = JSON.stringify(entry.edges);
    prevStateRef.current = { nodes: entry.nodes, edges: entry.edges };
    isUndoRedoRef.current = false;
    setStatusText("Undo.");
  }, [undoStack, setNodes, setEdges]);

  const redo = useCallback(() => {
    if (redoStack.length === 0)
    {
      return;
    }

    // Pop from redo stack
    const entry = redoStack[redoStack.length - 1];

    // Save current state for undo
    const currentState: HistoryEntry = {
      nodes: JSON.parse(nodesJsonRef.current || "[]"),
      edges: JSON.parse(edgesJsonRef.current || "[]"),
    };

    isUndoRedoRef.current = true;
    setRedoStack((prev) => prev.slice(0, -1)); // Pop
    setUndoStack((prev) => [...prev, currentState]); // Push current to undo
    setNodes(entry.nodes as Node<EditorTaskNodeData>[]);
    setEdges(entry.edges);
    // Update refs to match restored state
    nodesJsonRef.current = JSON.stringify(entry.nodes);
    edgesJsonRef.current = JSON.stringify(entry.edges);
    prevStateRef.current = { nodes: entry.nodes, edges: entry.edges };
    isUndoRedoRef.current = false;
    setStatusText("Redo.");
  }, [redoStack, setNodes, setEdges]);

  // Keyboard shortcuts for undo/redo
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      const isMac = navigator.platform.toUpperCase().indexOf("MAC") >= 0;
      const modifier = isMac ? e.metaKey : e.ctrlKey;
      if (modifier && e.key === "z" && !e.shiftKey)
      {
        e.preventDefault();
        undo();
      }
      else if (modifier && e.key === "z" && e.shiftKey)
      {
        e.preventDefault();
        redo();
      }
      else if (modifier && e.key === "y")
      {
        e.preventDefault();
        redo();
      }
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [undo, redo]);

  // While the user is editing text in a field, keep destructive / clipboard / undo keystrokes from
  // reaching the GRAPH-level keydown handlers — ReactFlow's deleteKeyCode (Backspace/Delete deletes
  // the selected node/edge), copy/paste, and undo/redo — so native text editing wins. Runs in the
  // CAPTURE phase on window, i.e. before any of those bubble/document-level listeners, and only
  // stops propagation (never preventDefault), so the input's own editing + React onChange still fire.
  // Without this, Backspace/Delete/Ctrl-V are swallowed and inspector path/text fields are uneditable.
  useEffect(() => {
    const guard = (e: KeyboardEvent) => {
      const el = document.activeElement as HTMLElement | null;
      const editable = !!el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.isContentEditable);
      if (!editable)
      {
        return;
      }
      const mod = e.ctrlKey || e.metaKey;
      if (e.key === "Backspace" || e.key === "Delete"
        || (mod && ["v", "V", "c", "C", "x", "X", "z", "Z", "y", "Y", "a", "A"].includes(e.key)))
      {
        e.stopPropagation();
      }
    };
    window.addEventListener("keydown", guard, true);
    return () => window.removeEventListener("keydown", guard, true);
  }, []);

  // Clipboard for copy/paste of selected task node(s). Declared here; the keydown
  // handler that uses it is registered after recomputeValidation exists (see below).
  const clipboardRef = useRef<EditorTaskNode[]>([]);

  const currentSignature = useMemo(() => {
    const graphSig = computeGraphSignature(nodes.filter((n): n is EditorTaskNode => n.type === "task") as EditorTaskNode[], edges as EditorTaskEdge[], nodes.filter((n): n is EditorControlNode => n.type === "branch") as EditorControlNode[]);
    const wfFieldsSig = JSON.stringify({
      wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel,
      manualStartEnabled, triggers,
    });
    return graphSig + "|" + wfFieldsSig;
  }, [nodes, edges, wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel, manualStartEnabled, triggers]);

  const resetToNewDraft = useCallback(() => {
    setNodes([]);
    setEdges([]);
    setSelectedNodeId(null);
    setLoadedWorkflowId(null);
    loadedJcwfRef.current = null;
    setBackendErrors([]);
    setBackendWarnings([]);
    setBackendInfos([]);
    setErrorText(null);

    const emptyGraphSig = computeGraphSignature([], []);
    const emptyWfFieldsSig = JSON.stringify({
      wfLabel: "", wfDoc: "", wfBaseDirectory: "",
      wfDefaultTimeoutMs: "", wfDefaultAiProvider: "", wfDefaultAiModel: "",
      manualStartEnabled: true, triggers: [],
    });
    setLastSavedSignature(emptyGraphSig + "|" + emptyWfFieldsSig);
    setLastSavedNodeSnapshot({});
    setIsDirty(false);
    if (props.onDirtyStateChange)
    {
      props.onDirtyStateChange(false);
    }

    setStatusText("New workflow draft.");
  }, [setNodes, setEdges, props.onDirtyStateChange]);

  // Dirty tracking.
  useEffect(() => {
    if (lastSavedSignature.length === 0)
    {
      // First mount.
      setLastSavedSignature(computeGraphSignature([], []));
      return;
    }

    const nextDirty = currentSignature !== lastSavedSignature;
    setIsDirty(nextDirty);
    if (props.onDirtyStateChange)
    {
      props.onDirtyStateChange(nextDirty);
    }
  }, [currentSignature, lastSavedSignature, props.onDirtyStateChange]);

  const recomputeValidation = useCallback((graph: EditorGraph, backendFindings?: {
    errors: WorkflowValidationFinding[];
    warnings: WorkflowValidationFinding[];
    infos?: WorkflowValidationFinding[];
  }) => {
    const validation = validateGraph(graph);

    // Collect client-side validation issues for sidebar display
    const clientErrorsList: { taskId: string; message: string }[] = [];
    const clientInfosList: { taskId: string; message: string }[] = [];
    for (const [taskId, errors] of validation.nodeErrorsById)
    {
      for (const msg of errors)
      {
        clientErrorsList.push({ taskId, message: msg });
      }
    }
    if (validation.nodeInfosById)
    {
      for (const [taskId, infos] of validation.nodeInfosById)
      {
        for (const msg of infos)
        {
          clientInfosList.push({ taskId, message: msg });
        }
      }
    }
    setClientErrors(clientErrorsList);
    setClientInfos(clientInfosList);

    // The backend sets a structured taskId on every task-scoped finding; graph-level findings
    // (cycles, sub-workflow depth, trigger/workflow-id issues) intentionally carry no taskId and
    // surface in the issues list rather than decorating a node.
    const backendErrorsByTaskId = new Map<string, string[]>();
    const sourceErrors = backendFindings ? backendFindings.errors : backendErrors;
    for (const finding of sourceErrors)
    {
      const taskId = finding.taskId;
      if (!taskId)
      {
        continue;
      }

      const existing = backendErrorsByTaskId.get(taskId) ?? [];
      existing.push(finding.message);
      backendErrorsByTaskId.set(taskId, existing);
    }

    const backendWarningsByTaskId = new Map<string, string[]>();
    const sourceWarnings = backendFindings ? backendFindings.warnings : backendWarnings;
    for (const finding of sourceWarnings)
    {
      const taskId = finding.taskId;
      if (!taskId)
      {
        continue;
      }

      const existing = backendWarningsByTaskId.get(taskId) ?? [];
      existing.push(finding.message);
      backendWarningsByTaskId.set(taskId, existing);
    }

    const backendInfosByTaskId = new Map<string, string[]>();
    const sourceInfos = backendFindings ? (backendFindings.infos ?? []) : backendInfos;
    for (const finding of sourceInfos)
    {
      const taskId = finding.taskId;
      if (!taskId)
      {
        continue;
      }

      const existing = backendInfosByTaskId.get(taskId) ?? [];
      existing.push(finding.message);
      backendInfosByTaskId.set(taskId, existing);
    }

    const nextNodes = graph.nodes.map((n) => {
      // Filter and branch nodes pass through without validation decorations
      if (n.type === "filter" || n.type === "branch")
      {
        return n;
      }

      const taskNode = n as EditorTaskNode;
      const clientErrors = validation.nodeErrorsById.get(n.id) ?? [];
      const serverErrors = backendErrorsByTaskId.get(n.id) ?? [];
      const mergedErrors = [...clientErrors, ...serverErrors];

      const clientWarnings = validation.nodeWarningsById ? (validation.nodeWarningsById.get(n.id) ?? []) : [];
      const serverWarnings = backendWarningsByTaskId.get(n.id) ?? [];
      // Script existence warnings from async check cache
      const scriptWarnings: string[] = [];
      if (taskNode.data.task?.type === "shell")
      {
        const params = (taskNode.data.task.params ?? {}) as Record<string, unknown>;
        const cmd = typeof params.command === "string" ? params.command : "";
        if (cmd.length > 0)
        {
          const cached = scriptCheckCacheRef.current[cmd];
          if (cached)
          {
            if (!cached.exists)
            {
              scriptWarnings.push(`Script '${cmd}' not found.`);
            }
            else if (!cached.executable)
            {
              scriptWarnings.push(`Script '${cmd}' is not executable (chmod +x).`);
            }
          }
        }
      }
      else if (taskNode.data.task?.type === "python")
      {
        const params = (taskNode.data.task.params ?? {}) as Record<string, unknown>;
        const mod = typeof params.module === "string" ? params.module : "";
        if (mod.startsWith("scripts.") || mod.startsWith("scripts/"))
        {
          const filePath = mod.replace(/\./g, "/") + ".py";
          const cached = scriptCheckCacheRef.current[filePath];
          if (cached && !cached.exists)
          {
            scriptWarnings.push(`Script '${filePath}' not found.`);
          }
        }
      }
      // File input existence warnings from async check cache
      const fileInputWarnings: string[] = [];
      const taskType = taskNode.data.task?.type;
      if (taskType === "python" || taskType === "shell")
      {
        const wd = typeof taskNode.data.task.working_directory === "string" ? taskNode.data.task.working_directory : "";
        const fileInputs = Array.isArray(taskNode.data.task.file_inputs) ? taskNode.data.task.file_inputs as string[] : [];
        for (const filePath of fileInputs)
        {
          // Skip template bindings like {{input[0]}}
          if (filePath.includes("{{")) continue;
          if (filePath.length === 0) continue;
          const cacheKey = fileInputCacheKey(filePath, wd);
          const cached = fileCheckCacheRef.current[cacheKey];
          if (cached && !cached.exists)
          {
            fileInputWarnings.push(`Input file '${filePath}' not found.`);
          }
        }
      }
      const mergedWarnings = [...clientWarnings, ...serverWarnings, ...scriptWarnings, ...fileInputWarnings];

      const clientInfos = validation.nodeInfosById ? (validation.nodeInfosById.get(n.id) ?? []) : [];
      const serverInfos = backendInfosByTaskId.get(n.id) ?? [];
      const mergedInfos = [...clientInfos, ...serverInfos];

      const savedTaskSignature = lastSavedNodeSnapshot[n.id];
      const nodeIsDirty = savedTaskSignature ? computeTaskSignature(taskNode.data.task) !== savedTaskSignature : true;
      const runtimeSnapshot = runtimeTasksByIdRef.current[n.id];


      return {
        ...taskNode,
        data: {
          ...taskNode.data,
          validationErrors: mergedErrors.length > 0 ? mergedErrors : undefined,
          validationWarnings: mergedWarnings.length > 0 ? mergedWarnings : undefined,
          validationInfos: mergedInfos.length > 0 ? mergedInfos : undefined,
          isDirty: nodeIsDirty ? true : undefined,
          hideTierDWarnings: props.hideTierDWarnings,
          runtimeState: runtimeSnapshot ? runtimeSnapshot.state : undefined,
          runtimeRunId: runtimeSnapshot ? runtimeSnapshot.runId : undefined,
          runtimeError: runtimeSnapshot?.lastErrorMessage,
          capturedStdout: runtimeSnapshot?.capturedStdout,
          capturedStderr: runtimeSnapshot?.capturedStderr,
        },
      };
    });

    setNodes(nextNodes);
    setEdges(graph.edges);
  }, [setNodes, setEdges, backendErrors, backendWarnings, backendInfos, lastSavedNodeSnapshot, props.hideTierDWarnings]);

  // Async script existence check — fires when shell/python commands change.
  const shellCommandsFingerprint = useMemo(() => {
    const commands: string[] = [];
    for (const node of nodes)
    {
      if (node.type !== "task") continue;
      const taskData = (node as EditorTaskNode).data;
      const taskType = taskData.task?.type;
      const params = (taskData.task?.params ?? {}) as Record<string, unknown>;
      if (taskType === "shell")
      {
        const cmd = typeof params.command === "string" ? params.command : "";
        if (cmd.startsWith("scripts/"))
        {
          commands.push(`${node.id}:${cmd}`);
        }
      }
      else if (taskType === "python")
      {
        const mod = typeof params.module === "string" ? params.module : "";
        if (mod.startsWith("scripts.") || mod.startsWith("scripts/"))
        {
          const filePath = mod.replace(/\./g, "/") + ".py";
          commands.push(`${node.id}:${filePath}`);
        }
      }
    }
    return commands.sort().join("|");
  }, [nodes]);

  const recomputeValidationRef = useRef(recomputeValidation);
  useEffect(() => { recomputeValidationRef.current = recomputeValidation; }, [recomputeValidation]);

  useEffect(() => {
    if (shellCommandsFingerprint.length === 0) return;

    // Parse fingerprint into command list
    const entries = shellCommandsFingerprint.split("|").map((s) => {
      const idx = s.indexOf(":");
      return { nodeId: s.slice(0, idx), command: s.slice(idx + 1) };
    });

    // Only check commands not already in cache
    const uncached = [...new Set(entries.map((e) => e.command))].filter(
      (cmd) => !(cmd in scriptCheckCacheRef.current)
    );
    if (uncached.length === 0) return;

    let cancelled = false;

    (async () => {
      const results: Record<string, { exists: boolean; executable: boolean }> = {};
      await Promise.all(
        uncached.map(async (cmd) => {
          try
          {
            const res = await checkScript(cmd);
            results[cmd] = { exists: res.exists, executable: res.executable };
          }
          catch
          {
            // Transient error — leave uncached so it retries next time
          }
        })
      );

      if (cancelled) return;

      // Update cache
      scriptCheckCacheRef.current = { ...scriptCheckCacheRef.current, ...results };

      // Re-run validation to pick up the new script check results
      recomputeValidationRef.current({
        nodes: nodes as EditorTaskNode[],
        edges: edges as EditorTaskEdge[],
      });
    })();

    return () => { cancelled = true; };
  }, [shellCommandsFingerprint, nodes, edges, scriptCheckTick]);

  // Build a cache key for file_input checks. The backend resolves the actual path
  // using TaskPathResolver (same code as the runtime), so we just need a unique key.
  const fileInputCacheKey = (fileInput: string, workingDirectory: string): string => {
    return `${loadedWorkflowId ?? props.workflowId ?? ""}|${workingDirectory}|${fileInput}`;
  };

  // Async file_input existence check — fires when static file_input paths change.
  const fileInputsFingerprint = useMemo(() => {
    const paths: string[] = [];
    for (const node of nodes)
    {
      if (node.type !== "task") continue;
      const taskData = (node as EditorTaskNode).data;
      const taskType = taskData.task?.type;
      if (taskType !== "python" && taskType !== "shell") continue;
      const wd = typeof taskData.task.working_directory === "string" ? taskData.task.working_directory : "";
      const fileInputs = Array.isArray(taskData.task.file_inputs) ? taskData.task.file_inputs as string[] : [];
      for (const fp of fileInputs)
      {
        if (fp.length === 0 || fp.includes("{{")) continue;
        const cacheKey = fileInputCacheKey(fp, wd);
        paths.push(`${node.id}:${cacheKey}`);
      }
    }
    return paths.sort().join("|");
  }, [nodes]);

  useEffect(() => {
    if (fileInputsFingerprint.length === 0) return;

    const entries = fileInputsFingerprint.split("|").map((s) => {
      const idx = s.indexOf(":");
      return { nodeId: s.slice(0, idx), cacheKey: s.slice(idx + 1) };
    });

    const uncached = [...new Set(entries.map((e) => e.cacheKey))].filter(
      (key) => !(key in fileCheckCacheRef.current)
    );
    if (uncached.length === 0) return;

    let cancelled = false;

    (async () => {
      const results: Record<string, { exists: boolean }> = {};
      await Promise.all(
        uncached.map(async (key) => {
          try
          {
            // Cache key format: "workflowId|wd|filePath"
            const parts = key.split("|");
            const wfId = parts[0] || undefined;
            const wd = parts[1] || undefined;
            const filePath = parts.slice(2).join("|");
            const res = await checkFileExists(filePath, wfId, wd);
            results[key] = { exists: res.exists };
          }
          catch
          {
            // Transient error — leave uncached so it retries next time
          }
        })
      );

      if (cancelled) return;

      fileCheckCacheRef.current = { ...fileCheckCacheRef.current, ...results };

      recomputeValidationRef.current({
        nodes: nodes as EditorTaskNode[],
        edges: edges as EditorTaskEdge[],
      });
    })();

    return () => { cancelled = true; };
  }, [fileInputsFingerprint, nodes, edges, scriptCheckTick]);

  useEffect(() => {
    const runPaused = selectedRunId
      ? activeRuns.some((r) => r.runId === selectedRunId && r.state === "paused")
      : false;

    setNodes((current) => {
      let changed = false;
      const next = current.map((n) => {
        // Filter and branch nodes have no runtime state
        if (n.type === "filter" || n.type === "branch") { return n; }

        const taskNode = n as Node<EditorTaskNodeData>;
        const snapshot = runtimeTasksById[n.id];
        const nextRuntimeState = snapshot ? snapshot.state : undefined;
        const nextRuntimeRunId = snapshot ? snapshot.runId : undefined;
        const nextRuntimeError = snapshot?.lastErrorMessage;
        const nextStdout = snapshot?.capturedStdout;
        const nextStderr = snapshot?.capturedStderr;

        if (taskNode.data.runtimeState === nextRuntimeState
          && taskNode.data.runtimeRunId === nextRuntimeRunId
          && taskNode.data.runtimeError === nextRuntimeError
          && taskNode.data.capturedStdout === nextStdout
          && taskNode.data.capturedStderr === nextStderr
          && taskNode.data.isRunPaused === runPaused)
        {
          return n;
        }

        changed = true;
        return {
          ...taskNode,
          data: {
            ...taskNode.data,
            runtimeState: nextRuntimeState,
            runtimeRunId: nextRuntimeRunId,
            runtimeError: nextRuntimeError,
            capturedStdout: nextStdout,
            capturedStderr: nextStderr,
            isRunPaused: runPaused,
          },
        };
      });

      return changed ? next : current;
    });
  }, [runtimeTasksById, setNodes, activeRuns, selectedRunId]);

  // Update nodes when hideTierDWarnings setting changes
  useEffect(() => {
    setNodes((current) => {
      return current.map((n) => {
        if (n.type === "filter" || n.type === "branch") { return n; }
        const taskNode = n as Node<EditorTaskNodeData>;
        if (taskNode.data.hideTierDWarnings === props.hideTierDWarnings)
        {
          return n;
        }
        return { ...taskNode, data: { ...taskNode.data, hideTierDWarnings: props.hideTierDWarnings } };
      });
    });
  }, [props.hideTierDWarnings, setNodes]);

  const loadFromJcwf = useCallback((workflowId: string | null, jcwfUnknown: unknown) => {
    const jcwf = jcwfUnknown as JcwfFile;
    const graph = jcwfToGraph(jcwf, workflowId ?? (typeof jcwf.id === "string" ? jcwf.id : undefined));
    // For templates (workflowId is null), mark as dirty since it's not saved yet
    const isFromTemplate = workflowId === null;
    if (!isFromTemplate)
    {
      const graphSig = computeGraphSignature(graph.nodes.filter((n): n is EditorTaskNode => n.type === "task"), graph.edges, graph.nodes.filter((n): n is EditorControlNode => n.type === "branch"));
      const loadedLabel = typeof jcwf.label === "string" ? jcwf.label : "";
      const loadedDoc = Array.isArray(jcwf.doc) ? jcwf.doc.join("\n") : (typeof jcwf.doc === "string" ? jcwf.doc : "");
      const loadedBaseDir = typeof jcwf.base_directory === "string" ? (jcwf.base_directory as string) : "";
      const loadedDefaults = (jcwf as Record<string, unknown>).defaults as Record<string, unknown> | undefined;
      const loadedTimeout = loadedDefaults?.timeout_ms !== undefined ? String(loadedDefaults.timeout_ms) : "";
      const loadedAi = loadedDefaults?.ai as Record<string, unknown> | undefined;
      const loadedProvider = typeof loadedAi?.provider === "string" ? loadedAi.provider : "";
      const loadedModel = typeof loadedAi?.model === "string" ? loadedAi.model : "";
      const loadedManualStart = jcwf.manual_start !== false;
      const loadedTriggers = Array.isArray(jcwf.triggers) ? jcwf.triggers : [];
      const wfFieldsSig = JSON.stringify({
        wfLabel: loadedLabel, wfDoc: loadedDoc, wfBaseDirectory: loadedBaseDir,
        wfDefaultTimeoutMs: loadedTimeout, wfDefaultAiProvider: loadedProvider, wfDefaultAiModel: loadedModel,
        manualStartEnabled: loadedManualStart, triggers: loadedTriggers,
      });
      setLastSavedSignature(graphSig + "|" + wfFieldsSig);
      setLastSavedNodeSnapshot(computeNodeSnapshot(graph.nodes.filter((n): n is EditorTaskNode => n.type === "task")));
      setIsDirty(false);
      if (props.onDirtyStateChange)
      {
        props.onDirtyStateChange(false);
      }
    }
    else
    {
      setLastSavedSignature("");
      setLastSavedNodeSnapshot({});
      setIsDirty(true);
      if (props.onDirtyStateChange)
      {
        props.onDirtyStateChange(true);
      }
    }
    recomputeValidation(graph);
    loadedJcwfRef.current = jcwf;
    setLoadedWorkflowId(workflowId);
    setManualStartEnabled(jcwf.manual_start !== false);
    setTriggers(Array.isArray(jcwf.triggers) ? jcwf.triggers : []);
    setWfLabel(typeof jcwf.label === "string" ? jcwf.label : "");
    setWfDoc(Array.isArray(jcwf.doc) ? jcwf.doc.join("\n") : (typeof jcwf.doc === "string" ? jcwf.doc : ""));
    setWfBaseDirectory(typeof jcwf.base_directory === "string" ? (jcwf.base_directory as string) : "");
    const defaults = (jcwf as Record<string, unknown>).defaults as Record<string, unknown> | undefined;
    setWfDefaultTimeoutMs(defaults?.timeout_ms !== undefined ? String(defaults.timeout_ms) : "");
    const ai = defaults?.ai as Record<string, unknown> | undefined;
    setWfDefaultAiProvider(typeof ai?.provider === "string" ? ai.provider : "");
    setWfDefaultAiModel(typeof ai?.model === "string" ? ai.model : "");
    setBackendErrors([]);
    setBackendWarnings([]);
    setBackendInfos([]);
    setSelectedNodeId(null);
    setStatusText(isFromTemplate ? "Loaded from template. Save to create workflow." : `Loaded workflow '${workflowId}'.`);
    setErrorText(null);
  }, [recomputeValidation, props.onDirtyStateChange]);

  const loadFromJcwfRef = useRef(loadFromJcwf);
  useEffect(() => {
    loadFromJcwfRef.current = loadFromJcwf;
  }, [loadFromJcwf]);

  // Sub-workflow navigation: enter a child workflow canvas
  const navigateToSubWorkflow = useCallback(async (workflowFile: string) => {
    if (!workflowFile)
    {
      setErrorText("Sub-workflow has no workflow_file set.");
      return;
    }

    // If dirty, warn the user before navigating away.
    if (isDirty)
    {
      const proceed = window.confirm("You have unsaved changes. Navigate to sub-workflow anyway?");
      if (!proceed) return;
    }

    try
    {
      // Build the sub-workflow ID: parentId__workflowFile
      const subWorkflowId = loadedWorkflowId ? `${loadedWorkflowId}__${workflowFile}` : workflowFile;

      // Push current workflow onto the nav stack (avoid duplicates).
      const currentId = loadedWorkflowId;
      if (currentId && currentId !== subWorkflowId)
      {
        setWorkflowNavStack((prev) =>
          prev[prev.length - 1] === currentId ? prev : [...prev, currentId]
        );
      }

      // Load the child workflow.
      const jcwf = await loadWorkflow(subWorkflowId);
      if (jcwf !== null)
      {
        loadFromJcwfRef.current(subWorkflowId, jcwf);
        setStatusText(`Navigated into sub-workflow '${subWorkflowId}'.`);
      }
      else
      {
        setErrorText(`Failed to load sub-workflow '${subWorkflowId}'.`);
      }
    }
    catch (e)
    {
      setErrorText(`Navigation error: ${e instanceof Error ? e.message : String(e)}`);
    }
  }, [isDirty, loadedWorkflowId]);

  // Sub-workflow navigation: go back to parent workflow
  const navigateBack = useCallback(async () => {
    if (workflowNavStack.length === 0) return;

    if (isDirty)
    {
      const proceed = window.confirm("You have unsaved changes. Navigate back anyway?");
      if (!proceed) return;
    }

    const parentId = workflowNavStack[workflowNavStack.length - 1];
    setWorkflowNavStack((prev) => prev.slice(0, -1));

    try
    {
      const jcwf = await loadWorkflow(parentId);
      if (jcwf !== null)
      {
        loadFromJcwfRef.current(parentId, jcwf);
        setStatusText(`Returned to workflow '${parentId}'.`);
      }
      else
      {
        setErrorText(`Failed to load parent workflow '${parentId}'.`);
      }
    }
    catch (e)
    {
      setErrorText(`Navigation error: ${e instanceof Error ? e.message : String(e)}`);
    }
  }, [workflowNavStack, isDirty]);

  const getCurrentJcwf = useCallback((): JcwfFile | null => {
    const workflowId = loadedWorkflowId ?? props.workflowId ?? "workflow";
    const graph: EditorGraph = { nodes: nodes as EditorTaskNode[], edges };
    const result = graphToJcwf(graph, workflowId);
    if (!result.ok)
    {
      return null;
    }
    const merged: JcwfFile = {
      ...(loadedJcwfRef.current ?? {}),
      ...result.jcwf,
    };
    if (wfLabel.length > 0) { merged.label = wfLabel; }
    const docValue = wfDoc.length > 0 ? (wfDoc.includes("\n") ? wfDoc.split("\n") : wfDoc) : undefined;
    if (docValue) { merged.doc = docValue; }
    if (triggers.length > 0) { merged.triggers = triggers; }
    if (!manualStartEnabled) { merged.manual_start = false; }
    const timeoutNum = wfDefaultTimeoutMs.length > 0 ? Number(wfDefaultTimeoutMs) : undefined;
    if (timeoutNum !== undefined || wfDefaultAiProvider.length > 0 || wfDefaultAiModel.length > 0)
    {
      const defs: Record<string, unknown> = { ...((merged as Record<string, unknown>).defaults as Record<string, unknown> ?? {}) };
      if (timeoutNum !== undefined && !isNaN(timeoutNum)) { defs.timeout_ms = timeoutNum; }
      if (wfDefaultAiProvider.length > 0 || wfDefaultAiModel.length > 0)
      {
        const ai: Record<string, unknown> = { ...(defs.ai as Record<string, unknown> ?? {}) };
        if (wfDefaultAiProvider.length > 0) { ai.provider = wfDefaultAiProvider; }
        if (wfDefaultAiModel.length > 0) { ai.model = wfDefaultAiModel; }
        defs.ai = Object.keys(ai).length > 0 ? ai : undefined;
      }
      (merged as Record<string, unknown>).defaults = defs;
    }
    return merged;
  }, [nodes, edges, loadedWorkflowId, props.workflowId, wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel, manualStartEnabled, triggers]);

  const onAutoLayoutRef = useRef<(() => void) | null>(null);

  const onJcwfGenerated = useCallback((jcwf: JcwfFile) => {
    // Capture the current ID before loadFromJcwf resets it.
    const existingId = loadedWorkflowId;
    // Load as template (null) so isDirty=true and savedSignature is reset.
    loadFromJcwf(null, jcwf);
    // Restore the workflowId so Save does PUT (update) instead of POST (create).
    if (existingId)
    {
      setLoadedWorkflowId(existingId);
    }
    setStatusText("Loaded AI-generated workflow. Review and Save.");
    // Auto-layout arranges nodes by dependency topology, then fitView centers the result.
    // Deferred so React has time to render the new nodes.
    setTimeout(() => {
      onAutoLayoutRef.current?.();
      reactFlowInstance?.fitView({ padding: 0.15, maxZoom: 1.2 });
    }, 80);
  }, [loadFromJcwf, loadedWorkflowId, reactFlowInstance]);

  const skipFitViewRef = useRef(false);

  const resetToNewDraftRef = useRef(resetToNewDraft);
  useEffect(() => {
    resetToNewDraftRef.current = resetToNewDraft;
  }, [resetToNewDraft]);

  // Respond to workflowId changes.
  useEffect(() => {
    let isCancelled = false;

    async function load(): Promise<void>
    {
      // If initialJcwf is provided (template), use it directly
      if (props.initialJcwf)
      {
        loadFromJcwfRef.current(null, props.initialJcwf);
        setStatusText("Loaded from template.");
        return;
      }

      if (!props.workflowId)
      {
        // "New" workflow.
        resetToNewDraftRef.current();
        return;
      }

      try
      {
        setStatusText("Loading…");
        setErrorText(null);

        const jcwf = await loadWorkflow(props.workflowId);
        if (!isCancelled)
        {
          if (jcwf !== null)
          {
            loadFromJcwfRef.current(props.workflowId, jcwf);
          }
          else
          {
            setStatusText("");
          }
        }
      }
      catch (e)
      {
        if (!isCancelled)
        {
          const message = e instanceof Error ? e.message : String(e);
          setErrorText(`Load failed: ${message}`);
          setStatusText("");
        }
      }
    }

    void load();
    return () => { isCancelled = true; };
  }, [props.workflowId, props.initialJcwf]);

  // Center the graph after a workflow loads.
  const MIN_ZOOM = 0.10;
  useEffect(() => {
    if (!reactFlowInstance)
    {
      return;
    }
    if (skipFitViewRef.current)
    {
      skipFitViewRef.current = false;
      return;
    }
    const timer = setTimeout(() => {
      reactFlowInstance.fitView({ padding: 0.15, maxZoom: 1.2 });
      const vp = reactFlowInstance.getViewport();
      if (vp.zoom <= MIN_ZOOM)
      {
        // Graph too large to fit — show top of graph at minimum zoom
        const allNodes = reactFlowInstance.getNodes();
        if (allNodes.length > 0)
        {
          const minY = Math.min(...allNodes.map((n) => n.position.y));
          const y = 20 - minY * MIN_ZOOM; // small top padding
          reactFlowInstance.setViewport({ x: vp.x, y, zoom: MIN_ZOOM });
        }
      }
      setCurrentZoom(reactFlowInstance.getViewport().zoom);
    }, 80);
    return () => clearTimeout(timer);
  }, [loadedWorkflowId, reactFlowInstance]);

  // Keep selectedRunIdRef in sync with state.
  useEffect(() => {
    selectedRunIdRef.current = selectedRunId;
  }, [selectedRunId]);

  // WebSocket run monitoring — single connection for the lifetime of the component.
  useEffect(() => {
    let unmounted = false;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

    function connect()
    {
      if (unmounted) return;

      const socket = new WebSocket(buildWebSocketUrl("/ws"));
      webSocketRef.current = socket;

      socket.onopen = () => {
        setIsWebSocketConnected(true);
        try
        {
          socket.send(JSON.stringify({ type: "workflow-runs-request" }));
        }
        catch
        {
          // ignore
        }
      };

      socket.onclose = () => {
        setIsWebSocketConnected(false);
        if (!unmounted)
        {
          reconnectTimer = setTimeout(connect, 2000);
        }
      };

      socket.onmessage = (event: MessageEvent) => {
        let message: unknown;
        try
        {
          message = JSON.parse(String(event.data)) as unknown;
        }
        catch
        {
          return;
        }

        if (!message || typeof message !== "object" || Array.isArray(message))
        {
          return;
        }

        const processOne = (obj: Record<string, unknown>) => {
        const messageType = typeof obj.type === "string" ? obj.type : "";

        // Unwrap batch envelope (server batches all broadcasts into a single frame).
        if (messageType === "batch")
        {
          const msgs = obj.messages;
          if (Array.isArray(msgs))
          {
            for (const sub of msgs)
            {
              if (sub && typeof sub === "object" && !Array.isArray(sub))
              {
                processOne(sub as Record<string, unknown>);
              }
            }
          }
          return;
        }

        // Older snapshot shape: { type: "workflowRunsSnapshot", runs: [...] }
        if (messageType === "workflowRunsSnapshot")
        {
          const runsUnknown = obj.runs;
          if (!Array.isArray(runsUnknown))
          {
            return;
          }

          const runs = runsUnknown as Record<string, unknown>[];
          const nextActiveRuns: WorkflowRunListItem[] = [];
          for (const r of runs)
          {
            const runId = typeof r.runId === "string" ? r.runId : "";
            const workflowId = typeof r.workflowId === "string" ? r.workflowId : "";
            const state = typeof r.state === "string" ? r.state : "";
            if (runId.length > 0 && workflowId.length > 0)
            {
              nextActiveRuns.push({
                runId,
                workflowId,
                state,
                startedAt: typeof r.startedAt === "string" ? r.startedAt : undefined,
                completedAt: typeof r.completedAt === "string" ? r.completedAt : undefined,
              });
            }
          }

          setActiveRuns(nextActiveRuns);

          const curSelectedRunId = selectedRunIdRef.current;
          const targetRunId = curSelectedRunId ?? (nextActiveRuns.length > 0 ? nextActiveRuns[0].runId : null);
          if (targetRunId && !curSelectedRunId)
          {
            setSelectedRunId(targetRunId);
          }

          if (!targetRunId)
          {
            // Keep last task states visible after run completes (don't clear).
            return;
          }

          const matchingRun = runs.find((r) => (typeof r.runId === "string" ? r.runId : "") === targetRunId);
          if (!matchingRun)
          {
            // Run left activeRuns (completed/failed) — keep last task states.
            return;
          }

          const tasksUnknown = matchingRun.tasks;
          if (!Array.isArray(tasksUnknown))
          {
            return;
          }

          const nextRuntime: RuntimeTaskSnapshotById = {};
          for (const t of tasksUnknown as Record<string, unknown>[])
          {
            const taskId = typeof t.taskId === "string" ? t.taskId : "";
            if (taskId.length === 0)
            {
              continue;
            }

            const rawState = typeof t.state === "string" ? t.state : "";
            const attemptCount = typeof t.attemptCount === "number" ? t.attemptCount : undefined;
            const lastErrorMessage = typeof t.lastErrorMessage === "string" && t.lastErrorMessage.length > 0 ? t.lastErrorMessage : undefined;
            nextRuntime[taskId] = {
              taskId,
              runId: targetRunId,
              state: normalizeRuntimeState(rawState),
              attemptCount,
              lastErrorMessage,
              capturedStdout: typeof t.capturedStdout === "string" ? t.capturedStdout : undefined,
              capturedStderr: typeof t.capturedStderr === "string" ? t.capturedStderr : undefined,
            };
          }

          setRuntimeTasksById(nextRuntime);
          return;
        }

        // Newer snapshot shape: { type: "workflow-runs-snapshot", activeRuns: [...] }
        if (messageType === "workflow-runs-snapshot")
        {
          const activeRunsUnknown = obj.activeRuns;
          if (!Array.isArray(activeRunsUnknown))
          {
            return;
          }

          const nextActiveRuns: WorkflowRunListItem[] = [];
          for (const r of activeRunsUnknown as Record<string, unknown>[])
          {
            const runId = typeof r.runId === "string" ? r.runId : "";
            const workflowId = typeof r.workflowId === "string" ? r.workflowId : "";
            const state = typeof r.state === "string" ? r.state : "";
            if (runId.length > 0 && workflowId.length > 0)
            {
              nextActiveRuns.push({
                runId,
                workflowId,
                state,
                startedAt: typeof r.startedAt === "string" ? r.startedAt : undefined,
                completedAt: typeof r.completedAt === "string" ? r.completedAt : undefined,
              });
            }
          }
          setActiveRuns(nextActiveRuns);

          // Extract task snapshots for the selected (or first) run
          const curSelectedRunId = selectedRunIdRef.current;
          const targetRunId = curSelectedRunId ?? (nextActiveRuns.length > 0 ? nextActiveRuns[0].runId : null);
          if (targetRunId && !curSelectedRunId)
          {
            setSelectedRunId(targetRunId);
          }

          if (!targetRunId)
          {
            // Keep last task states visible after run completes (don't clear).
            return;
          }

          const matchingRun = (activeRunsUnknown as Record<string, unknown>[]).find(
            (r) => (typeof r.runId === "string" ? r.runId : "") === targetRunId
          );
          if (!matchingRun || !Array.isArray(matchingRun.tasks))
          {
            // Run left activeRuns (completed/failed) — keep last task states.
            return;
          }

          const nextRuntime: RuntimeTaskSnapshotById = {};
          for (const t of matchingRun.tasks as Record<string, unknown>[])
          {
            const taskId = typeof t.taskId === "string" ? t.taskId : "";
            if (taskId.length === 0) { continue; }
            nextRuntime[taskId] = {
              taskId,
              runId: targetRunId,
              state: normalizeRuntimeState(typeof t.state === "string" ? t.state : ""),
              attemptCount: typeof t.attemptCount === "number" ? t.attemptCount : undefined,
              lastErrorMessage: typeof t.lastErrorMessage === "string" && t.lastErrorMessage.length > 0 ? t.lastErrorMessage : undefined,
              capturedStdout: typeof t.capturedStdout === "string" ? t.capturedStdout : undefined,
              capturedStderr: typeof t.capturedStderr === "string" ? t.capturedStderr : undefined,
            };
          }
          setRuntimeTasksById(nextRuntime);
          return;
        }
      };

        processOne(message as Record<string, unknown>);
      };
    }

    connect();

    return () => {
      unmounted = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      try
      {
        webSocketRef.current?.close();
      }
      catch
      {
        // ignore
      }
    };
  }, []);  // eslint-disable-line react-hooks/exhaustive-deps

  // Fetch real final task states from the REST API when a run completes.
  const fetchFinalRunState = useCallback(async (runId: string) => {
    try
    {
      const detail = await fetchRunDetails(runId);
      const runState = detail.run.state;
      const stateLabel = runState === "failed" ? "\u2717 Run failed" :
                         runState === "succeeded" ? "\u2713 Run completed successfully" :
                         runState === "stopped" ? "\u25a0 Run stopped" :
                         runState === "cancelled" ? "Run cancelled" :
                         `Run ${runState}`;
      setStatusText(`${stateLabel}. runId=${runId}`);
      setLastRunResult({ runId, state: runState });

      const nextRuntime: RuntimeTaskSnapshotById = {};
      for (const t of detail.run.tasks)
      {
        nextRuntime[t.taskId] = {
          taskId: t.taskId,
          runId,
          state: normalizeRuntimeState(t.state),
          attemptCount: t.attemptCount,
          lastErrorMessage: t.error,
          capturedStdout: t.capturedStdout,
          capturedStderr: t.capturedStderr,
        };
      }
      // The run is terminal here (it just left the active list) — reconcile any per_item parent the
      // backend left frozen non-terminal from its child instances (F-41).
      setRuntimeTasksById(reconcileTerminalParentStates(nextRuntime));
    }
    catch
    {
      // Fallback: infer states if API call fails
      setStatusText(`Run finished. runId=${runId}`);
      setLastRunResult({ runId, state: "unknown" });
    }
    setPendingRunId(null);
    pendingRunSeenRef.current = false;
    pendingRunLastStateRef.current = "running";
  }, []);

  // When the selected run disappears from activeRuns (completed/failed/cancelled),
  // fetch final task states from the REST API so badges update correctly.
  useEffect(() => {
    const currentIds = new Set(activeRuns.map((r) => r.runId));
    const prevIds = prevActiveRunIdsRef.current;
    prevActiveRunIdsRef.current = currentIds;

    if (selectedRunId && prevIds.has(selectedRunId) && !currentIds.has(selectedRunId))
    {
      // Selected run just left the active list — fetch final state
      void fetchFinalRunState(selectedRunId);
    }
  }, [activeRuns, selectedRunId, fetchFinalRunState]);

  // Poll run status while a run is pending — reliably fetches real task states.
  useEffect(() => {
    if (!pendingRunId)
    {
      return;
    }

    const terminalStates = ["completed", "failed", "cancelled", "succeeded", "stopped"];
    let cancelled = false;

    const poll = async () => {
      if (cancelled) return;
      try
      {
        const detail = await fetchRunDetails(pendingRunId);
        if (cancelled) return;

        const runState = detail.run.state;

        // Update task states from the real API data
        const nextRuntime: RuntimeTaskSnapshotById = {};
        for (const t of detail.run.tasks)
        {
          nextRuntime[t.taskId] = {
            taskId: t.taskId,
            runId: pendingRunId,
            state: normalizeRuntimeState(t.state),
            attemptCount: t.attemptCount,
            lastErrorMessage: t.error,
            capturedStdout: t.capturedStdout,
            capturedStderr: t.capturedStderr,
          };
        }
        // On the terminal poll, reconcile a per_item parent the backend left frozen non-terminal
        // from its child instances (F-41); mid-run polls keep the live states as-is.
        setRuntimeTasksById(terminalStates.includes(runState)
          ? reconcileTerminalParentStates(nextRuntime)
          : nextRuntime);

        if (terminalStates.includes(runState))
        {
          const stateLabel = runState === "failed" ? "\u2717 Run failed" :
                             (runState === "succeeded" || runState === "completed") ? "\u2713 Run completed successfully" :
                             runState === "stopped" ? "\u25a0 Run stopped" :
                             runState === "cancelled" ? "Run cancelled" :
                             `Run ${runState}`;
          setStatusText(`${stateLabel}. runId=${pendingRunId}`);
          setLastRunResult({ runId: pendingRunId, state: runState });
          setPendingRunId(null);
        }
      }
      catch
      {
        // Ignore transient fetch errors; will retry on next poll
      }
    };

    // Safety-net poll: WS push handles real-time updates; this is a fallback
    // in case the WebSocket connection drops or misses a terminal transition.
    const initialTimer = setTimeout(() => {
      void poll();
    }, 3000);

    const interval = setInterval(() => {
      void poll();
    }, 10000);

    return () => {
      cancelled = true;
      clearTimeout(initialTimer);
      clearInterval(interval);
    };
  }, [pendingRunId]);

  const selectedNode = useMemo((): EditorTaskNode | null => {
    if (!selectedNodeId)
    {
      return null;
    }
    const found = nodes.find((n) => n.id === selectedNodeId && n.type === "task");
    return (found as EditorTaskNode | undefined) ?? null;
  }, [nodes, selectedNodeId]);

  const selectedFilterNode = useMemo((): EditorFilterNode | null => {
    if (!selectedNodeId)
    {
      return null;
    }
    const found = nodes.find((n) => n.id === selectedNodeId && n.type === "filter");
    return (found as EditorFilterNode | undefined) ?? null;
  }, [nodes, selectedNodeId]);

  const selectedControlNode = useMemo((): EditorControlNode | null => {
    if (!selectedNodeId)
    {
      return null;
    }
    const found = nodes.find((n) => n.id === selectedNodeId && n.type === "branch");
    return (found as EditorControlNode | undefined) ?? null;
  }, [nodes, selectedNodeId]);

  const templateVariables = useMemo((): string[] => {
    if (!selectedNode) return [];
    const vars: string[] = [];
    const task = selectedNode.data.task;

    // Task's own declared inputs (bare names)
    const inputs = task.inputs as Record<string, unknown> | undefined;
    if (inputs)
    {
      for (const name of Object.keys(inputs)) vars.push(name);
    }

    // file_inputs indexed: input[0], input[1], ...
    const fi = Array.isArray(task.file_inputs) ? task.file_inputs as string[] : [];
    for (let i = 0; i < fi.length; i++) vars.push(`input[${i}]`);
    if (fi.length > 0) vars.push("inputs");

    // file_outputs indexed: output[0], output[1], ...
    const fo = Array.isArray(task.file_outputs) ? task.file_outputs as string[] : [];
    for (let i = 0; i < fo.length; i++) vars.push(`output[${i}]`);
    if (fo.length > 0) vars.push("outputs");

    // Upstream task outputs reachable via dataflow (taskId.outputName)
    const taskNodes = (nodes as EditorTaskNode[]).filter((n) => n.type === "task" && n.id !== selectedNode.id);
    for (const tn of taskNodes)
    {
      const outs = tn.data.task.outputs as Record<string, unknown> | undefined;
      if (outs)
      {
        for (const outName of Object.keys(outs))
        {
          vars.push(`${tn.id}.${outName}`);
        }
      }
    }

    return vars;
  }, [selectedNode, nodes]);

  const [aiInterfaces, setAiInterfaces] = useState<AiInterface[]>([]);
  useEffect(() => {
    listAiInterfaces().then((res) => {
      if (res.ok) setAiInterfaces(res.interfaces);
    }).catch(() => {});
  }, []);

  const selectNodeById = useCallback((nodeId: string | null) => {
    setSelectedNodeId(nodeId);
    setNodes((current) => {
      const currentNodes = current as EditorTaskNode[];
      if (!nodeId)
      {
        return currentNodes.map((n) => (n.selected ? { ...n, selected: false } : n));
      }

      return currentNodes.map((n) => {
        const nextSelected = n.id === nodeId;
        return n.selected === nextSelected ? n : { ...n, selected: nextSelected };
      });
    });
  }, [setNodes]);

  const [selectedEdgeIds, setSelectedEdgeIds] = useState<string[]>([]);
  // Full multi-selection (box-select) — authoritative source for copy. The single
  // selectedNodeId above drives the inspector; this list drives Ctrl+C of N nodes.
  const [selectedNodeIds, setSelectedNodeIds] = useState<string[]>([]);

  const onSelectionChange = useCallback((params: { nodes?: Node[]; edges?: Edge[]; }) => {
    const selected = params.nodes && params.nodes.length > 0 ? params.nodes[0] : null;
    setSelectedNodeId(selected ? selected.id : null);
    setSelectedNodeIds((params.nodes ?? []).map((n) => n.id));
    setSelectedEdgeIds(params.edges ? params.edges.map((e) => e.id) : []);
  }, []);

  const onDeleteSelectedEdges = useCallback(() => {
    if (selectedEdgeIds.length === 0)
    {
      return;
    }
    const nextEdges = edges.filter((e) => !selectedEdgeIds.includes(e.id));
    setEdges(nextEdges as EditorTaskEdge[]);
    setSelectedEdgeIds([]);
    recomputeValidation({ nodes: nodes as EditorTaskNode[], edges: nextEdges as EditorTaskEdge[] });
    setStatusText(`Deleted ${selectedEdgeIds.length} edge(s).`);
  }, [selectedEdgeIds, edges, nodes, setEdges, recomputeValidation]);

  const onConnect = useCallback((rawConnection: Connection) => {
    // ConnectionMode.Loose lets the user drag from either end of a wire; normalize so an
    // output-like handle is always the source and an input-like handle the target.
    const isOutputHandle = (h: string | null | undefined): boolean =>
      !!h && (h.startsWith("out:") || h.startsWith("fileoutput-") || h === "dep-source" || h === "error-signal" || h.startsWith("cf-out"));
    const isInputHandle = (h: string | null | undefined): boolean =>
      !!h && (h.startsWith("in:") || h.startsWith("dephandle-") || h === "dep-target" || h.startsWith("cf-in"));
    const connection: Connection = (isInputHandle(rawConnection.sourceHandle) && isOutputHandle(rawConnection.targetHandle))
      ? { source: rawConnection.target, target: rawConnection.source, sourceHandle: rawConnection.targetHandle, targetHandle: rawConnection.sourceHandle }
      : rawConnection;

    // Artifact-file node → task input (U3). The file node is a pure overlay; wiring it sets the
    // consumer's file_inputs to the relative path from the consumer's wd to the file, and draws a
    // `file:` edge (so it round-trips: graphToJcwf skips it, jcwfToGraph reconstructs from the
    // editor_layout marker + matching file_inputs). Only file_inputs-reading tasks (shell/python/
    // internal) accept a file node; ai_call reads via queue cntx, not file ports.
    {
      const fileSource = (nodes as EditorNode[]).find((n) => n.id === connection.source && n.type === "file");
      if (fileSource && fileSource.type === "file")
      {
        const targetTaskNode = (nodes as EditorNode[]).find((n) => n.id === connection.target && n.type === "task");
        if (!targetTaskNode || targetTaskNode.type !== "task" || !readsFileInputs(targetTaskNode.data.task.type))
        {
          setStatusText("A file can only feed a shell, python, or internal task.");
          return;
        }
        const consumerWd = (targetTaskNode.data.task.working_directory ?? "") as string;
        const inputPath = fileNodeInputPath(fileSource.data.workflowRelPath, consumerWd, loadedWorkflowId ?? props.workflowId ?? "workflow");
        const existing: string[] = Array.isArray(targetTaskNode.data.task.file_inputs)
          ? [...(targetTaskNode.data.task.file_inputs as string[])] : [];
        let idx = existing.indexOf(inputPath);
        if (idx < 0)
        {
          const emptyIdx = existing.findIndex((s) => s.trim().length === 0);
          if (emptyIdx >= 0) { existing[emptyIdx] = inputPath; idx = emptyIdx; }
          else { existing.push(inputPath); idx = existing.length - 1; }
        }
        const edgeId = `file:${fileSource.data.workflowRelPath}->${targetTaskNode.id}:${idx}`;
        if (edges.some((e) => e.id === edgeId))
        {
          setStatusText("That file is already wired to this task.");
          return;
        }
        const color = FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length];
        const fileEdge: EditorTaskEdge = {
          id: edgeId,
          source: fileSource.id,
          target: targetTaskNode.id,
          targetHandle: `dephandle-${idx}`,
          type: "default",
          style: { stroke: color, strokeWidth: 2, strokeDasharray: "1 0" },
        };
        const updated = (nodes as EditorNode[]).map((n) => {
          if (n.id !== targetTaskNode.id || n.type !== "task") return n;
          const nextTask = { ...(n.data.task as JcwfTask), file_inputs: existing };
          const { title, subtitle } = nodeTitleFromTask(nextTask);
          return { ...n, data: { ...n.data, task: nextTask, title, subtitle } };
        });
        recomputeValidation({ nodes: updated, edges: [...edges, fileEdge] });
        setStatusText(`Wired '${fileSource.data.title}' → ${targetTaskNode.id}.`);
        setErrorText(null);
        return;
      }
    }

    // Route through the shared edge classifier (U2 / S5) — one source of truth for what an edge means,
    // also used by graphToJcwf's serialization. (The file-node → task case is a fileflow sub-case
    // handled specially above.)
    const edgeClass = classifyEdge(connection.sourceHandle, connection.targetHandle, connection.source ?? "", connection.target ?? "");
    const isDataflow = edgeClass === "dataflow";
    const isControlflow = edgeClass === "controlflow";

    // Filter → task fan-out (B). Drawing the edge IS the binding: set task.filter + mode:"per_item"
    // automatically (the user never re-types the filter id) and draw the fanout edge. The binding
    // lives in task.filter (graphToJcwf skips the fanout edge), and jcwfToGraph re-derives the edge on
    // load — so this is the inverse of that derivation.
    if (edgeClass === "fanout")
    {
      const filterNodeId = connection.source ?? "";
      const filterId = filterNodeId.replace(/^filter:/, "");
      const fanoutTarget = (nodes as EditorNode[]).find((n) => n.id === connection.target && n.type === "task");
      if (!fanoutTarget || fanoutTarget.type !== "task")
      {
        setStatusText("A filter can only fan out into a task.");
        return;
      }
      const fanoutEdgeId = `fanout:${filterNodeId}->${fanoutTarget.id}`;
      if (edges.some((e) => e.id === fanoutEdgeId))
      {
        setStatusText("That filter already fans out into this task.");
        return;
      }
      const fanoutEdge: EditorTaskEdge = {
        id: fanoutEdgeId,
        source: filterNodeId,
        target: fanoutTarget.id,
        style: { strokeDasharray: "6 3", stroke: "rgba(180, 140, 255, 0.7)" },
        label: "per_item",
        labelStyle: { fill: "rgba(180, 140, 255, 0.9)", fontSize: 10 },
      };
      const updated = (nodes as EditorNode[]).map((n) => {
        if (n.id !== fanoutTarget.id || n.type !== "task") return n;
        const nextTask = { ...(n.data.task as JcwfTask), filter: filterId, mode: "per_item" as const };
        const { title, subtitle } = nodeTitleFromTask(nextTask);
        return { ...n, data: { ...n.data, task: nextTask, title, subtitle } };
      });
      recomputeValidation({ nodes: updated, edges: [...edges, fanoutEdge] });
      setStatusText(`'${fanoutTarget.id}' now fans out over '${filterId}'.`);
      setErrorText(null);
      return;
    }

    let nextEdges: EditorTaskEdge[];
    // Id of the dependency edge just created (set in the dep branch below) so the
    // auto-populate step can snap it to whichever input port ends up holding its file.
    let createdDepEdgeId: string | null = null;
    if (isDataflow)
    {
      const fromOutput = connection.sourceHandle!.slice(4);
      const toInput = connection.targetHandle!.slice(3);
      const dfId = `df:${connection.source}.${fromOutput}->${connection.target}.${toInput}`;
      // Don't pile up duplicate dataflow edges if the same wire is drawn more than once.
      if (edges.some((e) => e.id === dfId))
      {
        setStatusText("Dataflow edge already exists.");
        return;
      }
      const dfEdge: EditorTaskEdge = {
        id: dfId,
        source: connection.source!,
        target: connection.target!,
        sourceHandle: connection.sourceHandle,
        targetHandle: connection.targetHandle,
        type: "default",
        style: { strokeDasharray: "5 4", stroke: "rgba(100, 210, 180, 0.7)" },
        label: `${fromOutput} → ${toInput}`,
        labelStyle: { fill: "rgba(100, 210, 180, 0.85)", fontSize: 10 },
      };
      nextEdges = [...edges, dfEdge] as EditorTaskEdge[];
    }
    else if (isControlflow)
    {
      const kind = connection.sourceHandle === "error-signal" || connection.targetHandle === "cf-in-error"
        ? "error_signal"
        : connection.sourceHandle === "cf-out-error"
          ? "on_error"
          : "normal";

      const style = kind === "error_signal"
        ? { strokeDasharray: "2 3", stroke: "rgba(255,120,120,0.85)", strokeWidth: 2 }
        : kind === "on_error"
          ? { strokeDasharray: "6 4", stroke: "rgba(255,120,120,0.85)", strokeWidth: 2 }
          : { strokeDasharray: "6 4", stroke: "rgba(255,200,140,0.9)", strokeWidth: 2 };

      const label = kind === "error_signal" ? "error" : (kind === "on_error" ? "on_error" : "normal");

      const cfEdge: EditorTaskEdge = {
        id: `cf:${connection.source}->${connection.target}:${kind}`,
        source: connection.source!,
        target: connection.target!,
        sourceHandle: connection.sourceHandle,
        targetHandle: connection.targetHandle,
        type: "default",
        style,
        label,
        labelStyle: { fill: "rgba(255,220,180,0.85)", fontSize: 10 },
      };

      nextEdges = [...edges, cfEdge] as EditorTaskEdge[];
    }
    else
    {
      const isDepHandle = typeof connection.targetHandle === "string" && connection.targetHandle.startsWith("dephandle-");
      const depHandleIdx = isDepHandle ? parseInt(connection.targetHandle!.slice(10), 10) : -1;
      // Assign a `dep:` id so graphToJcwf serialises this edge into depends_on. A ReactFlow
      // auto-generated id (reactflow__edge-…) is silently dropped on save — graphToJcwf only
      // keeps edges whose id starts with "dep:" (matching jcwfToGraph's load-time pattern).
      const baseConn = {
        ...connection,
        id: `dep:${connection.source}->${connection.target}:${isDepHandle ? depHandleIdx : "x"}`,
        type: "default",
      } as Connection & { id: string; type: string; style?: React.CSSProperties };
      if (isDepHandle && depHandleIdx >= 0)
      {
        (baseConn as unknown as { style: React.CSSProperties }).style = { stroke: FILE_INPUT_COLORS[depHandleIdx % FILE_INPUT_COLORS.length], strokeWidth: 2 };
      }
      createdDepEdgeId = baseConn.id;
      nextEdges = addEdge(baseConn, edges) as EditorTaskEdge[];
    }

    // Auto-populate file_inputs on the target when connecting into a shell/python task:
    //   - ai_call source     → derive input paths from the source's prob_files (… → .output.txt)
    //   - shell/python source → derive from the source's file_outputs (the file it writes)
    let updatedNodes = nodes as EditorTaskNode[];
    if (!isDataflow && connection.source && connection.target)
    {
      const sourceNode = updatedNodes.find((n) => n.id === connection.source && n.type === "task");
      const targetNode = updatedNodes.find((n) => n.id === connection.target && n.type === "task");
      // shell / python / internal / ai_call all read their inputs as file_inputs, so an edge into
      // any of them auto-populates the input path from the upstream output AND retargets the edge to
      // the matching port (F-19 adds internal; F-23 adds ai_call — without it an edge into an
      // ai_call stayed on the hidden catch-all handle and dangled once a port appeared).
      const targetReadsFileInputs = !!targetNode && acceptsWiredInput(targetNode.data.task.type);

      let newInputPaths: string[] = [];

      if (sourceNode && targetNode && targetReadsFileInputs)
      {
        newInputPaths = deriveUpstreamOutputPaths(sourceNode.data.task, targetNode.data.task, loadedWorkflowId ?? props.workflowId ?? "workflow", connection.sourceHandle);
      }

      if (targetNode && newInputPaths.length > 0)
      {
        const existingInputs: string[] = Array.isArray(targetNode.data.task.file_inputs)
          ? [...(targetNode.data.task.file_inputs as string[])]
          : [];

        // If the drop landed on an explicit existing port whose edge was removed (a now-open
        // port — e.g. the user deleted an edge and re-dropped onto that same port), reuse it:
        // overwrite its file with this edge's file instead of appending a fresh port (F-16).
        const edgeFileForReuse = newInputPaths[0];
        if (typeof connection.targetHandle === "string" && connection.targetHandle.startsWith("dephandle-")
          && edgeFileForReuse && !existingInputs.includes(edgeFileForReuse))
        {
          const di = parseInt(connection.targetHandle.slice(10), 10);
          const portHasLiveEdge = (edges as EditorTaskEdge[]).some((e) =>
            e.id !== createdDepEdgeId
            && e.target === targetNode.id
            && e.targetHandle === `dephandle-${di}`);
          if (di >= 0 && di < existingInputs.length && !portHasLiveEdge)
          {
            existingInputs[di] = edgeFileForReuse;
          }
        }

        // Fill empty slots first, then append remaining
        const toPlace = newInputPaths.filter((p) => !existingInputs.includes(p));
        const toAppend: string[] = [];
        for (const path of toPlace)
        {
          // Append a NEW port for each new file — never reuse an occupied port for a
          // different file (empty slots are still filled first so blank ports aren't wasted).
          const emptyIdx = existingInputs.findIndex((s) => s.trim().length === 0);
          if (emptyIdx >= 0)
          {
            existingInputs[emptyIdx] = path;
          }
          else
          {
            toAppend.push(path);
          }
        }
        const mergedInputs = [...existingInputs, ...toAppend];
        const changed = mergedInputs.length !== (Array.isArray(targetNode.data.task.file_inputs) ? (targetNode.data.task.file_inputs as string[]).length : 0)
          || mergedInputs.some((v, i) => v !== ((targetNode.data.task.file_inputs as string[]) ?? [])[i]);

        if (changed)
        {
          updatedNodes = updatedNodes.map((n) => {
            if (n.id !== targetNode.id) return n;
            const nextTask = { ...(n.data.task as JcwfTask), file_inputs: mergedInputs };
            const { title, subtitle } = nodeTitleFromTask(nextTask);
            return { ...n, data: { ...n.data, task: nextTask, title, subtitle } };
          });
        }

        // Snap the edge to whichever port actually holds THIS edge's file — the port we
        // just created/filled, even if the drop landed on the generic handle or on an
        // already-occupied port belonging to a different file.
        const edgeFile = newInputPaths.length > 0 ? newInputPaths[0] : null;
        const edgePortIndex = edgeFile ? mergedInputs.indexOf(edgeFile) : -1;
        if (createdDepEdgeId && edgePortIndex >= 0)
        {
          const desiredHandle = `dephandle-${edgePortIndex}`;
          const newId = `dep:${connection.source}->${connection.target}:${edgePortIndex}`;
          const color = FILE_INPUT_COLORS[edgePortIndex % FILE_INPUT_COLORS.length];
          const duplicate = nextEdges.some((e) => e.id === newId && e.id !== createdDepEdgeId);
          if (duplicate)
          {
            // Same source already wired to this port — drop the redundant new edge.
            nextEdges = nextEdges.filter((e) => e.id !== createdDepEdgeId) as EditorTaskEdge[];
          }
          else
          {
            nextEdges = nextEdges.map((e) => e.id === createdDepEdgeId
              ? { ...e, id: newId, targetHandle: desiredHandle, style: { stroke: color, strokeWidth: 2 } }
              : e) as EditorTaskEdge[];
          }
        }
      }
    }

    const graph: EditorGraph = {
      nodes: updatedNodes,
      edges: nextEdges,
    };

    const validation = validateGraph(graph);
    if (validation.cycleNodes.length > 0)
    {
      setStatusText("Blocked: edge would create a cycle.");
      setErrorText(null);
      // do not apply the edge
      recomputeValidation({ nodes: updatedNodes, edges });
      return;
    }

    recomputeValidation({ nodes: updatedNodes, edges: nextEdges });
    setStatusText(isDataflow ? "Dataflow edge added." : `Edge added.${updatedNodes !== nodes ? " file_inputs auto-populated from upstream output." : ""}`);
    setErrorText(null);
  }, [nodes, edges, recomputeValidation, loadedWorkflowId, props.workflowId]);

  const findNonOverlappingPosition = useCallback((
    _startX: number,
    _startY: number,
    existingNodes: EditorTaskNode[]
  ): { x: number; y: number } => {
    if (existingNodes.length === 0)
    {
      return { x: 50, y: 50 };
    }

    const nodeWidth = 280;
    const padding = 40;

    // Find bounding box of all existing nodes
    let maxRight = -Infinity;
    let minTop = Infinity;
    for (const node of existingNodes)
    {
      const right = node.position.x + nodeWidth;
      if (right > maxRight) maxRight = right;
      if (node.position.y < minTop) minTop = node.position.y;
    }

    // Place new node to the right of the bounding box
    return { x: maxRight + padding, y: minTop };
  }, []);

  const addTaskNode = useCallback((taskType: JcwfTaskType) => {
    const existingIds = new Set<string>((nodes as EditorTaskNode[]).map((n) => n.id));
    const newId = nextId(existingIds, taskType);

    const task = buildDefaultTask(newId, taskType);
    // AI tasks self-configure a queue working_directory so their runtime files (transcript,
    // output, queue files) land in the queue area instead of polluting the workflow folder —
    // no warning needed, and the downstream auto-fill computes a matching path.
    if (taskType === "ai_call")
    {
      const wfId = loadedWorkflowId ?? props.workflowId ?? "workflow";
      task.working_directory = `../../queue/${wfId}/${newId}`;
    }
    const { title, subtitle } = nodeTitleFromTask(task);

    const viewportCenter = reactFlowInstance ? reactFlowInstance.project({
      x: window.innerWidth / 2,
      y: window.innerHeight / 2,
    }) : { x: 0, y: 0 };

    const position = findNonOverlappingPosition(
      viewportCenter.x,
      viewportCenter.y,
      nodes as EditorTaskNode[]
    );

    const newNode: EditorTaskNode = {
      id: newId,
      type: "task",
      position,
      data: { task, title, subtitle },
    };

    const graph: EditorGraph = { nodes: [...(nodes as EditorTaskNode[]), newNode], edges };
    recomputeValidation(graph);
    setSelectedNodeId(newId);
    setStatusText(`Added node '${newId}'.`);
    setErrorText(null);
  }, [nodes, edges, reactFlowInstance, recomputeValidation, findNonOverlappingPosition]);

  // Artifact-file nodes (U3 creation). A file lives in the workflow folder; its node is a `file:<rel>`
  // overlay wired into a task input. The "+ file" button and OS drag-drop both upload the bytes via
  // POST /api/workflows/<id>/files (which repacks the .jcwf), then place the node.
  const fileUploadInputRef = useRef<HTMLInputElement>(null);
  const [showFilePicker, setShowFilePicker] = useState(false);

  const addFileNode = useCallback((workflowRelPath: string, dropPos?: { x: number; y: number }) => {
    const fileNodeId = `file:${workflowRelPath}`;
    if ((nodes as EditorNode[]).some((n) => n.id === fileNodeId))
    {
      setSelectedNodeId(fileNodeId);
      setStatusText(`File '${workflowRelPath}' is already on the canvas.`);
      return;
    }
    const pos = dropPos
      ?? (reactFlowInstance ? reactFlowInstance.project({ x: window.innerWidth / 2, y: window.innerHeight / 2 }) : { x: 0, y: 0 });
    const newNode: EditorFileNode = {
      id: fileNodeId,
      type: "file",
      position: pos,
      data: { workflowRelPath, title: workflowRelPath.split("/").pop() || workflowRelPath },
    };
    recomputeValidation({ nodes: [...(nodes as EditorNode[]), newNode], edges });
    setSelectedNodeId(fileNodeId);
    setStatusText(`Added file '${workflowRelPath}'. Drag from its dot into a shell/python/internal task.`);
    setErrorText(null);
  }, [nodes, edges, reactFlowInstance, recomputeValidation]);

  const uploadAndAddFile = useCallback(async (file: File, dropPos?: { x: number; y: number }) => {
    const wfId = loadedWorkflowId ?? props.workflowId;
    if (!wfId)
    {
      setStatusText("Save the workflow first, then add files.");
      return;
    }
    setStatusText(`Uploading '${file.name}'…`);
    try
    {
      const res = await uploadWorkflowFile(wfId, file);
      addFileNode(res.path, dropPos);
    }
    catch (e)
    {
      setErrorText(`Upload failed: ${e instanceof Error ? e.message : String(e)}`);
    }
  }, [loadedWorkflowId, props.workflowId, addFileNode]);

  const triggerFileUpload = useCallback(() => {
    const wfId = loadedWorkflowId ?? props.workflowId;
    if (!wfId)
    {
      setStatusText("Save the workflow first, then add files.");
      return;
    }
    fileUploadInputRef.current?.click();
  }, [loadedWorkflowId, props.workflowId]);

  // "+ File" opens a picker (upload a new file OR add one already in the workflow folder as a node).
  const openFilePicker = useCallback(() => {
    const wfId = loadedWorkflowId ?? props.workflowId;
    if (!wfId)
    {
      setStatusText("Save the workflow first, then add files.");
      return;
    }
    setShowFilePicker(true);
  }, [loadedWorkflowId, props.workflowId]);

  const handleFileUploadSelected = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    e.target.value = ""; // reset so the same file can be re-picked
    if (file) void uploadAndAddFile(file);
  }, [uploadAndAddFile]);

  const onCanvasDragOver = useCallback((e: React.DragEvent) => {
    if (Array.from(e.dataTransfer.types).includes("Files"))
    {
      e.preventDefault();
      e.dataTransfer.dropEffect = "copy";
    }
  }, []);

  const onCanvasDrop = useCallback((e: React.DragEvent) => {
    const files = e.dataTransfer.files;
    if (!files || files.length === 0) return;
    e.preventDefault();
    const dropPos = reactFlowInstance ? reactFlowInstance.project({ x: e.clientX, y: e.clientY }) : undefined;
    void uploadAndAddFile(files[0], dropPos);
  }, [reactFlowInstance, uploadAndAddFile]);

  // Ctrl/Cmd+C / Ctrl/Cmd+V — copy and paste selected task node(s), single or box-selection.
  useEffect(() => {
    const handleCopyPaste = (e: KeyboardEvent) => {
      const isMac = navigator.platform.toUpperCase().indexOf("MAC") >= 0;
      const modifier = isMac ? e.metaKey : e.ctrlKey;
      if (!modifier)
      {
        return;
      }

      // Never hijack native copy/paste while the user is editing text in a field.
      const active = document.activeElement as HTMLElement | null;
      if (active && (active.tagName === "INPUT" || active.tagName === "TEXTAREA"
        || active.tagName === "SELECT" || active.isContentEditable))
      {
        return;
      }

      if (e.key === "c" || e.key === "C")
      {
        // Prefer the tracked multi-selection; fall back to the single inspector selection.
        const idSet = new Set<string>(selectedNodeIds.length > 0
          ? selectedNodeIds
          : (selectedNodeId ? [selectedNodeId] : []));
        const toCopy = (nodes as EditorNode[]).filter((n) => idSet.has(n.id) && n.type === "task") as EditorTaskNode[];
        if (toCopy.length === 0)
        {
          return;
        }
        e.preventDefault();
        // Deep-clone the task so later edits to the originals don't mutate the clipboard.
        clipboardRef.current = toCopy.map((n) => ({
          ...n,
          data: { ...n.data, task: JSON.parse(JSON.stringify(n.data.task)) as JcwfTask },
        }));
        setStatusText(`Copied ${toCopy.length} node${toCopy.length > 1 ? "s" : ""}.`);
      }
      else if (e.key === "v" || e.key === "V")
      {
        const clip = clipboardRef.current;
        if (!clip || clip.length === 0)
        {
          return;
        }
        e.preventDefault();

        const existingIds = new Set<string>((nodes as EditorNode[]).map((n) => n.id));
        const OFFSET = 48;
        const pasted: EditorTaskNode[] = [];
        for (const src of clip)
        {
          const newId = nextId(existingIds, src.data.task.id);
          existingIds.add(newId);
          const task = JSON.parse(JSON.stringify(src.data.task)) as JcwfTask;
          task.id = newId;
          // A pasted copy starts unwired — depends_on references the originals' ids.
          delete task.depends_on;
          const { title, subtitle } = nodeTitleFromTask(task);
          pasted.push({
            id: newId,
            type: "task",
            position: { x: src.position.x + OFFSET, y: src.position.y + OFFSET },
            selected: true,
            data: { task, title, subtitle },
          });
        }

        // Clear the prior selection so only the freshly-pasted nodes are selected.
        const deselected = (nodes as EditorNode[]).map((n) => n.selected ? { ...n, selected: false } : n);
        const graph: EditorGraph = { nodes: [...(deselected as EditorNode[]), ...pasted] as EditorNode[], edges };
        recomputeValidation(graph);
        setSelectedNodeId(pasted.length === 1 ? pasted[0].id : null);
        setSelectedNodeIds(pasted.map((n) => n.id));
        setStatusText(`Pasted ${pasted.length} node${pasted.length > 1 ? "s" : ""}.`);
        setErrorText(null);
      }
    };
    window.addEventListener("keydown", handleCopyPaste);
    return () => window.removeEventListener("keydown", handleCopyPaste);
  }, [nodes, edges, recomputeValidation, selectedNodeId, selectedNodeIds]);

  const addBranchNode = useCallback(() => {
    const existingIds = new Set<string>(nodes.map((n) => n.id));
    let id = "branch_1";
    let counter = 1;
    while (existingIds.has(id))
    {
      counter += 1;
      id = `branch_${counter}`;
    }

    const controlNode: JcwfControlNode = { id, type: "branch", label: "branch" };

    const viewportCenter = reactFlowInstance ? reactFlowInstance.project({
      x: window.innerWidth / 2,
      y: window.innerHeight / 2,
    }) : { x: 0, y: 0 };

    const position = findNonOverlappingPosition(
      viewportCenter.x,
      viewportCenter.y,
      nodes as EditorTaskNode[]
    );

    const newNode: EditorControlNode = {
      id,
      type: "branch",
      position,
      data: { controlNode, title: controlNode.label ?? controlNode.id, subtitle: "branch" },
    };

    const graph: EditorGraph = { nodes: [...nodes, newNode] as EditorNode[], edges };
    recomputeValidation(graph);
    setSelectedNodeId(newNode.id);
    setStatusText(`Added branch '${id}'.`);
    setErrorText(null);
  }, [nodes, edges, reactFlowInstance, recomputeValidation, findNonOverlappingPosition]);

  const addFilterNode = useCallback(() => {
    const existingIds = new Set<string>(nodes.map((n) => n.id));
    let filterId = "filter_1";
    let counter = 1;
    while (existingIds.has(`filter:${filterId}`))
    {
      counter += 1;
      filterId = `filter_${counter}`;
    }

    const filter: JcwfFilter = {
      id: filterId,
      source: { kind: "csv", path: "" },
      binding: "item",
    };

    const viewportCenter = reactFlowInstance ? reactFlowInstance.project({
      x: window.innerWidth / 2,
      y: window.innerHeight / 2,
    }) : { x: 0, y: 0 };

    const position = findNonOverlappingPosition(
      viewportCenter.x,
      viewportCenter.y,
      nodes as EditorTaskNode[]
    );

    const newNode: EditorFilterNode = {
      id: `filter:${filterId}`,
      type: "filter",
      position,
      data: { filter, title: filterId, subtitle: filter.source.kind },
    };

    const graph: EditorGraph = { nodes: [...nodes, newNode] as EditorNode[], edges };
    recomputeValidation(graph);
    setSelectedNodeId(newNode.id);
    setStatusText(`Added filter '${filterId}'.`);
    setErrorText(null);
  }, [nodes, edges, reactFlowInstance, recomputeValidation, findNonOverlappingPosition]);

  const openFilterBuilder = useCallback((filter: JcwfFilter) => {
    editingFilterOriginalIdRef.current = filter.id;
    setEditingFilter(structuredClone(filter));
    setShowFilterBuilder(true);
  }, []);

  const onFilterBuilderSave = useCallback((updatedFilter: JcwfFilter) => {
    setShowFilterBuilder(false);
    setEditingFilter(null);

    // Find the node by the id it had when the dialog opened — the dialog may have renamed the
    // filter id, and matching on the (new) updatedFilter.id would find nothing and drop the edit.
    const originalId = editingFilterOriginalIdRef.current ?? updatedFilter.id;
    const oldNodeId = `filter:${originalId}`;
    const newNodeId = `filter:${updatedFilter.id}`;

    const nextNodes = nodes.map((n) => {
      if (n.type !== "filter") { return n; }
      const filterNode = n as EditorFilterNode;
      if (filterNode.data.filter.id !== originalId && n.id !== oldNodeId)
      {
        return n;
      }
      return {
        ...filterNode,
        id: newNodeId,
        data: {
          ...filterNode.data,
          filter: updatedFilter,
          title: updatedFilter.id,
          subtitle: updatedFilter.source.kind,
        },
      };
    });

    // The node id carries the filter id (jcwfToGraph rebuilds the fanout edge as
    // `filter:<task.filter>` on load), so on a rename re-point any edge touching the old node id.
    const nextEdges = oldNodeId === newNodeId
      ? edges
      : (edges as EditorTaskEdge[]).map((e) => {
        if (e.source !== oldNodeId && e.target !== oldNodeId) { return e; }
        const source = e.source === oldNodeId ? newNodeId : e.source;
        const target = e.target === oldNodeId ? newNodeId : e.target;
        const id = e.id.startsWith("fanout:") ? `fanout:${source}->${target}` : e.id;
        return { ...e, id, source, target };
      });

    const graph: EditorGraph = { nodes: nextNodes as EditorNode[], edges: nextEdges };
    recomputeValidation(graph);
    setIsDirty(true);
  }, [nodes, edges, recomputeValidation]);

  const updateSelectedTaskField = useCallback((patch: Partial<JcwfTask>) => {
    if (!selectedNode)
    {
      return;
    }

    // Just apply the patch to the selected task. file_inputs no longer needs live propagation here:
    // graphToJcwf derives every wired input from its edge at save time (U1), so a wd / file_outputs /
    // PROB-name change is reflected in the saved file_inputs automatically — nothing stored to keep in
    // sync between edits. (This deleted the F-15/F-21 PROB-rename + file_outputs/wd re-sync branches.)
    const nextNodes: EditorTaskNode[] = (nodes as EditorTaskNode[]).map((n) => {
      if (n.id !== selectedNode.id)
      {
        return n;
      }

      const nextTask: JcwfTask = { ...(n.data.task as JcwfTask), ...patch };
      const { title, subtitle } = nodeTitleFromTask(nextTask);

      return {
        ...n,
        data: {
          ...n.data,
          task: nextTask,
          title,
          subtitle,
        },
      };
    });

    recomputeValidation({ nodes: nextNodes, edges });
  }, [selectedNode, nodes, edges, recomputeValidation]);

  const updateSelectedControlNodeField = useCallback((patch: Partial<JcwfControlNode>) => {
    if (!selectedControlNode)
    {
      return;
    }

    const nextNodes = (nodes as EditorNode[]).map((n) => {
      if (n.id !== selectedControlNode.id || n.type !== "branch")
      {
        return n;
      }
      const cn = n as EditorControlNode;
      const nextCn: JcwfControlNode = { ...(cn.data.controlNode), ...patch };
      return {
        ...cn,
        data: {
          ...cn.data,
          controlNode: nextCn,
          title: nextCn.label && nextCn.label.length > 0 ? nextCn.label : nextCn.id,
        },
      };
    });

    recomputeValidation({ nodes: nextNodes as EditorNode[], edges });
  }, [selectedControlNode, nodes, edges, recomputeValidation]);

  const exportJcwfObject = useCallback((): JcwfFile | null => {
    const workflowId = loadedWorkflowId ?? props.workflowId ?? "workflow";
    const graph: EditorGraph = { nodes: nodes as EditorTaskNode[], edges };
    const result = graphToJcwf(graph, workflowId);
    if (!result.ok)
    {
      setErrorText(`${result.message} Cycle nodes: ${result.cycleNodes.join(", ")}`);
      setStatusText("");
      return null;
    }
    setErrorText(null);

    // Start from the original loaded JCWF to preserve workflow-level fields
    // (defaults, base_directory, label, doc, dataflow, etc.) that graphToJcwf
    // does not handle.  Then overwrite graph-derived fields.
    const merged: JcwfFile = {
      ...(loadedJcwfRef.current ?? {}),
      ...result.jcwf,
    };

    // Merge workflow-level metadata managed by editor state.
    if (triggers.length > 0)
    {
      merged.triggers = triggers;
    }
    else
    {
      delete merged.triggers;
    }
    if (!manualStartEnabled)
    {
      merged.manual_start = false;
    }
    else
    {
      delete merged.manual_start;
    }

    if (wfLabel.length > 0) { merged.label = wfLabel; } else { delete merged.label; }
    const docValue = wfDoc.length > 0 ? (wfDoc.includes("\n") ? wfDoc.split("\n") : wfDoc) : undefined;
    if (docValue) { merged.doc = docValue; } else { delete merged.doc; }
    if (wfBaseDirectory.length > 0) { (merged as Record<string, unknown>).base_directory = wfBaseDirectory; } else { delete (merged as Record<string, unknown>).base_directory; }

    const timeoutNum = wfDefaultTimeoutMs.length > 0 ? Number(wfDefaultTimeoutMs) : undefined;
    if (timeoutNum !== undefined || wfDefaultAiProvider.length > 0 || wfDefaultAiModel.length > 0)
    {
      const defs: Record<string, unknown> = { ...((merged as Record<string, unknown>).defaults as Record<string, unknown> ?? {}) };
      if (timeoutNum !== undefined && !isNaN(timeoutNum)) { defs.timeout_ms = timeoutNum; }
      if (wfDefaultAiProvider.length > 0 || wfDefaultAiModel.length > 0)
      {
        const ai: Record<string, unknown> = { ...(defs.ai as Record<string, unknown> ?? {}) };
        if (wfDefaultAiProvider.length > 0) { ai.provider = wfDefaultAiProvider; } else { delete ai.provider; }
        if (wfDefaultAiModel.length > 0) { ai.model = wfDefaultAiModel; } else { delete ai.model; }
        defs.ai = Object.keys(ai).length > 0 ? ai : undefined;
      }
      (merged as Record<string, unknown>).defaults = defs;
    }

    return merged;
  }, [nodes, edges, loadedWorkflowId, props.workflowId, triggers, manualStartEnabled, wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel]);

  const notifyPersisted = useCallback((event: WorkflowPersistEvent) => {
    if (props.onWorkflowPersisted)
    {
      props.onWorkflowPersisted(event);
    }
  }, [props.onWorkflowPersisted]);

  const updateSavedBaseline = useCallback(() => {
    const graphSig = computeGraphSignature(nodes.filter((n): n is EditorTaskNode => n.type === "task") as EditorTaskNode[], edges as EditorTaskEdge[], nodes.filter((n): n is EditorControlNode => n.type === "branch") as EditorControlNode[]);
    const wfFieldsSig = JSON.stringify({
      wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel,
      manualStartEnabled, triggers,
    });
    setLastSavedSignature(graphSig + "|" + wfFieldsSig);
    setLastSavedNodeSnapshot(computeNodeSnapshot(nodes.filter((n): n is EditorTaskNode => n.type === "task") as EditorTaskNode[]));
    setBackendErrors([]);
    setBackendWarnings([]);
    setIsDirty(false);
    if (props.onDirtyStateChange)
    {
      props.onDirtyStateChange(false);
    }
  }, [nodes, edges, wfLabel, wfDoc, wfBaseDirectory, wfDefaultTimeoutMs, wfDefaultAiProvider, wfDefaultAiModel, manualStartEnabled, triggers, props.onDirtyStateChange]);

  const performCreateWorkflow = useCallback(async (newId: string, mode: "create" | "saveAs") => {
    const graph: EditorGraph = { nodes: nodes as EditorTaskNode[], edges };
    const exportResult = graphToJcwf(graph, newId);
    if (!exportResult.ok)
    {
      setErrorText(`${exportResult.message} Cycle nodes: ${exportResult.cycleNodes.join(", ")}`);
      setStatusText("");
      return;
    }

    try
    {
      setStatusText(mode === "create" ? "Creating…" : "Saving As…");
      setErrorText(null);

      const merged: JcwfFile = {
        ...(loadedJcwfRef.current ?? {}),
        ...exportResult.jcwf,
      };
      if (triggers.length > 0)
      {
        merged.triggers = triggers;
      }
      if (!manualStartEnabled)
      {
        merged.manual_start = false;
      }
      const result = await createWorkflowWithId(newId, merged);
      skipFitViewRef.current = true;
      setLoadedWorkflowId(newId);
      props.onWorkflowCreated(newId);
      setStatusText(result.ok ? `${mode === "create" ? "Created" : "Saved as"} '${newId}'.` : `${mode === "create" ? "Create" : "Save As"} returned ok=false for '${newId}'.`);
      updateSavedBaseline();
      notifyPersisted({ kind: mode, workflowId: newId });
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`${mode === "create" ? "Create" : "Save As"} failed: ${message}`);
      setStatusText("");
    }
  }, [nodes, edges, props.onWorkflowCreated, updateSavedBaseline, notifyPersisted]);

  const onSave = useCallback(async () => {
    const jcwf = exportJcwfObject();
    if (!jcwf)
    {
      return;
    }

    // If loadedWorkflowId is set, we can update (PUT) - it exists on backend
    if (loadedWorkflowId)
    {
      try
      {
        setStatusText("Saving…");
        setErrorText(null);

        const result = await saveWorkflow(loadedWorkflowId, jcwf);
        setStatusText(result.ok ? `Saved '${loadedWorkflowId}'.` : `Save returned ok=false for '${loadedWorkflowId}'.`);
        updateSavedBaseline();
        notifyPersisted({ kind: "save", workflowId: loadedWorkflowId });
      }
      catch (e)
      {
        const message = e instanceof Error ? e.message : String(e);
        setErrorText(`Save failed: ${message}`);
        setStatusText("");
      }
    }
    else if (props.workflowId)
    {
      // We have an intended ID (e.g., from template) but it's not saved yet - create it
      await performCreateWorkflow(props.workflowId, "create");
    }
    else
    {
      // No workflow id yet - show modal to get one
      setCreateModalMode("create");
      setShowCreateModal(true);
    }
  }, [loadedWorkflowId, props.workflowId, exportJcwfObject, updateSavedBaseline, notifyPersisted, performCreateWorkflow]);

  const onSaveAs = useCallback(() => {
    setCreateModalMode("saveAs");
    setShowCreateModal(true);
  }, []);

  // Ctrl+S keyboard shortcut
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === "s")
      {
        e.preventDefault();
        void onSave();
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [onSave]);

  const onCreateModalSubmit = useCallback((newId: string) => {
    setShowCreateModal(false);
    void performCreateWorkflow(newId, createModalMode);
  }, [performCreateWorkflow, createModalMode]);

  const onValidate = useCallback(async () => {
    const jcwf = exportJcwfObject();
    if (!jcwf)
    {
      return;
    }

    try
    {
      setStatusText("Validating…");
      setErrorText(null);
      const result = await validateDraft(jcwf);
      setBackendErrors(result.errors);
      setBackendWarnings(result.warnings);
      setBackendInfos(result.infos ?? []);
      recomputeValidation({ nodes: nodes as EditorTaskNode[], edges }, { errors: result.errors, warnings: result.warnings, infos: result.infos ?? [] });

      if (result.ok)
      {
        setStatusText("Validation ok.");
      }
      else
      {
        setStatusText(`Validation failed: ${result.errors.length} error(s), ${result.warnings.length} warning(s).`);
      }
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Validate failed: ${message}`);
      setStatusText("");
    }
  }, [exportJcwfObject, nodes, edges, recomputeValidation]);

  const onRun = useCallback(async () => {
    const workflowId = loadedWorkflowId ?? props.workflowId;
    if (!workflowId)
    {
      setErrorText("No workflow selected.");
      return;
    }

    try
    {
      // Auto-save pending AI scripts (if any) before running
      if (aiPromptAreaRef.current)
      {
        setStatusText("Writing pending scripts…");
        await aiPromptAreaRef.current.flushPendingScripts();
      }

      // Always save before running to ensure backend has the latest version
      setStatusText("Saving before run…");
      await onSave();

      setStatusText("Starting workflow…");
      setErrorText(null);
      const result = await runWorkflow(workflowId);
      if (result.ok && result.runId && result.runId.length > 0)
      {
        setSelectedRunId(result.runId);
        setPendingRunId(result.runId);
        setLastRunResult(null);
        setRuntimeTasksById({});
        const socket = webSocketRef.current;
        if (socket && socket.readyState === WebSocket.OPEN)
        {
          socket.send(JSON.stringify({ type: "workflow-runs-request" }));
        }
      }
      setStatusText(result.ok ? `Run started. runId=${result.runId}` : "Run returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Run failed: ${message}`);
      setStatusText("");
    }
  }, [loadedWorkflowId, props.workflowId, onSave, aiPromptAreaRef]);

  const onClean = useCallback(async () => {
    const workflowId = loadedWorkflowId ?? props.workflowId;
    if (!workflowId)
    {
      setErrorText("No workflow selected.");
      return;
    }

    if (!window.confirm(`Clean all generated outputs for "${workflowId}"?`))
    {
      return;
    }

    try
    {
      setStatusText("Cleaning…");
      setErrorText(null);
      setRuntimeTasksById({});
      setLastRunResult(null);
      const result = await cleanWorkflow(workflowId);
      setStatusText(result.ok
        ? `Clean completed for "${workflowId}".`
        : "Clean returned ok=false.");
      if (result.errors && result.errors.length > 0)
      {
        setErrorText(`Clean warnings: ${result.errors.join("; ")}`);
      }
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Clean failed: ${message}`);
      setStatusText("");
    }
  }, [loadedWorkflowId, props.workflowId]);

  const onCancelRun = useCallback(async () => {
    if (!selectedRunId)
    {
      return;
    }

    try
    {
      setStatusText("Cancelling run\u2026");
      setErrorText(null);
      const result = await cancelRun(selectedRunId);
      setStatusText(result.ok ? `Cancel requested for ${selectedRunId}.` : "Cancel returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Cancel failed: ${message}`);
      setStatusText("");
    }
  }, [selectedRunId]);

  const onPauseRun = useCallback(async () => {
    if (!selectedRunId) { return; }
    try
    {
      setStatusText("Pausing run\u2026");
      setErrorText(null);
      const result = await pauseRun(selectedRunId);
      setStatusText(result.ok ? `Paused ${selectedRunId}.` : "Pause returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Pause failed: ${message}`);
      setStatusText("");
    }
  }, [selectedRunId]);

  const onResumeRun = useCallback(async () => {
    if (!selectedRunId) { return; }
    try
    {
      setStatusText("Resuming run\u2026");
      setErrorText(null);
      const result = await resumeRun(selectedRunId);
      setStatusText(result.ok ? `Resumed ${selectedRunId}.` : "Resume returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Resume failed: ${message}`);
      setStatusText("");
    }
  }, [selectedRunId]);

  const onStopRun = useCallback(async () => {
    if (!selectedRunId) { return; }
    try
    {
      setStatusText("Stopping run\u2026");
      setErrorText(null);
      const result = await stopRun(selectedRunId);
      setStatusText(result.ok ? `Stop requested for ${selectedRunId}.` : "Stop returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Stop failed: ${message}`);
      setStatusText("");
    }
  }, [selectedRunId]);

  const onExportToConsole = useCallback(() => {
    const jcwf = exportJcwfObject();
    if (!jcwf)
    {
      return;
    }
    // eslint-disable-next-line no-console
    console.log("Exported JCWF:", JSON.stringify(jcwf, null, 2));
    setStatusText("Exported to console.");
  }, [exportJcwfObject]);

  const onAutoLayout = useCallback(() => {
    const allNodes = nodes as EditorNode[];
    const currentEdges = edges as EditorTaskEdge[];

    // Separate layoutable nodes (task + branch) from filter nodes (kept in own column)
    const layoutableNodes = allNodes.filter((n) => n.type === "task" || n.type === "branch");
    const filterNodes = allNodes.filter((n) => n.type === "filter");

    if (layoutableNodes.length === 0 && filterNodes.length === 0)
    {
      return;
    }

    // Build dependency map: nodeId -> set of nodes it depends on (from all edge types)
    const dependsOn = new Map<string, Set<string>>();
    for (const node of layoutableNodes)
    {
      dependsOn.set(node.id, new Set());
    }
    for (const edge of currentEdges)
    {
      // Skip filter fanout edges for layout dependencies
      if (edge.id.startsWith("fanout:")) continue;
      const deps = dependsOn.get(edge.target);
      if (deps && dependsOn.has(edge.source))
      {
        deps.add(edge.source);
      }
    }

    // Compute levels using topological sort
    const levels = new Map<string, number>();
    const computeLevel = (nodeId: string, visited: Set<string>): number => {
      if (levels.has(nodeId))
      {
        return levels.get(nodeId)!;
      }
      if (visited.has(nodeId))
      {
        return 0; // Cycle detected, treat as level 0
      }
      visited.add(nodeId);
      const deps = dependsOn.get(nodeId) ?? new Set();
      let maxDepLevel = -1;
      for (const depId of deps)
      {
        maxDepLevel = Math.max(maxDepLevel, computeLevel(depId, visited));
      }
      const level = maxDepLevel + 1;
      levels.set(nodeId, level);
      return level;
    };

    for (const node of layoutableNodes)
    {
      computeLevel(node.id, new Set());
    }

    // Group nodes by level, sorted alphabetically within each level
    const nodesByLevel = new Map<number, EditorNode[]>();
    for (const node of layoutableNodes)
    {
      const level = levels.get(node.id) ?? 0;
      const group = nodesByLevel.get(level) ?? [];
      group.push(node);
      nodesByLevel.set(level, group);
    }
    for (const [, group] of nodesByLevel.entries())
    {
      group.sort((a, b) => a.id.localeCompare(b.id));
    }

    // Position nodes with dynamic heights
    const NODE_WIDTH = 320;
    const BASE_NODE_HEIGHT = 60;
    const DEP_ROW_HEIGHT = 15;
    const HORIZONTAL_GAP = 100;
    const VERTICAL_GAP = 30;

    function estimateNodeHeight(node: EditorNode): number
    {
      if (node.type !== "task") return BASE_NODE_HEIGHT;
      const taskData = (node as EditorTaskNode).data;
      const fileInputs = Array.isArray(taskData.task.file_inputs) ? (taskData.task.file_inputs as string[]).length : 0;
      const deps = Array.isArray(taskData.task.depends_on) ? (taskData.task.depends_on as string[]).length : 0;
      const inputHandleCount = Math.max(fileInputs, deps);
      const fileOutputs = Array.isArray(taskData.task.file_outputs) ? (taskData.task.file_outputs as string[]).length : 0;
      return BASE_NODE_HEIGHT + Math.max(inputHandleCount, fileOutputs) * DEP_ROW_HEIGHT;
    }

    const layoutedNodes: EditorNode[] = [];
    const sortedLevels = Array.from(nodesByLevel.keys()).sort((a, b) => a - b);

    for (const level of sortedLevels)
    {
      const nodesAtLevel = nodesByLevel.get(level) ?? [];
      const x = level * (NODE_WIDTH + HORIZONTAL_GAP) + 50;
      let cumulativeY = 50;

      for (let i = 0; i < nodesAtLevel.length; i++)
      {
        const node = nodesAtLevel[i];
        layoutedNodes.push({
          ...node,
          position: { x, y: cumulativeY },
        });
        cumulativeY += estimateNodeHeight(node) + VERTICAL_GAP;
      }
    }

    // Position filter nodes to the left of the leftmost task column
    const minLevel = sortedLevels.length > 0 ? sortedLevels[0] : 0;
    const filterX = minLevel * (NODE_WIDTH + HORIZONTAL_GAP) + 50 - NODE_WIDTH - HORIZONTAL_GAP;
    for (let i = 0; i < filterNodes.length; i++)
    {
      layoutedNodes.push({
        ...filterNodes[i],
        position: { x: filterX, y: 50 + i * (BASE_NODE_HEIGHT + VERTICAL_GAP) },
      });
    }

    const graph: EditorGraph = { nodes: layoutedNodes, edges: currentEdges };
    recomputeValidation(graph);
    setStatusText("Auto-layout applied.");
  }, [nodes, edges, recomputeValidation]);
  onAutoLayoutRef.current = onAutoLayout;

  return (
    <div className="editorShell">
      <aside className="sidebar">
        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Nodes</div>
          <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
            <button className="btn" type="button" onClick={() => { addTaskNode("ai_call"); }}>+ AI Call</button>
            <button className="btn" type="button" onClick={() => { addTaskNode("python"); }}>+ Python</button>
            <button className="btn" type="button" onClick={() => { addTaskNode("shell"); }}>+ Shell</button>
            <button className="btn" type="button" onClick={() => { addTaskNode("internal"); }}>+ Internal</button>
            <button className="btn" type="button" title="Add a file: upload a new one, pick one already in the workflow, or drag a file onto the canvas" onClick={openFilePicker}>{"\u{1F4C4}"} + File</button>
            <input ref={fileUploadInputRef} type="file" style={{ display: "none" }} onChange={handleFileUploadSelected} />
            <button className="btn" type="button" style={{ borderColor: "rgba(255,180,80,0.35)", color: "rgba(255,200,140,0.95)" }} onClick={addBranchNode}>+ Branch</button>
            <hr style={{ border: "none", borderTop: "1px solid rgba(255,255,255,0.08)", margin: "4px 0" }} />
            <button className="btn" type="button" style={{ borderColor: "rgba(180,140,255,0.35)", color: "rgba(200,170,255,0.95)" }} onClick={addFilterNode}>+ Filter</button>
            <button className="btn" type="button" style={{ borderColor: "rgba(100,200,255,0.35)", color: "rgba(140,220,255,0.95)" }} onClick={() => { addTaskNode("sub_workflow"); }}>{"\u29C9"} + Sub-Workflow</button>
          </div>
        </div>

        <div className="card">
          <WorkflowTreeView
            currentWorkflowId={loadedWorkflowId}
            rootWorkflowId={workflowNavStack.length > 0 ? workflowNavStack[0] : loadedWorkflowId}
            onNavigate={async (workflowId) => {
              if (isDirty)
              {
                const proceed = window.confirm("You have unsaved changes. Navigate anyway?");
                if (!proceed) return;
              }
              if (loadedWorkflowId && loadedWorkflowId !== workflowId)
              {
                setWorkflowNavStack((prev) => [...prev, loadedWorkflowId]);
              }
              try
              {
                const jcwf = await loadWorkflow(workflowId);
                if (jcwf !== null)
                {
                  loadFromJcwfRef.current(workflowId, jcwf);
                  setStatusText(`Navigated to '${workflowId}'.`);
                }
              }
              catch (e)
              {
                setErrorText(`Navigation error: ${e instanceof Error ? e.message : String(e)}`);
              }
            }}
          />
        </div>

        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Triggers</div>
          <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
            <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13 }}>
              <input type="checkbox" checked={manualStartEnabled} onChange={(e) => { setManualStartEnabled(e.target.checked); setIsDirty(true); }} />
              manual_start
            </label>
            {triggers.map((trigger, index) => (
              <div key={trigger.id + index} style={{ border: "1px solid rgba(255,255,255,0.1)", borderRadius: 6, padding: 6 }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 4 }}>
                  <span className="small" style={{ fontWeight: 600 }}>{trigger.type}: {trigger.id}</span>
                  <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 11 }} onClick={() => {
                    setTriggers((prev) => prev.filter((_, i) => i !== index));
                    setIsDirty(true);
                  }}>✕</button>
                </div>
                <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12 }}>
                  <input type="checkbox" checked={trigger.enabled !== false} onChange={(e) => {
                    setTriggers((prev) => prev.map((t, i) => i === index ? { ...t, enabled: e.target.checked } : t));
                    setIsDirty(true);
                  }} />
                  enabled
                </label>
                {trigger.type === "cron" ? (
                  <div style={{ display: "flex", flexDirection: "column", gap: 4, marginTop: 4 }}>
                    <input className="input" placeholder="expression (e.g. 0 8 * * *)" style={{ fontSize: 12, padding: "3px 6px" }}
                      value={(trigger.params?.expression as string) ?? ""}
                      onChange={(e) => {
                        setTriggers((prev) => prev.map((t, i) => i === index ? { ...t, params: { ...t.params, expression: e.target.value } } : t));
                        setIsDirty(true);
                      }} />
                    <input className="input" placeholder="timezone (e.g. America/Los_Angeles)" style={{ fontSize: 12, padding: "3px 6px" }}
                      value={(trigger.params?.timezone as string) ?? ""}
                      onChange={(e) => {
                        const tz = e.target.value;
                        setTriggers((prev) => prev.map((t, i) => i === index ? { ...t, params: { ...t.params, timezone: tz || undefined } } : t));
                        setIsDirty(true);
                      }} />
                  </div>
                ) : null}
                {trigger.type === "file_watch" ? (
                  <div style={{ display: "flex", flexDirection: "column", gap: 4, marginTop: 4 }}>
                    <input className="input" placeholder="path" style={{ fontSize: 12, padding: "3px 6px" }}
                      value={(trigger.params?.path as string) ?? ""}
                      onChange={(e) => {
                        setTriggers((prev) => prev.map((t, i) => i === index ? { ...t, params: { ...t.params, path: e.target.value } } : t));
                        setIsDirty(true);
                      }} />
                  </div>
                ) : null}
                {trigger.type === "webhook" ? (
                  <div style={{ display: "flex", flexDirection: "column", gap: 4, marginTop: 4 }}>
                    <input className="input" placeholder="secret (optional HMAC-SHA256 shared secret)" style={{ fontSize: 12, padding: "3px 6px" }}
                      value={(trigger.params?.secret as string) ?? ""}
                      onChange={(e) => {
                        const secret = e.target.value;
                        setTriggers((prev) => prev.map((t, i) => i === index ? { ...t, params: { ...t.params, secret: secret || undefined } } : t));
                        setIsDirty(true);
                      }} />
                    <div style={{ fontSize: 11, color: "rgba(255,255,255,0.45)", fontFamily: "monospace", wordBreak: "break-all" }}>
                      POST /api/webhook/{loadedWorkflowId ?? props.workflowId ?? "<workflowId>"}
                    </div>
                  </div>
                ) : null}
              </div>
            ))}
            <div style={{ display: "flex", gap: 4, flexWrap: "wrap", marginTop: 4 }}>
              <button className="btn" type="button" style={{ fontSize: 11, padding: "3px 8px" }} onClick={() => {
                setTriggers((prev) => [...prev, { type: "auto", id: "auto", enabled: true }]);
                setIsDirty(true);
              }}>+ auto</button>
              <button className="btn" type="button" style={{ fontSize: 11, padding: "3px 8px" }} onClick={() => {
                setTriggers((prev) => [...prev, { type: "cron", id: `cron-${prev.length + 1}`, enabled: true, params: { expression: "" } }]);
                setIsDirty(true);
              }}>+ cron</button>
              <button className="btn" type="button" style={{ fontSize: 11, padding: "3px 8px" }} onClick={() => {
                setTriggers((prev) => [...prev, { type: "manual", id: "manual", enabled: true }]);
                setIsDirty(true);
              }}>+ manual</button>
              <button className="btn" type="button" style={{ fontSize: 11, padding: "3px 8px" }} onClick={() => {
                setTriggers((prev) => [...prev, { type: "file_watch", id: `file-watch-${prev.length + 1}`, enabled: true, params: { path: "", events: ["modified"] } }]);
                setIsDirty(true);
              }}>+ file_watch</button>
              <button className="btn" type="button" style={{ fontSize: 11, padding: "3px 8px" }} onClick={() => {
                setTriggers((prev) => [...prev, { type: "webhook", id: "webhook", enabled: true, params: {} }]);
                setIsDirty(true);
              }}>+ webhook</button>
            </div>
          </div>
        </div>

        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Workflow</div>
          <div className="small">
            Loaded: <code>{loadedWorkflowId ?? props.workflowId ?? "none"}</code>
            {isDirty ? <span className="dirtyBadge">unsaved</span> : null}
          </div>

          <div style={{ display: "flex", flexDirection: "column", gap: 6, marginTop: 10 }}>
            <label className="field">
              <div className="small">label</div>
              <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={wfLabel} onChange={(e) => { setWfLabel(e.target.value); setIsDirty(true); }} placeholder="Workflow label" />
            </label>
            <label className="field">
              <div className="small">doc</div>
              <textarea className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={wfDoc} onChange={(e) => { setWfDoc(e.target.value); setIsDirty(true); }} placeholder="Workflow description" rows={3} />
            </label>
            <label className="field">
              <div className="small">base_directory</div>
              <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={wfBaseDirectory} onChange={(e) => { setWfBaseDirectory(e.target.value); setIsDirty(true); }} placeholder="(optional)" />
            </label>
            <div style={{ fontWeight: 600, fontSize: 11, opacity: 0.7, marginTop: 4 }}>defaults</div>
            <label className="field">
              <div className="small">timeout_ms</div>
              <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} type="number" value={wfDefaultTimeoutMs} onChange={(e) => { setWfDefaultTimeoutMs(e.target.value); setIsDirty(true); }} placeholder="(ms)" />
            </label>
            <label className="field">
              <div className="small">ai.provider</div>
              <select
                className="input"
                style={{ fontSize: 12, padding: "4px 8px" }}
                value={wfDefaultAiProvider}
                onChange={(e) => {
                  const name = e.target.value;
                  setWfDefaultAiProvider(name);
                  // Auto-fill the model from the chosen interface when the model is still blank,
                  // so picking a provider yields a runnable default without a second selection.
                  if (name && !wfDefaultAiModel)
                  {
                    const iface = aiInterfaces.find((i) => i.name === name);
                    if (iface && iface.model) { setWfDefaultAiModel(iface.model); }
                  }
                  setIsDirty(true);
                }}
              >
                <option value="">(none — global API index)</option>
                {aiInterfaces.map((iface) => (
                  <option key={iface.name} value={iface.name}>{iface.name}</option>
                ))}
                {wfDefaultAiProvider && !aiInterfaces.some((i) => i.name === wfDefaultAiProvider) && (
                  <option value={wfDefaultAiProvider}>{wfDefaultAiProvider} (not configured)</option>
                )}
              </select>
            </label>
            <label className="field">
              <div className="small">ai.model</div>
              <select
                className="input"
                style={{ fontSize: 12, padding: "4px 8px" }}
                value={wfDefaultAiModel}
                onChange={(e) => { setWfDefaultAiModel(e.target.value); setIsDirty(true); }}
              >
                <option value="">(use the provider's model)</option>
                {Array.from(new Set(aiInterfaces.map((i) => i.model).filter((m) => !!m))).map((m) => (
                  <option key={m} value={m}>{m}</option>
                ))}
                {wfDefaultAiModel && !aiInterfaces.some((i) => i.model === wfDefaultAiModel) && (
                  <option value={wfDefaultAiModel}>{wfDefaultAiModel} (custom)</option>
                )}
              </select>
            </label>
          </div>

          <div style={{ display: "flex", flexDirection: "column", gap: 8, marginTop: 10 }}>
            <div style={{ display: "flex", gap: 8 }}>
              <button className="btn" type="button" onClick={undo} disabled={undoStack.length === 0} title="Undo (Ctrl+Z)">
                Undo
              </button>
              <button className="btn" type="button" onClick={redo} disabled={redoStack.length === 0} title="Redo (Ctrl+Shift+Z)">
                Redo
              </button>
            </div>
            <button className="btn" type="button" onClick={onSave}>Save</button>
            <button className="btn" type="button" onClick={onSaveAs}>Save As…</button>
            <button className="btn" type="button" onClick={onValidate}>Validate</button>
            <button className="btn" type="button" onClick={onRun} disabled={(!loadedWorkflowId && !props.workflowId) || !manualStartEnabled} title={!manualStartEnabled ? "manual_start is disabled for this workflow" : undefined}>Run</button>
            <button className="btn" type="button" onClick={onClean} disabled={!loadedWorkflowId && !props.workflowId}>Clean</button>
            <button className="btn" type="button" onClick={() => setShowVersionHistory(true)} disabled={!loadedWorkflowId && !props.workflowId}>History</button>
            <button className="btn" type="button" onClick={onAutoLayout}>Auto Layout</button>
            <button className="btn" type="button" onClick={onExportToConsole}>Export (console)</button>
          </div>

          {statusText ? <div className="small" style={{ marginTop: 10 }}>{statusText}</div> : null}
          {errorText ? <div className="errorText" style={{ marginTop: 10 }}>{errorText}</div> : null}

          {(clientErrors.length > 0 || (clientInfos.length > 0 && !props.hideTierDWarnings))
            ? (
              <div style={{ marginTop: 12 }}>
                <div style={{ fontWeight: 700, marginBottom: 6 }}>Client validation</div>
                {clientErrors.length > 0
                  ? (
                    <ul style={{ margin: 0, paddingLeft: 18, marginBottom: 6 }}>
                      {clientErrors.map((e, idx) => (
                        <li key={`client-err-${idx}`} className="errorText" style={{ marginBottom: 2 }}>
                          <button
                            type="button"
                            className="btnLink"
                            onClick={() => selectNodeById(e.taskId)}
                            style={{ textAlign: "left" }}
                          >
                            {e.message}
                            <span className="small"> (task: <code>{e.taskId}</code>)</span>
                          </button>
                        </li>
                      ))}
                    </ul>
                  )
                  : null}
                {clientInfos.length > 0 && !props.hideTierDWarnings
                  ? (
                    <ul style={{ margin: 0, paddingLeft: 18 }}>
                      {clientInfos.map((i, idx) => (
                        <li key={`client-info-${idx}`} className="infoText" style={{ marginBottom: 2 }}>
                          <button
                            type="button"
                            className="btnLink"
                            onClick={() => selectNodeById(i.taskId)}
                            style={{ textAlign: "left" }}
                          >
                            {i.message}
                            <span className="small"> (task: <code>{i.taskId}</code>)</span>
                          </button>
                        </li>
                      ))}
                    </ul>
                  )
                  : null}
              </div>
            )
            : null}

          {(backendErrors.length > 0 || backendWarnings.length > 0 || (backendInfos.length > 0 && !props.hideTierDWarnings))
            ? (
              <div style={{ marginTop: 12 }}>
                {backendErrors.length > 0
                  ? (
                    <div style={{ marginBottom: 10 }}>
                      <div style={{ fontWeight: 700, marginBottom: 6 }}>Backend errors</div>
                      <ul style={{ margin: 0, paddingLeft: 18 }}>
                        {backendErrors.map((e) => (
                          <li
                            key={`${e.code}:${e.message}`}
                            className={e.tier === "A" ? "errorTextTierA" : "errorText"}
                            style={{ marginBottom: 4 }}
                          >
                            <button
                              type="button"
                              className="btnLink"
                              onClick={() => {
                                if (e.taskId)
                                {
                                  selectNodeById(e.taskId);
                                }
                              }}
                              style={{ textAlign: "left" }}
                            >
                              <code>{e.code}</code>: {e.message}
                              {e.tier ? <span className="small"> (Tier <code>{e.tier}</code>)</span> : null}
                              {e.taskId ? <span className="small"> (task: <code>{e.taskId}</code>)</span> : null}
                              {e.path ? <span className="small"> (path: <code>{e.path}</code>)</span> : null}
                            </button>
                          </li>
                        ))}
                      </ul>
                    </div>
                  )
                  : null}

                {backendWarnings.length > 0
                  ? (
                    <div>
                      <div style={{ fontWeight: 700, marginBottom: 6 }}>Backend warnings</div>
                      <ul style={{ margin: 0, paddingLeft: 18 }}>
                        {backendWarnings.map((w) => (
                          <li key={`${w.code}:${w.message}`} className="warningText" style={{ marginBottom: 4 }}>
                            <button
                              type="button"
                              className="btnLink"
                              onClick={() => {
                                if (w.taskId)
                                {
                                  selectNodeById(w.taskId);
                                }
                              }}
                              style={{ textAlign: "left" }}
                            >
                              <code>{w.code}</code>: {w.message}
                              {w.tier ? <span className="small"> (Tier <code>{w.tier}</code>)</span> : null}
                              {w.taskId ? <span className="small"> (task: <code>{w.taskId}</code>)</span> : null}
                              {w.path ? <span className="small"> (path: <code>{w.path}</code>)</span> : null}
                            </button>
                          </li>
                        ))}
                      </ul>
                    </div>
                  )
                  : null}

                {backendInfos.length > 0 && !props.hideTierDWarnings
                  ? (
                    <div style={{ marginTop: 10 }}>
                      <div style={{ fontWeight: 700, marginBottom: 6 }}>Backend info</div>
                      <ul style={{ margin: 0, paddingLeft: 18 }}>
                        {backendInfos.map((i) => (
                          <li key={`${i.code}:${i.message}`} className="infoText" style={{ marginBottom: 4 }}>
                            <button
                              type="button"
                              className="btnLink"
                              onClick={() => {
                                if (i.taskId)
                                {
                                  selectNodeById(i.taskId);
                                }
                              }}
                              style={{ textAlign: "left" }}
                            >
                              <code>{i.code}</code>: {i.message}
                              {i.tier ? <span className="small"> (Tier <code>{i.tier}</code>)</span> : null}
                              {i.taskId ? <span className="small"> (task: <code>{i.taskId}</code>)</span> : null}
                              {i.path ? <span className="small"> (path: <code>{i.path}</code>)</span> : null}
                            </button>
                          </li>
                        ))}
                      </ul>
                    </div>
                  )
                  : null}
              </div>
            )
            : null}

          <div className="small">
            WebSocket: <span className={isWebSocketConnected ? "runWsConnected" : "runWsDisconnected"}>
              {isWebSocketConnected ? "connected" : "disconnected"}
            </span>
          </div>

          {activeRuns.length === 0
            ? <div className="muted" style={{ marginTop: 8 }}>No active runs.</div>
            : (
              <div style={{ display: "flex", flexDirection: "column", gap: 8, marginTop: 10 }}>
                {activeRuns.map((run) => (
                  <button
                    key={run.runId}
                    className={`btn ${selectedRunId === run.runId ? "btnActive" : ""}`}
                    type="button"
                    onClick={() => { setSelectedRunId(run.runId); }}
                  >
                    <div style={{ fontWeight: 700, textAlign: "left" }}>{run.runId}</div>
                    <div className="small" style={{ textAlign: "left" }}>{run.workflowId} • {run.state}</div>
                  </button>
                ))}
              </div>
            )}

          {selectedRunId && activeRuns.some((r) => r.runId === selectedRunId)
            ? (() => {
              const selectedRun = activeRuns.find((r) => r.runId === selectedRunId);
              const runState = selectedRun?.state ?? "";
              const isPaused = runState === "paused";
              const isRunning = runState === "running";
              return (
                <div style={{ marginTop: 10 }}>
                  <div className="small">Selected: <code>{selectedRunId}</code></div>
                  {isPaused && (
                    <div style={{ marginTop: 6, padding: "4px 8px", background: "rgba(255, 200, 60, 0.15)", border: "1px solid rgba(255, 200, 60, 0.4)", borderRadius: 4, color: "#ffc83c", fontWeight: 700, fontSize: 12, textAlign: "center" }}>
                      ❚❚ PAUSED
                    </div>
                  )}
                  <div style={{ display: "flex", gap: 6, marginTop: 6, flexWrap: "wrap" }}>
                    {isPaused && (
                      <button className="btn" type="button" onClick={onResumeRun} style={{ color: "#5fdd5f", fontWeight: 700 }} title="Resume run">&#9654; Resume</button>
                    )}
                    {isRunning && (
                      <button className="btn" type="button" onClick={onPauseRun} style={{ fontWeight: 700 }} title="Pause after current task finishes">&#10074;&#10074; Pause</button>
                    )}
                    <button className="btn" type="button" onClick={onStopRun} style={{ color: "#ff8a8a" }} title="Stop after current task finishes">&#9632; Stop</button>
                    <button className="btn" type="button" onClick={onCancelRun} style={{ color: "#ff6060", fontSize: 11 }} title="Cancel and kill immediately">Cancel</button>
                  </div>
                </div>
              );
            })()
            : null}
        </div>

        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 6 }}>Delete edge</div>
          <div className="small">
            Click the edge, then press <code>Delete</code> (or <code>Backspace</code>).
          </div>
          {selectedEdgeIds.length > 0 && (
            <button
              className="btn"
              type="button"
              onClick={onDeleteSelectedEdges}
              style={{ marginTop: 8 }}
            >
              Delete selected edge{selectedEdgeIds.length > 1 ? "s" : ""}
            </button>
          )}
        </div>
      </aside>

      <div style={{ display: "flex", flexDirection: "column", height: "100%", overflow: "hidden" }}>
        {workflowNavStack.length > 0 && (
          <div style={{
            display: "flex", alignItems: "center", gap: 6, padding: "4px 10px",
            background: "rgba(100,200,255,0.06)", borderBottom: "1px solid rgba(100,200,255,0.15)",
            fontSize: 12, color: "rgba(140,220,255,0.9)", flexShrink: 0,
          }}>
            <button
              className="btn"
              type="button"
              style={{ fontSize: 11, padding: "2px 8px", borderColor: "rgba(100,200,255,0.3)" }}
              onClick={navigateBack}
            >
              {"\u2190"} Back
            </button>
            {workflowNavStack.map((wfId, idx) => (
              <React.Fragment key={idx}>
                <span style={{ opacity: 0.5 }}>{"\u203A"}</span>
                <span
                  style={{ cursor: "pointer", textDecoration: "underline", opacity: 0.7 }}
                  onClick={async () => {
                    if (isDirty)
                    {
                      const proceed = window.confirm("You have unsaved changes. Navigate anyway?");
                      if (!proceed) return;
                    }
                    setWorkflowNavStack(workflowNavStack.slice(0, idx));
                    try
                    {
                      const jcwf = await loadWorkflow(wfId);
                      if (jcwf !== null) loadFromJcwfRef.current(wfId, jcwf);
                    }
                    catch { /* ignore */ }
                  }}
                >
                  {wfId}
                </span>
              </React.Fragment>
            ))}
            <span style={{ opacity: 0.5 }}>{"\u203A"}</span>
            <span style={{ fontWeight: 700 }}>{loadedWorkflowId ?? "?"}</span>
          </div>
        )}
        <div style={{ flex: 1, position: "relative", minHeight: 0 }} onDrop={onCanvasDrop} onDragOver={onCanvasDragOver}>
          {nodes.length === 0 && (
            <div style={{
              position: "absolute", inset: 0, display: "flex", flexDirection: "column",
              alignItems: "center", justifyContent: "center", gap: 10, pointerEvents: "none",
              textAlign: "center", zIndex: 5, opacity: 0.6,
            }}>
              <div style={{ fontSize: 30 }}>{"✨"}</div>
              <div style={{ fontSize: 16, fontWeight: 700 }}>Start your workflow</div>
              <div style={{ fontSize: 12.5, maxWidth: 340, lineHeight: 1.5 }}>
                Add a node from the <b>Nodes</b> panel on the left — try <b>+ AI Call</b> or <b>+ Shell</b>.
                Then drag from a node{"’"}s output dot into another node to connect them.
              </div>
            </div>
          )}
          <ReactFlow
            key={`rf-${props.hideTierDWarnings ? "hide" : "show"}`}
            nodes={nodes}
            edges={edges}
            nodeTypes={nodeTypes}
            onNodesChange={onNodesChange}
            onNodeDragStart={onNodeDragStart}
            onNodeDragStop={onNodeDragStop}
            onEdgesChange={onEdgesChangeWithUndo}
            onConnect={onConnect}
            connectionMode={ConnectionMode.Loose}
            onSelectionChange={onSelectionChange}
            onNodeDoubleClick={(_event, node) => {
              if (node.type === "task")
              {
                const taskData = (node as EditorTaskNode).data;
                if (taskData.task.type === "sub_workflow" && taskData.task.workflow_file)
                {
                  navigateToSubWorkflow(taskData.task.workflow_file);
                }
              }
            }}
            selectionOnDrag={shiftHeld}
            panOnDrag={!shiftHeld}
            selectionMode={SelectionMode.Partial}
            deleteKeyCode={["Backspace", "Delete"]}
            minZoom={MIN_ZOOM}
            onInit={(instance) => { setReactFlowInstance(instance); instance.fitView(); }}
            onMoveEnd={(_event, viewport) => { setCurrentZoom(viewport.zoom); }}
          >
            <NodeInternalsUpdater signature={slotsSignature} nodeIds={taskNodeIds} />
            <Background />
            <Controls />
            <MiniMap />
          </ReactFlow>
        </div>
        <AiPromptArea
          ref={aiPromptAreaRef}
          getCurrentJcwf={getCurrentJcwf}
          onJcwfGenerated={onJcwfGenerated}
          onScriptsAccepted={onValidate}
          webSocketRef={webSocketRef}
          isWebSocketConnected={isWebSocketConnected}
        />
      </div>

      <aside className="inspector">
        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Inspector</div>

          {selectedFilterNode
            ? (
              <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
                <div className="small" style={{ color: "rgba(200,170,255,0.95)" }}>filter: <code>{selectedFilterNode.data.filter.id}</code></div>
                <div className="small">Source: <code>{selectedFilterNode.data.filter.source.kind}</code></div>
                <div className="small">Binding: <code>{selectedFilterNode.data.filter.binding ?? "item"}</code></div>
                {selectedFilterNode.data.filter.source.path && (
                  <div className="small">Path: <code>{selectedFilterNode.data.filter.source.path}</code></div>
                )}
                {selectedFilterNode.data.filter.source.query && (
                  <div className="small">Query: <code>{selectedFilterNode.data.filter.source.query}</code></div>
                )}
                {selectedFilterNode.data.runtimeItemCount !== undefined && (
                  <div className="small">Items: {selectedFilterNode.data.runtimeItemCount}</div>
                )}
                <button
                  className="btn"
                  type="button"
                  style={{ borderColor: "rgba(180,140,255,0.45)" }}
                  onClick={() => openFilterBuilder(selectedFilterNode.data.filter)}
                >
                  Edit Filter…
                </button>
              </div>
            )
            : selectedControlNode
              ? (
                <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
                  <div className="small" style={{ color: "rgba(255,200,140,0.95)" }}>control node: <code>{selectedControlNode.id}</code></div>
                  <div className="small">Type: <code>{selectedControlNode.data.controlNode.type}</code></div>

                  <label className="field">
                    <div className="small">Label</div>
                    <input
                      className="input"
                      value={(selectedControlNode.data.controlNode.label ?? "") as string}
                      onChange={(e) => { updateSelectedControlNodeField({ label: e.target.value }); }}
                    />
                  </label>

                  <div className="small" style={{ opacity: 0.6, fontSize: 11 }}>
                    Handles: cf-in-normal, cf-in-error (left) · cf-out-normal, cf-out-error (right)
                  </div>

                  <button
                    className="btn"
                    type="button"
                    style={{ borderColor: "rgba(255,100,100,0.4)", color: "#ff8a8a", fontSize: 12, marginTop: 6 }}
                    onClick={() => {
                      const nextNodes = (nodes as EditorNode[]).filter((n) => n.id !== selectedControlNode.id);
                      const nextEdges = edges.filter((e) => e.source !== selectedControlNode.id && e.target !== selectedControlNode.id);
                      setEdges(nextEdges as EditorTaskEdge[]);
                      setSelectedNodeId(null);
                      recomputeValidation({ nodes: nextNodes as EditorNode[], edges: nextEdges as EditorTaskEdge[] });
                    }}
                  >
                    Delete branch node
                  </button>
                </div>
              )
            : !selectedNode
              ? <div className="muted">Select a node to edit its properties.</div>
              : (
                <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
                  <div className="small">taskId: <code>{selectedNode.id}</code></div>

                  {runtimeTasksById[selectedNode.id]
                    ? (
                      <div style={{ padding: 8, background: "rgba(255,255,255,0.04)", borderRadius: 4 }}>
                        <div className="small" style={{ fontWeight: 700, marginBottom: 4 }}>Runtime</div>
                        <div className="small">State: <code>{runtimeTasksById[selectedNode.id].state}</code></div>
                        {runtimeTasksById[selectedNode.id].attemptCount !== undefined
                          ? <div className="small">Attempts: {runtimeTasksById[selectedNode.id].attemptCount}</div>
                          : null}
                        {runtimeTasksById[selectedNode.id].lastErrorMessage
                          ? <div className="errorText small" style={{ marginTop: 4 }}>{runtimeTasksById[selectedNode.id].lastErrorMessage}</div>
                          : null}
                        {runtimeTasksById[selectedNode.id].capturedStderr
                          ? <pre style={{ marginTop: 6, fontSize: 11, color: "#ff8a8a", whiteSpace: "pre-wrap", wordBreak: "break-all", maxHeight: 120, overflow: "auto", background: "rgba(255,80,80,0.06)", borderRadius: 3, padding: 4 }}>{runtimeTasksById[selectedNode.id].capturedStderr}</pre>
                          : null}
                        {runtimeTasksById[selectedNode.id].capturedStdout
                          ? <pre style={{ marginTop: 4, fontSize: 11, color: "rgba(220,230,240,0.9)", whiteSpace: "pre-wrap", wordBreak: "break-all", maxHeight: 120, overflow: "auto", background: "rgba(255,255,255,0.03)", borderRadius: 3, padding: 4 }}>{runtimeTasksById[selectedNode.id].capturedStdout}</pre>
                          : null}
                        {/* Fix Script button: shown when a shell/python task failed with stderr */}
                        {(() => {
                          const snap = runtimeTasksById[selectedNode.id];
                          const taskData = selectedNode.data.task;
                          const taskType = typeof taskData.type === "string" ? taskData.type : "";
                          const isFixable = (taskType === "shell" || taskType === "python");
                          const hasFailed = snap.state === "failed";
                          const hasStderr = typeof snap.capturedStderr === "string" && snap.capturedStderr.length > 0;
                          if (!isFixable || !hasFailed || !hasStderr) return null;

                          const params = taskData.params as Record<string, unknown> | undefined;
                          const scriptPath = taskType === "shell"
                            ? (typeof params?.command === "string" ? params.command : "")
                            : (typeof params?.module === "string" ? params.module : "");
                          if (!scriptPath) return null;

                          return (
                            <button
                              className="btn"
                              type="button"
                              style={{ marginTop: 8, color: "#ffb347", fontWeight: 700, fontSize: 12, width: "100%" }}
                              title={`Send ${scriptPath} + stderr to AI for a fix`}
                              disabled={!isWebSocketConnected}
                              onClick={() => {
                                const socket = webSocketRef.current;
                                if (!socket || socket.readyState !== WebSocket.OPEN) return;
                                socket.send(JSON.stringify({
                                  type: "ai-fix-failed-script",
                                  scriptPath,
                                  stderr: snap.capturedStderr,
                                  taskType,
                                }));
                              }}
                            >
                              Fix Script
                            </button>
                          );
                        })()}
                      </div>
                    )
                    : null}

                  <label className="field">
                    <div className="small">Label</div>
                    <input
                      className="input"
                      value={(selectedNode.data.task.label ?? "") as string}
                      onChange={(e) => { updateSelectedTaskField({ label: e.target.value }); }}
                    />
                  </label>

                  <label className="field">
                    <div className="small">Type</div>
                    <select
                      className="input"
                      value={selectedNode.data.task.type}
                      onChange={(e) => { updateSelectedTaskField({ type: e.target.value as JcwfTaskType }); }}
                    >
                      <option value="ai_call">ai_call</option>
                      <option value="python">python</option>
                      <option value="shell">shell</option>
                      <option value="internal">internal</option>
                      <option value="sub_workflow">sub_workflow</option>
                      <option value="polarion_write">polarion_write</option>
                      <option value="s3">s3</option>
                      <option value="db_query">db_query</option>
                      <option value="onedrive_upload">onedrive_upload</option>
                      <option value="onedrive_download">onedrive_download</option>
                      <option value="snowflake_query">snowflake_query</option>
                      <option value="slack_message">slack_message</option>
                      <option value="email_send">email_send</option>
                      <option value="email_read">email_read</option>
                      <option value="github_issue">github_issue</option>
                      <option value="jira_issue">jira_issue</option>
                      <option value="redmine_issue">redmine_issue</option>
                      <option value="sheets_read">sheets_read</option>
                      <option value="sheets_write">sheets_write</option>
                      <option value="azure_blob_upload">azure_blob_upload</option>
                      <option value="azure_blob_download">azure_blob_download</option>
                      <option value="gcs_upload">gcs_upload</option>
                      <option value="gcs_download">gcs_download</option>
                    </select>
                  </label>

                  <label className="field">
                    <div className="small">Mode</div>
                    <select
                      className="input"
                      value={selectedNode.data.task.mode ?? "single"}
                      onChange={(e) => { updateSelectedTaskField({ mode: e.target.value as JcwfTaskMode }); }}
                    >
                      <option value="single">single</option>
                      <option value="per_item">per_item</option>
                    </select>
                  </label>

                  {selectedNode.data.task.mode === "per_item" && (() => {
                    // Fan-out source. The filter id is an internal handle — pick from the filters
                    // that exist on the canvas rather than re-typing it (the F-35 AI-dropdown pattern;
                    // kills F-38/F-39's "typed the id wrong" class). A value that no longer matches a
                    // filter is preserved as a "(not on canvas)" option so a load never silently drops it.
                    const filterIds = (nodes as EditorNode[])
                      .filter((n): n is EditorFilterNode => n.type === "filter")
                      .map((n) => n.data.filter.id);
                    const current = (selectedNode.data.task.filter ?? "") as string;
                    const currentMissing = current.length > 0 && !filterIds.includes(current);
                    // B: when a filter→task edge implies the binding, the id field is redundant — show a
                    // read-only chip naming the filter (and its source) instead of the picker.
                    const fanoutEdge = (edges as EditorTaskEdge[]).find((e) => e.target === selectedNode.id
                      && classifyEdge(e.sourceHandle, e.targetHandle, e.source, e.target) === "fanout");
                    if (fanoutEdge)
                    {
                      const filterNode = (nodes as EditorNode[]).find((n): n is EditorFilterNode =>
                        n.id === fanoutEdge.source && n.type === "filter");
                      const fid = filterNode?.data.filter.id ?? fanoutEdge.source.replace(/^filter:/, "");
                      const src = filterNode?.data.filter.source;
                      const srcLabel = src ? (src.path ?? src.query ?? src.kind ?? "") : "";
                      return (
                        <div className="field">
                          <div className="small">Fans out over (filter)</div>
                          <div
                            className="small"
                            style={{ marginTop: 2, padding: "4px 8px", borderRadius: 4, background: "rgba(180,140,255,0.14)", border: "1px solid rgba(180,140,255,0.4)", color: "rgba(230,220,255,0.95)" }}
                            title="Driven by the filter → task edge — remove that edge to unbind."
                          >
                            {"⑂"} {fid}{srcLabel ? ` (${srcLabel})` : ""}
                          </div>
                        </div>
                      );
                    }
                    return (
                      <label className="field">
                        <div className="small">Fans out over (filter)</div>
                        <select
                          className="input"
                          value={current}
                          onChange={(e) => { updateSelectedTaskField({ filter: e.target.value || undefined } as Partial<JcwfTask>); }}
                        >
                          <option value="">— select a filter —</option>
                          {filterIds.map((id) => <option key={id} value={id}>{id}</option>)}
                          {currentMissing && <option value={current}>{current} (not on canvas)</option>}
                        </select>
                        {filterIds.length === 0 && (
                          <div className="small" style={{ opacity: 0.7, marginTop: 2 }}>
                            No filter nodes yet — add a filter to the canvas to fan out over its items.
                          </div>
                        )}
                      </label>
                    );
                  })()}

                  {selectedNode.data.task.type === "sub_workflow" && (
                    <>
                      <label className="field">
                        <div className="small" style={{ color: "rgba(100,200,255,0.95)" }}>workflow_file</div>
                        <input
                          className="input"
                          value={(selectedNode.data.task.workflow_file ?? "") as string}
                          onChange={(e) => { updateSelectedTaskField({ workflow_file: e.target.value || undefined } as Partial<JcwfTask>); }}
                          placeholder="e.g. subworkflows/cleanup.jcwf"
                        />
                      </label>
                      {selectedNode.data.task.workflow_file && (
                        <button
                          className="btn"
                          type="button"
                          style={{ borderColor: "rgba(100,200,255,0.35)", color: "rgba(140,220,255,0.95)", fontSize: 12 }}
                          onClick={() => { navigateToSubWorkflow(selectedNode.data.task.workflow_file!); }}
                        >
                          Open Sub-Workflow
                        </button>
                      )}
                    </>
                  )}

                  {selectedNode.data.task.type === "ai_call" && (
                    <label className="field">
                      <div className="small">AI Interface</div>
                      <select
                        className="input"
                        value={(selectedNode.data.task.api_interface as string) ?? ""}
                        onChange={(e) => { updateSelectedTaskField({ api_interface: e.target.value || undefined } as Partial<JcwfTask>); }}
                      >
                        <option value="">default (global API index)</option>
                        {aiInterfaces.map((iface) => (
                          <option key={iface.name} value={iface.name}>{iface.name}</option>
                        ))}
                      </select>
                    </label>
                  )}

                  {selectedNode.data.task.type === "ai_call" && (
                    <StructuredOutputEditor
                      task={selectedNode.data.task}
                      onChange={updateSelectedTaskField}
                    />
                  )}

                  {selectedNode.data.task.type === "internal" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(250,204,21,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(250,204,21,0.95)" }}>Internal task params</div>
                        <label className="field">
                          <div className="small">action</div>
                          <input
                            className="input"
                            list="internalActionList"
                            value={(params.action as string) ?? ""}
                            placeholder="e.g. carMaintenance"
                            onChange={(e) => { updateParams({ action: e.target.value }); }}
                          />
                          <datalist id="internalActionList">
                            <option value="carMaintenance" />
                          </datalist>
                          <div className="small" style={{ opacity: 0.8 }}>
                            Maps to <code>params.action</code> — the registered C++ internal-task factory. Built-in: <code>carMaintenance</code>.
                          </div>
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "polarion_write" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(168,85,247,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(168,85,247,0.95)" }}>Polarion Write params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-polarion" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? ""} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="">select...</option>
                            <option value="update">update</option>
                            <option value="create">create</option>
                            <option value="upload_attachment">upload_attachment</option>
                            <option value="download_attachment">download_attachment</option>
                            <option value="linked_items">linked_items</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">work_item_id</div>
                          <input className="input" value={(params.work_item_id as string) ?? ""} placeholder="e.g. REQ-003 or {{item.work_item_id}}" onChange={(e) => { updateParams({ work_item_id: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">body (JSON:API)</div>
                          <textarea className="input" rows={3} value={(params.body as string) ?? ""} placeholder='{"data":{"type":"workitems","attributes":{...}}}' onChange={(e) => { updateParams({ body: e.target.value }); }} style={{ fontFamily: "monospace", fontSize: 11 }} />
                        </label>
                        <label className="field">
                          <div className="small">attachment_id</div>
                          <input className="input" value={(params.attachment_id as string) ?? ""} onChange={(e) => { updateParams({ attachment_id: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">file_path</div>
                          <input className="input" value={(params.file_path as string) ?? ""} onChange={(e) => { updateParams({ file_path: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">file_name</div>
                          <input className="input" value={(params.file_name as string) ?? ""} onChange={(e) => { updateParams({ file_name: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "s3" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(251,146,60,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(251,146,60,0.95)" }}>S3 params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-s3" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? ""} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="">select...</option>
                            <option value="upload">upload</option>
                            <option value="download">download</option>
                            <option value="list">list</option>
                            <option value="delete">delete</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">bucket (optional, defaults to connection)</div>
                          <input className="input" value={(params.bucket as string) ?? ""} onChange={(e) => { updateParams({ bucket: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">key</div>
                          <input className="input" value={(params.key as string) ?? ""} placeholder="path/to/object.txt" onChange={(e) => { updateParams({ key: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">file_path</div>
                          <input className="input" value={(params.file_path as string) ?? ""} placeholder="local file for upload/download" onChange={(e) => { updateParams({ file_path: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">prefix (for list)</div>
                          <input className="input" value={(params.prefix as string) ?? ""} onChange={(e) => { updateParams({ prefix: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "db_query" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(34,197,94,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(34,197,94,0.95)" }}>Database Query params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-postgres" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">query (SQL)</div>
                          <textarea className="input" rows={4} value={(params.query as string) ?? ""} placeholder="SELECT * FROM users LIMIT 10" onChange={(e) => { updateParams({ query: e.target.value }); }} style={{ fontFamily: "monospace", fontSize: 11 }} />
                        </label>
                        <SqlFilterBuilder query={(params.query as string) ?? ""} onInsert={(clause) => {
                          const current = (params.query as string) ?? "";
                          const upper = current.toUpperCase();
                          const whereIdx = upper.lastIndexOf("WHERE");
                          let updated: string;
                          if (whereIdx >= 0) {
                            const afterWhere = upper.substring(whereIdx + 5);
                            const orderIdx = afterWhere.search(/\b(ORDER\s+BY|GROUP\s+BY|HAVING|LIMIT|OFFSET)\b/);
                            if (orderIdx >= 0) {
                              const insertPos = whereIdx + 5 + orderIdx;
                              updated = current.substring(0, insertPos).trimEnd() + " AND " + clause + " " + current.substring(insertPos).trimStart();
                            } else {
                              updated = current.trimEnd() + " AND " + clause;
                            }
                          } else {
                            const tailIdx = upper.search(/\b(ORDER\s+BY|GROUP\s+BY|HAVING|LIMIT|OFFSET)\b/);
                            if (tailIdx >= 0) {
                              updated = current.substring(0, tailIdx).trimEnd() + " WHERE " + clause + " " + current.substring(tailIdx).trimStart();
                            } else {
                              updated = current.trimEnd() + " WHERE " + clause;
                            }
                          }
                          updateParams({ query: updated });
                        }} />
                        <label className="field">
                          <div className="small">format</div>
                          <select className="input" value={(params.format as string) ?? "csv"} onChange={(e) => { updateParams({ format: e.target.value }); }}>
                            <option value="csv">csv</option>
                            <option value="json">json</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">output_file (optional)</div>
                          <input className="input" value={(params.output_file as string) ?? ""} placeholder="result.csv" onChange={(e) => { updateParams({ output_file: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "onedrive_upload" || selectedNode.data.task.type === "onedrive_download") && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    const isUpload = selectedNode.data.task.type === "onedrive_upload";
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(56,139,253,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(56,139,253,0.95)" }}>OneDrive params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-onedrive" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? (isUpload ? "upload" : "download")} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="upload">upload</option>
                            <option value="download">download</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">remote_path (OneDrive path)</div>
                          <input className="input" value={(params.remote_path as string) ?? ""} placeholder="Documents/reports/output.pdf" onChange={(e) => { updateParams({ remote_path: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">local_path (relative to working directory)</div>
                          <input className="input" value={(params.local_path as string) ?? ""} placeholder="report.pdf" onChange={(e) => { updateParams({ local_path: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "snowflake_query" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(96,165,250,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(96,165,250,0.95)" }}>Snowflake Query params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-snowflake" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">query (SQL)</div>
                          <textarea className="input" rows={4} value={(params.query as string) ?? ""} placeholder="SELECT * FROM sales LIMIT 10" onChange={(e) => { updateParams({ query: e.target.value }); }} style={{ fontFamily: "monospace", fontSize: 11 }} />
                        </label>
                        <label className="field">
                          <div className="small">warehouse (optional, overrides connection)</div>
                          <input className="input" value={(params.warehouse as string) ?? ""} placeholder="COMPUTE_WH" onChange={(e) => { updateParams({ warehouse: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">database (optional, overrides connection)</div>
                          <input className="input" value={(params.database as string) ?? ""} onChange={(e) => { updateParams({ database: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">schema (optional, overrides connection)</div>
                          <input className="input" value={(params.schema as string) ?? ""} onChange={(e) => { updateParams({ schema: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">output_format</div>
                          <select className="input" value={(params.output_format as string) ?? "csv"} onChange={(e) => { updateParams({ output_format: e.target.value }); }}>
                            <option value="csv">csv</option>
                            <option value="json">json</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">output_file (optional)</div>
                          <input className="input" value={(params.output_file as string) ?? ""} placeholder="result.csv" onChange={(e) => { updateParams({ output_file: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "slack_message" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(233,30,99,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(233,30,99,0.95)" }}>Slack Message params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-slack" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">channel</div>
                          <input className="input" value={(params.channel as string) ?? ""} placeholder="#alerts or C01ABCDEF" onChange={(e) => { updateParams({ channel: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">text</div>
                          <textarea className="input" rows={3} value={(params.text as string) ?? ""} placeholder="Workflow completed: {{output}}" onChange={(e) => { updateParams({ text: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "email_send" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(255,152,0,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(255,152,0,0.95)" }}>Email Send params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-email" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">to (comma-separated)</div>
                          <input className="input" value={(params.to as string) ?? ""} placeholder="team@company.com" onChange={(e) => { updateParams({ to: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">subject</div>
                          <input className="input" value={(params.subject as string) ?? ""} placeholder="Report: {{workflow_id}}" onChange={(e) => { updateParams({ subject: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">body</div>
                          <textarea className="input" rows={3} value={(params.body as string) ?? ""} placeholder="See attached report." onChange={(e) => { updateParams({ body: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">cc (optional, comma-separated)</div>
                          <input className="input" value={(params.cc as string) ?? ""} onChange={(e) => { updateParams({ cc: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "email_read" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(255,152,0,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(255,152,0,0.95)" }}>Email Read params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-email" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">folder (default: INBOX)</div>
                          <input className="input" value={(params.folder as string) ?? ""} placeholder="INBOX" onChange={(e) => { updateParams({ folder: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">subject_filter (optional)</div>
                          <input className="input" value={(params.subject_filter as string) ?? ""} placeholder="match subject containing..." onChange={(e) => { updateParams({ subject_filter: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">max_messages (default: 10)</div>
                          <input className="input" type="number" value={(params.max_messages as number) ?? ""} placeholder="10" onChange={(e) => { updateParams({ max_messages: e.target.value ? parseInt(e.target.value) : undefined }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "github_issue" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(110,110,110,0.5)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(200,200,200,0.95)" }}>GitHub Issue params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-github" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? ""} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="">select...</option>
                            <option value="create">create</option>
                            <option value="comment">comment</option>
                            <option value="close">close</option>
                            <option value="get_file">get_file</option>
                            <option value="list_issues">list_issues</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">owner (optional, overrides connection)</div>
                          <input className="input" value={(params.owner as string) ?? ""} onChange={(e) => { updateParams({ owner: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">repo (optional, overrides connection)</div>
                          <input className="input" value={(params.repo as string) ?? ""} onChange={(e) => { updateParams({ repo: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">title (create)</div>
                          <input className="input" value={(params.title as string) ?? ""} onChange={(e) => { updateParams({ title: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">body (create, comment)</div>
                          <textarea className="input" rows={3} value={(params.body as string) ?? ""} onChange={(e) => { updateParams({ body: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">issue_number (comment, close)</div>
                          <input className="input" value={(params.issue_number as string) ?? ""} onChange={(e) => { updateParams({ issue_number: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">path (get_file)</div>
                          <input className="input" value={(params.path as string) ?? ""} placeholder="src/main.cpp" onChange={(e) => { updateParams({ path: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "jira_issue" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(0,130,216,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(0,130,216,0.95)" }}>Jira Issue params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-jira" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? ""} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="">select...</option>
                            <option value="create">create</option>
                            <option value="update">update</option>
                            <option value="transition">transition</option>
                            <option value="comment">comment</option>
                            <option value="get">get</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">project_key (create, overrides connection)</div>
                          <input className="input" value={(params.project_key as string) ?? ""} placeholder="PROJ" onChange={(e) => { updateParams({ project_key: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">summary (create)</div>
                          <input className="input" value={(params.summary as string) ?? ""} onChange={(e) => { updateParams({ summary: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">description (create, optional)</div>
                          <textarea className="input" rows={3} value={(params.description as string) ?? ""} onChange={(e) => { updateParams({ description: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">issue_type (create, default: Task)</div>
                          <input className="input" value={(params.issue_type as string) ?? ""} placeholder="Bug" onChange={(e) => { updateParams({ issue_type: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">issue_key (update, transition, comment, get)</div>
                          <input className="input" value={(params.issue_key as string) ?? ""} placeholder="PROJ-123" onChange={(e) => { updateParams({ issue_key: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">body (comment)</div>
                          <textarea className="input" rows={2} value={(params.body as string) ?? ""} onChange={(e) => { updateParams({ body: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {selectedNode.data.task.type === "redmine_issue" && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(179,0,0,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(179,0,0,0.95)" }}>Redmine Issue params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-redmine" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? ""} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="">select...</option>
                            <option value="list_issues">list_issues</option>
                            <option value="update_issue">update_issue</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">project_identifier (list_issues, overrides connection)</div>
                          <input className="input" value={(params.project_identifier as string) ?? ""} placeholder="j9t-demo" onChange={(e) => { updateParams({ project_identifier: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">issue_id (update_issue)</div>
                          <input className="input" value={(params.issue_id as string) ?? ""} placeholder="1" onChange={(e) => { updateParams({ issue_id: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">notes (update_issue, inline comment)</div>
                          <textarea className="input" rows={2} value={(params.notes as string) ?? ""} onChange={(e) => { updateParams({ notes: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">assigned_to_id (update_issue)</div>
                          <input className="input" value={(params.assigned_to_id as string) ?? ""} placeholder="5" onChange={(e) => { updateParams({ assigned_to_id: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">status (list_issues, default: open)</div>
                          <input className="input" value={(params.status as string) ?? ""} placeholder="open" onChange={(e) => { updateParams({ status: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">limit (list_issues, default: 25)</div>
                          <input className="input" value={(params.limit as string) ?? ""} placeholder="25" onChange={(e) => { updateParams({ limit: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "sheets_read" || selectedNode.data.task.type === "sheets_write") && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    const isWrite = selectedNode.data.task.type === "sheets_write";
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(15,157,88,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(15,157,88,0.95)" }}>Google Sheets params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-sheets" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">spreadsheet_id (optional, overrides connection)</div>
                          <input className="input" value={(params.spreadsheet_id as string) ?? ""} onChange={(e) => { updateParams({ spreadsheet_id: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">range (A1 notation)</div>
                          <input className="input" value={(params.range as string) ?? ""} placeholder="Sheet1!A1:D100" onChange={(e) => { updateParams({ range: e.target.value }); }} />
                        </label>
                        {!isWrite && (
                          <>
                            <label className="field">
                              <div className="small">output_format</div>
                              <select className="input" value={(params.output_format as string) ?? "csv"} onChange={(e) => { updateParams({ output_format: e.target.value }); }}>
                                <option value="csv">csv</option>
                                <option value="json">json</option>
                              </select>
                            </label>
                            <label className="field">
                              <div className="small">output_file (optional)</div>
                              <input className="input" value={(params.output_file as string) ?? ""} placeholder="result.csv" onChange={(e) => { updateParams({ output_file: e.target.value }); }} />
                            </label>
                          </>
                        )}
                        {isWrite && (
                          <>
                            <label className="field">
                              <div className="small">input_file (CSV)</div>
                              <input className="input" value={(params.input_file as string) ?? ""} placeholder="data.csv" onChange={(e) => { updateParams({ input_file: e.target.value }); }} />
                            </label>
                            <label className="field">
                              <div className="small">value_input_option</div>
                              <select className="input" value={(params.value_input_option as string) ?? "USER_ENTERED"} onChange={(e) => { updateParams({ value_input_option: e.target.value }); }}>
                                <option value="USER_ENTERED">USER_ENTERED</option>
                                <option value="RAW">RAW</option>
                              </select>
                            </label>
                          </>
                        )}
                      </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "azure_blob_upload" || selectedNode.data.task.type === "azure_blob_download") && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(0,120,212,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(0,120,212,0.95)" }}>Azure Blob params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-azure-blob" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? (selectedNode.data.task.type === "azure_blob_upload" ? "upload" : "download")} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="upload">upload</option>
                            <option value="download">download</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">container (optional, defaults to connection)</div>
                          <input className="input" value={(params.container as string) ?? ""} onChange={(e) => { updateParams({ container: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">blob_name</div>
                          <input className="input" value={(params.blob_name as string) ?? ""} placeholder="output/report.pdf" onChange={(e) => { updateParams({ blob_name: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">local_path</div>
                          <input className="input" value={(params.local_path as string) ?? ""} placeholder="report.pdf" onChange={(e) => { updateParams({ local_path: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "gcs_upload" || selectedNode.data.task.type === "gcs_download") && (() => {
                    const params = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updateParams = (patch: Record<string, unknown>) => {
                      const merged = { ...params, ...patch };
                      Object.keys(merged).forEach((k) => { if (merged[k] === "" || merged[k] === undefined) delete merged[k]; });
                      updateSelectedTaskField({ params: Object.keys(merged).length > 0 ? merged : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field" style={{ borderLeft: "2px solid rgba(66,133,244,0.4)", paddingLeft: 8 }}>
                        <div className="small" style={{ color: "rgba(66,133,244,0.95)" }}>GCS params</div>
                        <label className="field">
                          <div className="small">connection</div>
                          <input className="input" value={(params.connection as string) ?? ""} placeholder="my-gcs" onChange={(e) => { updateParams({ connection: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">operation</div>
                          <select className="input" value={(params.operation as string) ?? (selectedNode.data.task.type === "gcs_upload" ? "upload" : "download")} onChange={(e) => { updateParams({ operation: e.target.value }); }}>
                            <option value="upload">upload</option>
                            <option value="download">download</option>
                          </select>
                        </label>
                        <label className="field">
                          <div className="small">bucket (optional, defaults to connection)</div>
                          <input className="input" value={(params.bucket as string) ?? ""} onChange={(e) => { updateParams({ bucket: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">object_name</div>
                          <input className="input" value={(params.object_name as string) ?? ""} placeholder="output/report.pdf" onChange={(e) => { updateParams({ object_name: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">local_path</div>
                          <input className="input" value={(params.local_path as string) ?? ""} placeholder="report.pdf" onChange={(e) => { updateParams({ local_path: e.target.value }); }} />
                        </label>
                      </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python" || selectedNode.data.task.type === "ai_call" || selectedNode.data.task.type === "internal") && (() => {
                    // Inputs the upstream tasks wired by an edge into this task actually write —
                    // offered as a datalist dropdown and used to pre-fill "+ file_input" so the
                    // wired file is one click away (same derivation as the edge-draw auto-populate).
                    const incomingSources = (edges as EditorTaskEdge[])
                      .filter((e) => e.id.startsWith("dep:") && e.target === selectedNode.id)
                      .map((e) => ({
                        node: (nodes as EditorNode[]).find((n) => n.id === e.source && n.type === "task") as EditorTaskNode | undefined,
                        handle: e.sourceHandle,
                      }))
                      .filter((x): x is { node: EditorTaskNode; handle: string | null | undefined } => !!x.node);
                    const candidatePaths = Array.from(new Set(
                      incomingSources.flatMap(({ node, handle }) => deriveUpstreamOutputPaths(node.data.task, selectedNode.data.task, loadedWorkflowId ?? props.workflowId ?? "workflow", handle)),
                    )).filter((p) => p.trim().length > 0);
                    const datalistId = `fileInputCandidates-${selectedNode.id}`;
                    const curInputs = Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [];
                    const unusedCandidates = candidatePaths.filter((p) => !curInputs.includes(p));
                    // Area E (U1 display half): a wired input shows its friendly source (the file node or
                    // upstream task it comes from) instead of leaving the user staring at a raw ../../
                    // path. Map each input port (dephandle-N) to the node that feeds it via an edge.
                    const wiredSourceByPort = new Map<number, string>();
                    for (const e of edges as EditorTaskEdge[])
                    {
                      if (e.target !== selectedNode.id) continue;
                      if (typeof e.targetHandle !== "string" || !e.targetHandle.startsWith("dephandle-")) continue;
                      const pi = parseInt(e.targetHandle.slice(10), 10);
                      if (pi < 0) continue;
                      const src = (nodes as EditorNode[]).find((n) => n.id === e.source);
                      if (!src) continue;
                      if (src.type === "file") wiredSourceByPort.set(pi, `\u{1F4C4} ${src.data.title}`);
                      else if (src.type === "task") wiredSourceByPort.set(pi, `← ${src.id}`);
                    }
                    return (
                    <div className="field">
                      <div className="small">file_inputs</div>
                      {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python") && (
                        <div className="small" style={{ opacity: 0.6, fontSize: 10, marginBottom: 4 }}>
                          order matters — maps to {"{{input[0]}}, {{input[1]}}, …"} in args
                        </div>
                      )}
                      {candidatePaths.length > 0 && (
                        <datalist id={datalistId}>
                          {candidatePaths.map((p) => <option key={p} value={p} />)}
                        </datalist>
                      )}
                      {curInputs.map((fi, idx) => {
                        const wiredFrom = wiredSourceByPort.get(idx);
                        return (
                        <div key={`fi-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                          <span style={{
                            display: "inline-block", width: 8, height: 8, borderRadius: "50%",
                            background: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], flexShrink: 0,
                          }} title={`Input ${idx + 1}`} />
                          <span style={{ fontSize: 10, color: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], fontWeight: 600, flexShrink: 0 }}>{idx + 1}</span>
                          {wiredFrom ? (
                            // Wired input — show the friendly source; the synthesized path is in the tooltip.
                            <span
                              style={{ flex: 1, fontSize: 12, padding: "4px 8px", borderRadius: 4, background: "rgba(120,170,210,0.14)", border: "1px solid rgba(120,170,210,0.35)", color: "rgba(220,235,250,0.95)", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}
                              title={`from ${wiredFrom} — ${fi}`}
                            >{wiredFrom}</span>
                          ) : (
                            <FilePathInput
                              style={{ fontSize: 12, padding: "4px 8px", flex: 1 }}
                              value={fi}
                              listId={candidatePaths.length > 0 ? datalistId : undefined}
                              onChange={(v) => {
                                const list = [...(Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [])];
                                list[idx] = v;
                                updateSelectedTaskField({ file_inputs: list } as Partial<JcwfTask>);
                              }}
                            />
                          )}
                          <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                            const list = (Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : []).filter((_, i) => i !== idx);
                            updateSelectedTaskField({ file_inputs: list.length > 0 ? list : undefined } as Partial<JcwfTask>);
                          }}>x</button>
                        </div>
                        );
                      })}
                      <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                        // Pre-fill the new row with the next wired-but-unused upstream output, if any.
                        const list = [...(Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : []), unusedCandidates[0] ?? ""];
                        updateSelectedTaskField({ file_inputs: list } as Partial<JcwfTask>);
                      }}>{unusedCandidates.length > 0 ? `+ file_input (${unusedCandidates.length} wired)` : "+ file_input"}</button>
                    </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python" || selectedNode.data.task.type === "ai_call" || selectedNode.data.task.type === "internal") && (() => {
                    const curOutputs = Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : [];
                    // Suggest a conventional `<stem>.output.<ext>` name from the task's first input — a
                    // one-click default instead of typing an output filename (area G). Hidden once the
                    // suggestion is already present so it never nags.
                    const firstInput = (Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [])
                      .find((p) => p.trim().length > 0);
                    const suggestion = firstInput ? suggestOutputName(firstInput) : "";
                    const showSuggestion = suggestion.length > 0 && !curOutputs.includes(suggestion);
                    return (
                    <div className="field">
                      <div className="small">file_outputs</div>
                      {curOutputs.map((fo, idx) => (
                        <div key={`fo-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                          <FilePathInput
                            style={{ fontSize: 12, padding: "4px 8px", flex: 1 }}
                            value={fo}
                            onChange={(v) => {
                              const list = [...(Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : [])];
                              list[idx] = v;
                              updateSelectedTaskField({ file_outputs: list } as Partial<JcwfTask>);
                            }}
                          />
                          <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                            const list = (Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : []).filter((_, i) => i !== idx);
                            updateSelectedTaskField({ file_outputs: list.length > 0 ? list : undefined } as Partial<JcwfTask>);
                          }}>x</button>
                        </div>
                      ))}
                      <div style={{ display: "flex", gap: 6, alignItems: "center", flexWrap: "wrap", marginTop: 2 }}>
                        <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                          const list = [...curOutputs, ""];
                          updateSelectedTaskField({ file_outputs: list } as Partial<JcwfTask>);
                        }}>+ file_output</button>
                        {showSuggestion && (
                          <button
                            className="btn"
                            type="button"
                            style={{ padding: "3px 8px", fontSize: 11, borderColor: "rgba(100,210,180,0.45)", color: "rgba(140,230,200,0.95)" }}
                            title={`Add ${suggestion} (from input ${firstInput})`}
                            onClick={() => { updateSelectedTaskField({ file_outputs: [...curOutputs, suggestion] } as Partial<JcwfTask>); }}
                          >+ {suggestion}</button>
                        )}
                      </div>
                    </div>
                    );
                  })()}

                  {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python" || selectedNode.data.task.type === "internal") && (() => {
                    const mat = (selectedNode.data.task.materialize ?? {}) as Record<string, string>;
                    const entries = Object.entries(mat);
                    const curFileInputs: string[] = Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [];
                    return (
                      <div className="field">
                        <div className="small">materialize</div>
                        {entries.map(([src, tgt], idx) => {
                          const srcMatch = src.match(/^\{\{input\[(\d+)\]\}\}$/);
                          const srcIdx = srcMatch ? parseInt(srcMatch[1], 10) : -1;
                          const dotColor = srcIdx >= 0 ? FILE_INPUT_COLORS[srcIdx % FILE_INPUT_COLORS.length] : "#888";
                          return (
                          <div key={`mat-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                            <span style={{
                              display: "inline-block", width: 8, height: 8, borderRadius: "50%",
                              background: dotColor, flexShrink: 0,
                            }} />
                            {curFileInputs.length > 0 ? (
                              <select
                                className="input"
                                style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                                value={src}
                                onChange={(e) => {
                                  const newMat: Record<string, string> = {};
                                  entries.forEach(([k, v], i) => { newMat[i === idx ? e.target.value : k] = v; });
                                  updateSelectedTaskField({ materialize: Object.keys(newMat).length > 0 ? newMat : undefined } as Partial<JcwfTask>);
                                }}
                              >
                                <option value="">— select input —</option>
                                {curFileInputs.map((fiPath, fiIdx) => {
                                  const segments = fiPath.split("/");
                                  const shortName = segments[segments.length - 1] || fiPath;
                                  return (
                                    <option key={fiIdx} value={`{{input[${fiIdx}]}}`}>
                                      Input {fiIdx + 1}: {shortName}
                                    </option>
                                  );
                                })}
                              </select>
                            ) : (
                              <input
                                className="input"
                                style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                                value={src}
                                placeholder="source e.g. {{input[0]}}"
                                onChange={(e) => {
                                  const newMat: Record<string, string> = {};
                                  entries.forEach(([k, v], i) => { newMat[i === idx ? e.target.value : k] = v; });
                                  updateSelectedTaskField({ materialize: Object.keys(newMat).length > 0 ? newMat : undefined } as Partial<JcwfTask>);
                                }}
                              />
                            )}
                            <span style={{ color: "#888", fontSize: 11 }}>→</span>
                            <input
                              className="input"
                              style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                              value={tgt}
                              placeholder="target e.g. hello.c"
                              onChange={(e) => {
                                const newMat: Record<string, string> = {};
                                entries.forEach(([k, v], i) => { newMat[k] = i === idx ? e.target.value : v; });
                                updateSelectedTaskField({ materialize: Object.keys(newMat).length > 0 ? newMat : undefined } as Partial<JcwfTask>);
                              }}
                            />
                            <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                              const newMat: Record<string, string> = {};
                              entries.forEach(([k, v], i) => { if (i !== idx) newMat[k] = v; });
                              updateSelectedTaskField({ materialize: Object.keys(newMat).length > 0 ? newMat : undefined } as Partial<JcwfTask>);
                            }}>x</button>
                          </div>
                          );
                        })}
                        <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                          const newMat: Record<string, string> = { ...mat, "": "" };
                          updateSelectedTaskField({ materialize: newMat } as Partial<JcwfTask>);
                        }}>+ materialize</button>
                      </div>
                    );
                  })()}

                  {(() => {
                    const uniqueDeps = Array.from(new Set(
                      (edges as EditorTaskEdge[])
                        .filter((e) => e.id.startsWith("dep:") && e.target === selectedNode.id)
                        .map((e) => e.source),
                    ));
                    return (
                      <div className="field">
                        <div className="small" style={{ fontWeight: 600 }}>depends_on (from edges)</div>
                        {uniqueDeps.length > 0 ? (
                          <div className="small" style={{ display: "flex", flexWrap: "wrap", gap: 4, marginTop: 2 }}>
                            {uniqueDeps.map((d) => (
                              <span key={d} style={{ background: "rgba(255,255,255,0.08)", borderRadius: 4, padding: "1px 6px" }}>{d}</span>
                            ))}
                          </div>
                        ) : (
                          <div className="small" style={{ opacity: 0.7, marginTop: 2 }}>
                            None. Drag from a task&apos;s right edge to this task&apos;s left edge to set run-order.
                          </div>
                        )}
                      </div>
                    );
                  })()}

                  {(() => {
                    const dfTask = selectedNode.data.task;
                    const inputs = (dfTask.inputs ?? {}) as Record<string, { type?: string; required?: boolean }>;
                    const outputs = (dfTask.outputs ?? {}) as Record<string, { type?: string; required?: boolean }>;
                    const inputEntries = Object.entries(inputs);
                    const outputEntries = Object.entries(outputs);
                    const hasSlots = inputEntries.length > 0 || outputEntries.length > 0;

                    const writeSlots = (key: "inputs" | "outputs", obj: Record<string, unknown>) => {
                      updateSelectedTaskField({ [key]: Object.keys(obj).length > 0 ? obj : undefined } as Partial<JcwfTask>);
                    };

                    const renderSlotList = (
                      key: "inputs" | "outputs",
                      entries: Array<[string, { type?: string; required?: boolean }]>,
                    ) => (
                      <div style={{ marginTop: 2 }}>
                        {entries.map(([name, spec], idx) => (
                          <div key={`${key}-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center", marginBottom: 3 }}>
                            <span style={{
                              display: "inline-block", width: 8, height: 8, borderRadius: "50%",
                              background: "rgba(100,210,180,0.85)", flexShrink: 0,
                            }} />
                            <input
                              className="input"
                              style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                              value={name}
                              placeholder={key === "outputs" ? "output slot name" : "input slot name"}
                              onChange={(e) => {
                                const rebuilt: Record<string, unknown> = {};
                                entries.forEach(([k, v], i) => { rebuilt[i === idx ? e.target.value : k] = v; });
                                writeSlots(key, rebuilt);
                              }}
                            />
                            <select
                              className="input"
                              style={{ fontSize: 11, padding: "3px 4px", width: 72 }}
                              value={spec?.type ?? "string"}
                              onChange={(e) => {
                                const rebuilt: Record<string, unknown> = {};
                                entries.forEach(([k, v], i) => { rebuilt[k] = i === idx ? { ...v, type: e.target.value } : v; });
                                writeSlots(key, rebuilt);
                              }}
                            >
                              <option value="string">string</option>
                              <option value="object">object</option>
                              <option value="number">number</option>
                            </select>
                            <label className="small" style={{ display: "flex", alignItems: "center", gap: 3 }}>
                              <input
                                type="checkbox"
                                checked={spec?.required === true}
                                onChange={(e) => {
                                  const rebuilt: Record<string, unknown> = {};
                                  entries.forEach(([k, v], i) => {
                                    rebuilt[k] = i === idx
                                      ? { type: v?.type ?? "string", ...(e.target.checked ? { required: true } : {}) }
                                      : v;
                                  });
                                  writeSlots(key, rebuilt);
                                }}
                              /> req
                            </label>
                            <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                              const rebuilt: Record<string, unknown> = {};
                              entries.forEach(([k, v], i) => { if (i !== idx) rebuilt[k] = v; });
                              writeSlots(key, rebuilt);
                            }}>x</button>
                          </div>
                        ))}
                        <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                          const rebuilt: Record<string, unknown> = {};
                          entries.forEach(([k, v]) => { rebuilt[k] = v; });
                          const stem = key === "outputs" ? "out" : "in";
                          let n = entries.length + 1;
                          let nm = `${stem}${n}`;
                          while (rebuilt[nm] !== undefined) { n++; nm = `${stem}${n}`; }
                          rebuilt[nm] = { type: "string" };
                          writeSlots(key, rebuilt);
                        }}>+ {key === "outputs" ? "output" : "input"} slot</button>
                      </div>
                    );

                    return (
                      <details className="field" style={{ borderLeft: "2px solid rgba(100,210,180,0.4)", paddingLeft: 8 }} open={hasSlots}>
                        <summary style={{ cursor: "pointer", fontSize: 12, color: "rgba(100,210,180,0.95)" }}>
                          Named value inputs/outputs — advanced{hasSlots ? " ✓" : ""}
                        </summary>
                        <div className="small" style={{ marginTop: 6, opacity: 0.8 }}>
                          For passing a <em>value</em> to a script parameter — not a file. Declare a named output on the
                          producer and a matching named input on the consumer; both show as round ports on the node face,
                          and drawing an output→input wire passes the value. Most workflows use file_inputs/file_outputs
                          instead and never need this.
                        </div>
                        <div className="small" style={{ marginTop: 6, fontWeight: 600 }}>outputs</div>
                        {renderSlotList("outputs", outputEntries)}
                        <div className="small" style={{ marginTop: 6, fontWeight: 600 }}>inputs</div>
                        {renderSlotList("inputs", inputEntries)}
                      </details>
                    );
                  })()}

                  {selectedNode.data.task.type === "shell" ? (() => {
                    const shellParams = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const command = (shellParams.command ?? "") as string;
                    const args: string[] = Array.isArray(shellParams.args) ? shellParams.args as string[] : [];
                    const curFileInputs: string[] = Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [];
                    const curFileOutputs: string[] = Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : [];

                    const updateShellParams = (patch: Record<string, unknown>) => {
                      const next = { ...shellParams, ...patch };
                      if (!next.command) delete next.command;
                      if (Array.isArray(next.args) && (next.args as unknown[]).length === 0) delete next.args;
                      updateSelectedTaskField({ params: Object.keys(next).length > 0 ? next : undefined } as Partial<JcwfTask>);
                    };

                    return (
                      <div className="field">
                        <div className="small" style={{ fontWeight: 600 }}>params (shell)</div>
                        <label className="field" style={{ marginTop: 4 }}>
                          <div className="small">command</div>
                          <input
                            className="input"
                            style={{ fontSize: 12, padding: "4px 8px" }}
                            value={command}
                            onChange={(e) => updateShellParams({ command: e.target.value })}
                            onKeyDown={(e) => {
                              if (e.key === "Tab" && command.trim().length === 0)
                              {
                                e.preventDefault();
                                updateShellParams({ command: e.currentTarget.placeholder });
                              }
                            }}
                            placeholder="scripts/myScript.sh"
                          />
                        </label>
                        <div style={{ marginTop: 6 }}>
                          <div className="small">args</div>
                          {args.map((arg, idx) => {
                            const isFileRef = arg.startsWith("{{input[") || arg.startsWith("{{output[");
                            return (
                              <div key={`arg-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center", marginTop: 2 }}>
                                {(curFileInputs.length > 0 || curFileOutputs.length > 0) ? (
                                  <select
                                    className="input"
                                    style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                                    value={isFileRef ? arg : "__literal__"}
                                    onChange={(e) => {
                                      const newArgs = [...args];
                                      newArgs[idx] = e.target.value === "__literal__" ? "" : e.target.value;
                                      updateShellParams({ args: newArgs });
                                    }}
                                  >
                                    <option value="__literal__">— literal string —</option>
                                    {curFileInputs.map((_, fiIdx) => {
                                      const fiSegs = curFileInputs[fiIdx].split("/");
                                      const fiName = fiSegs[fiSegs.length - 1] || curFileInputs[fiIdx];
                                      return (
                                        <option key={`fi-${fiIdx}`} value={`{{input[${fiIdx}]}}`}>
                                          input[{fiIdx}]: {fiName}
                                        </option>
                                      );
                                    })}
                                    {curFileOutputs.map((_, foIdx) => {
                                      const foSegs = curFileOutputs[foIdx].split("/");
                                      const foName = foSegs[foSegs.length - 1] || curFileOutputs[foIdx];
                                      return (
                                        <option key={`fo-${foIdx}`} value={`{{output[${foIdx}]}}`}>
                                          output[{foIdx}]: {foName}
                                        </option>
                                      );
                                    })}
                                  </select>
                                ) : null}
                                {(!isFileRef || curFileInputs.length === 0 && curFileOutputs.length === 0) && (
                                  <input
                                    className="input"
                                    style={{ fontSize: 11, padding: "4px 6px", flex: 1 }}
                                    value={arg}
                                    onChange={(e) => {
                                      const newArgs = [...args];
                                      newArgs[idx] = e.target.value;
                                      updateShellParams({ args: newArgs });
                                    }}
                                    placeholder="e.g. --verbose"
                                  />
                                )}
                                <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                                  const newArgs = args.filter((_, i) => i !== idx);
                                  updateShellParams({ args: newArgs });
                                }}>x</button>
                              </div>
                            );
                          })}
                          <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11, marginTop: 4 }} onClick={() => {
                            updateShellParams({ args: [...args, ""] });
                          }}>+ arg</button>
                        </div>
                      </div>
                    );
                  })() : selectedNode.data.task.type === "python" ? (() => {
                    const pyParams = (selectedNode.data.task.params ?? {}) as Record<string, unknown>;
                    const updatePyParams = (patch: Record<string, unknown>) => {
                      const next = { ...pyParams, ...patch };
                      Object.keys(next).forEach((k) => { if (next[k] === "" || next[k] === undefined) delete next[k]; });
                      updateSelectedTaskField({ params: Object.keys(next).length > 0 ? next : undefined } as Partial<JcwfTask>);
                    };
                    return (
                      <div className="field">
                        <div className="small" style={{ fontWeight: 600 }}>params (python)</div>
                        <label className="field" style={{ marginTop: 4 }}>
                          <div className="small">module</div>
                          <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={(pyParams.module as string) ?? ""} placeholder="e.g. printFileInfo" onChange={(e) => { updatePyParams({ module: e.target.value }); }} />
                        </label>
                        <label className="field">
                          <div className="small">function</div>
                          <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={(pyParams.function as string) ?? ""} placeholder="e.g. get_file_info" onChange={(e) => { updatePyParams({ function: e.target.value }); }} />
                        </label>
                        <div className="small" style={{ opacity: 0.7, marginTop: 2 }}>
                          <code>module</code> resolves a Python module on the engine path; <code>function</code> receives the task inputs.
                        </div>
                      </div>
                    );
                  })() : null}

                  {selectedNode.data.task.type === "ai_call" && selectedNode.data.task.mode === "per_item"
                    && typeof selectedNode.data.task.filter === "string" && selectedNode.data.task.filter.length > 0
                    && (() => {
                      const fid = selectedNode.data.task.filter as string;
                      const filterNode = (nodes as EditorNode[]).find((n): n is EditorFilterNode =>
                        n.type === "filter" && n.data.filter.id === fid);
                      if (!filterNode) return null;
                      return (
                        <FanoutBuilder
                          filter={filterNode.data.filter}
                          workflowId={loadedWorkflowId ?? props.workflowId ?? null}
                          onApply={(entry) => {
                            const qb = { ...((selectedNode.data.task.queue_binding ?? {}) as JcwfQueueBinding) };
                            qb.prob_files = [entry];
                            updateSelectedTaskField({ queue_binding: qb } as Partial<JcwfTask>);
                            setStatusText(`Fan-out prompt applied — one AI call per row of '${fid}'.`);
                          }}
                        />
                      );
                    })()}

                  {selectedNode.data.task.type === "ai_call" && (
                    <QueueBindingEditor
                      queueBinding={selectedNode.data.task.queue_binding as JcwfQueueBinding | undefined}
                      onChange={(binding) => { updateSelectedTaskField({ queue_binding: binding } as Partial<JcwfTask>); }}
                      templateVariables={templateVariables}
                    />
                  )}

                  {/* Advanced — fields rarely or never touched in a typical workflow. Collapsed by
                      default so the essentials above aren't buried under a wall of inputs (U5). */}
                  <details className="field" style={{ borderLeft: "2px solid rgba(255,255,255,0.15)", paddingLeft: 8 }}>
                    <summary style={{ cursor: "pointer", fontSize: 12, opacity: 0.85 }}>Advanced</summary>

                    <label className="field" style={{ marginTop: 6 }}>
                      <div className="small">
                        {selectedNode.data.task.type === "ai_call"
                          ? <>working_directory <span style={{ opacity: 0.5 }}>(optional, tab to accept)</span></>
                          : <>working_directory relative to jcwf <span style={{ opacity: 0.5 }}>(tab to accept)</span></>}
                      </div>
                      <FilePathInput
                        value={(selectedNode.data.task.working_directory ?? "") as string}
                        onChange={(v) => { updateSelectedTaskField({ working_directory: v }); }}
                        onKeyDown={(e) => {
                          const cur = ((selectedNode.data.task.working_directory ?? "") as string).trim();
                          if (e.key === "Tab" && cur.length === 0)
                          {
                            e.preventDefault();
                            const el = e.currentTarget;
                            updateSelectedTaskField({ working_directory: el.placeholder });
                          }
                        }}
                        placeholder={
                          selectedNode.data.task.type === "ai_call"
                            ? (() => {
                              const wfId = loadedWorkflowId ?? props.workflowId ?? "workflowId";
                              const aiNodes = (nodes.filter((n) => n.type === "task") as EditorTaskNode[]).filter((n) => n.data.task.type === "ai_call");
                              const idx = aiNodes.findIndex((n) => n.id === selectedNode.id);
                              const num = String(idx >= 0 ? idx + 1 : aiNodes.length + 1).padStart(2, "0");
                              return `../queue/${wfId}/${num}_${selectedNode.id}`;
                            })()
                            : `${loadedWorkflowId ?? props.workflowId ?? "workflowId"}/01_taskName`
                        }
                      />
                      {(() => {
                        // Friendly "runs in" hint (area E): show where the task actually runs — resolved
                        // through the same base-leaf-strip the runtime uses — so the user reads a real
                        // location instead of decoding a raw ../ traversal. Empty wd ⇒ the workflow folder.
                        const wfId = loadedWorkflowId ?? props.workflowId ?? "workflowId";
                        const wd = ((selectedNode.data.task.working_directory ?? "") as string).trim();
                        const resolved = resolveTaskDirSegments(wfId, wd).join("/");
                        return (
                          <div className="small" style={{ opacity: 0.7, marginTop: 2 }}>
                            runs in: <code>{resolved}/</code>{wd.length === 0 ? " (the workflow folder — leave blank for this)" : ""}
                          </div>
                        );
                      })()}
                    </label>

                    {selectedNode.data.task.type !== "shell" && selectedNode.data.task.type !== "python" && (
                      <label className="field" style={{ marginTop: 6 }}>
                        <div className="small">params (JSON)</div>
                        <textarea
                          className="input codeInput"
                          value={JSON.stringify(selectedNode.data.task.params ?? {}, null, 2)}
                          onChange={(e) => {
                            try
                            {
                              const parsed = JSON.parse(e.target.value) as Record<string, unknown>;
                              updateSelectedTaskField({ params: parsed });
                              setErrorText(null);
                            }
                            catch
                            {
                              setErrorText("params is not valid JSON.");
                            }
                          }}
                          rows={8}
                        />
                      </label>
                    )}

                    <label className="field" style={{ marginTop: 6 }}>
                      <div className="small">timeout_ms</div>
                      <input
                        className="input"
                        type="number"
                        style={{ fontSize: 12, padding: "4px 8px" }}
                        value={selectedNode.data.task.timeout_ms !== undefined ? String(selectedNode.data.task.timeout_ms) : ""}
                        onChange={(e) => {
                          const raw = e.target.value;
                          const val = raw.length > 0 ? Math.max(0, Number(raw)) : undefined;
                          updateSelectedTaskField({ timeout_ms: (val !== undefined && isNaN(val)) ? undefined : val } as Partial<JcwfTask>);
                        }}
                        min={0}
                        placeholder="(inherits workflow default)"
                      />
                    </label>

                    <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12, marginTop: 8 }}>
                      <input
                        type="checkbox"
                        checked={selectedNode.data.task.expose_error_signal === true}
                        onChange={(e) => { updateSelectedTaskField({ expose_error_signal: e.target.checked || undefined } as Partial<JcwfTask>); }}
                      />
                      Expose error signal output
                    </label>

                    <label className="field" style={{ marginTop: 8 }}>
                      <div className="small">doc</div>
                      <textarea
                        className="input"
                        value={typeof selectedNode.data.task.doc === "string" ? selectedNode.data.task.doc : ""}
                        onChange={(e) => { updateSelectedTaskField({ doc: e.target.value }); }}
                        placeholder="(optional, string)"
                        rows={4}
                      />
                    </label>
                  </details>

                  {selectedNode.data.validationErrors && selectedNode.data.validationErrors.length > 0
                    ? (
                      <div className="errorText">
                        {selectedNode.data.validationErrors.join(" ")}
                      </div>
                    )
                    : null}

                  {selectedNode.data.validationWarnings && selectedNode.data.validationWarnings.length > 0
                    ? (
                      <div className="warningText">
                        {selectedNode.data.validationWarnings.join(" ")}
                      </div>
                    )
                    : null}

                  {selectedNode.data.validationInfos && selectedNode.data.validationInfos.length > 0
                    ? (
                      <div className="infoText">
                        {selectedNode.data.validationInfos.join(" ")}
                      </div>
                    )
                    : null}

                </div>
              )}
        </div>

        <div className="card" style={{ marginTop: 8 }}>
          <div style={{ fontWeight: 700, marginBottom: 4 }}>Debug</div>
          <div className="small">Zoom: {(currentZoom * 100).toFixed(0)}%</div>
          <div className="small">Nodes: {nodes.length}</div>
          <div className="small">Edges: {edges.length}</div>
        </div>
      </aside>

      <CreateWorkflowModal
        isOpen={showCreateModal}
        defaultId={createModalMode === "saveAs" ? `${loadedWorkflowId ?? props.workflowId ?? "workflow"}_copy` : "workflow"}
        title={createModalMode === "saveAs" ? "Save As…" : "Create Workflow"}
        submitLabel={createModalMode === "saveAs" ? "Save As" : "Create"}
        onCancel={() => setShowCreateModal(false)}
        onSubmit={onCreateModalSubmit}
      />

      {editingFilter && (
        <FilterBuilderDialog
          isOpen={showFilterBuilder}
          filter={editingFilter}
          onSave={onFilterBuilderSave}
          onCancel={() => { setShowFilterBuilder(false); setEditingFilter(null); }}
        />
      )}

      {showFilePicker && (loadedWorkflowId || props.workflowId) && (
        <FilePickerDialog
          workflowId={loadedWorkflowId ?? props.workflowId!}
          canvasFileName={`${loadedWorkflowId ?? props.workflowId!}.json`}
          existingFileNodeRelPaths={new Set(
            (nodes as EditorNode[]).filter((n): n is EditorFileNode => n.type === "file").map((n) => n.data.workflowRelPath),
          )}
          onPick={(relPath) => { addFileNode(relPath); }}
          onUploadNew={triggerFileUpload}
          onClose={() => { setShowFilePicker(false); }}
        />
      )}

      {showVersionHistory && (loadedWorkflowId || props.workflowId) && (
        <VersionHistoryModal
          workflowId={loadedWorkflowId ?? props.workflowId!}
          onClose={() => setShowVersionHistory(false)}
          onRestored={() => {
            setShowVersionHistory(false);
            window.location.reload();
          }}
        />
      )}
    </div>
  );
}
