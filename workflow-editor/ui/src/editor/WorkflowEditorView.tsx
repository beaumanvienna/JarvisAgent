import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  addEdge,
  applyNodeChanges,
  useEdgesState,
  useNodesState,
  type Connection,
  type Node,
  type Edge,
  type ReactFlowInstance,
} from "reactflow";
import "reactflow/dist/style.css";

import TaskNode from "./TaskNode";
import FilterNode from "./FilterNode";
import BranchNode from "./BranchNode";
import { FILE_INPUT_COLORS } from "./constants";
import FilterBuilderDialog from "./FilterBuilderDialog";
import QueueBindingEditor from "./QueueBindingEditor";
import { jcwfToGraph } from "./jcwfToGraph";
import { graphToJcwf } from "./graphToJcwf";
import { validateGraph } from "./validation";
import type { EditorControlNode, EditorControlNodeData, EditorFilterNode, EditorFilterNodeData, EditorGraph, EditorNode, EditorTaskEdge, EditorTaskNode, EditorTaskNodeData, RuntimeTaskState } from "./types";
import type { JcwfControlNode, JcwfFile, JcwfFilter, JcwfFilterSource, JcwfQueueBinding, JcwfTask, JcwfTaskMode, JcwfTaskType, JcwfTrigger } from "../jcwf/types";
import {
  cancelRun,
  cleanWorkflow,
  createWorkflowWithId,
  checkScript,
  fetchRunDetails,
  loadWorkflow,
  pauseRun,
  resumeRun,
  runWorkflow,
  saveWorkflow,
  stopRun,
  validateDraft,
  type WorkflowValidationFinding,
} from "../api/workflows";
import CreateWorkflowModal from "../components/CreateWorkflowModal";
import { listAiInterfaces, type AiInterface } from "../api/aiInterfaces";
import AiPromptArea from "./AiPromptArea";

const nodeTypes = { task: TaskNode, filter: FilterNode, branch: BranchNode };

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
  return {
    id: taskId,
    type: taskType,
    label: taskId,
    params: {},
  };
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

  const [statusText, setStatusText] = useState<string>("");
  const [errorText, setErrorText] = useState<string | null>(null);
  const [backendErrors, setBackendErrors] = useState<WorkflowValidationFinding[]>([]);
  const [backendWarnings, setBackendWarnings] = useState<WorkflowValidationFinding[]>([]);
  const [backendInfos, setBackendInfos] = useState<WorkflowValidationFinding[]>([]);
  const [clientErrors, setClientErrors] = useState<{ taskId: string; message: string }[]>([]);
  const [clientInfos, setClientInfos] = useState<{ taskId: string; message: string }[]>([]);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [loadedWorkflowId, setLoadedWorkflowId] = useState<string | null>(null);
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

  const loadedJcwfRef = useRef<JcwfFile | null>(null);
  const webSocketRef = useRef<WebSocket | null>(null);
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
  const [scriptCheckTick, setScriptCheckTick] = useState(0);
  useEffect(() => {
    const timer = setInterval(() => {
      scriptCheckCacheRef.current = {};
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
    console.log("[HISTORY] Pushing to undo stack:", state.nodes.length, "nodes, pos:", state.nodes[0]?.position);
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
      console.log("[DRAG STOP] Pushing pre-drag state with", stateToSave.nodes.length, "nodes, pos:", stateToSave.nodes[0]?.position);
      pushToUndoStack(stateToSave);
      pendingPrevStateRef.current = null;
    }
  }, [setNodes, pushToUndoStack]);

  const undo = useCallback(() => {
    console.log("[UNDO] Called. undoStack.length:", undoStack.length);
    if (undoStack.length === 0)
    {
      console.log("[UNDO] Early return: undoStack empty");
      return;
    }

    // Pop from undo stack
    const entry = undoStack[undoStack.length - 1];

    // Save current state for redo
    const currentState: HistoryEntry = {
      nodes: JSON.parse(nodesJsonRef.current || "[]"),
      edges: JSON.parse(edgesJsonRef.current || "[]"),
    };

    console.log("[UNDO] Restoring:", entry.nodes.length, "nodes, pos:", entry.nodes[0]?.position);

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
    console.log("[REDO] Called. redoStack.length:", redoStack.length);
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

    console.log("[REDO] Restoring:", entry.nodes.length, "nodes, pos:", entry.nodes[0]?.position);

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

    const parseTaskIdFromMessage = (message: string): string | null =>
    {
      const match = message.match(/Task '([^']+)'/);
      return match && match.length >= 2 ? match[1] : null;
    };

    const backendErrorsByTaskId = new Map<string, string[]>();
    const sourceErrors = backendFindings ? backendFindings.errors : backendErrors;
    for (const finding of sourceErrors)
    {
      const taskId = finding.taskId ?? parseTaskIdFromMessage(finding.message);
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
      const taskId = finding.taskId ?? parseTaskIdFromMessage(finding.message);
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
      const taskId = finding.taskId ?? parseTaskIdFromMessage(finding.message);
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
      const mergedWarnings = [...clientWarnings, ...serverWarnings, ...scriptWarnings];

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
          capturedStdout: runtimeSnapshot?.capturedStdout,
          capturedStderr: runtimeSnapshot?.capturedStderr,
        },
      };
    });

    setNodes(nextNodes);
    setEdges(graph.edges);
  }, [setNodes, setEdges, backendErrors, backendWarnings, backendInfos, lastSavedNodeSnapshot, props.hideTierDWarnings]);

  // Async script existence check — fires when shell commands change.
  const shellCommandsFingerprint = useMemo(() => {
    const commands: string[] = [];
    for (const node of nodes)
    {
      if (node.type !== "task") continue;
      const taskData = (node as EditorTaskNode).data;
      if (taskData.task?.type !== "shell") continue;
      const params = (taskData.task.params ?? {}) as Record<string, unknown>;
      const cmd = typeof params.command === "string" ? params.command : "";
      if (cmd.startsWith("scripts/"))
      {
        commands.push(`${node.id}:${cmd}`);
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
        const nextStdout = snapshot?.capturedStdout;
        const nextStderr = snapshot?.capturedStderr;

        if (taskNode.data.runtimeState === nextRuntimeState
          && taskNode.data.runtimeRunId === nextRuntimeRunId
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
    const graph = jcwfToGraph(jcwf);
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
      reactFlowInstance?.fitView({ padding: 0.15 });
    }, 80);
  }, [loadFromJcwf, loadedWorkflowId, reactFlowInstance]);

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
          loadFromJcwfRef.current(props.workflowId, jcwf);
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
  const MIN_ZOOM = 0.67;
  useEffect(() => {
    if (!reactFlowInstance)
    {
      return;
    }
    const timer = setTimeout(() => {
      reactFlowInstance.fitView({ padding: 0.15 });
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

      let pollTimer: ReturnType<typeof setInterval> | null = null;

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
        pollTimer = setInterval(() => {
          try
          {
            if (socket.readyState === WebSocket.OPEN)
            {
              socket.send(JSON.stringify({ type: "workflow-runs-request" }));
            }
          }
          catch
          {
            // ignore
          }
        }, 500);
      };

      socket.onclose = () => {
        if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
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
      setRuntimeTasksById(nextRuntime);
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
        setRuntimeTasksById(nextRuntime);

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

    // Initial poll after a short delay (give backend time to register the run)
    const initialTimer = setTimeout(() => {
      void poll();
    }, 1500);

    // Then poll every 2 seconds
    const interval = setInterval(() => {
      void poll();
    }, 2000);

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

  const onSelectionChange = useCallback((params: { nodes?: Node[]; edges?: Edge[]; }) => {
    const selected = params.nodes && params.nodes.length > 0 ? params.nodes[0] : null;
    setSelectedNodeId(selected ? selected.id : null);
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

  const onConnect = useCallback((connection: Connection) => {
    const isDataflow = connection.sourceHandle?.startsWith("out:") && connection.targetHandle?.startsWith("in:");
    const isControlflow =
      connection.sourceHandle === "error-signal"
      || connection.sourceHandle === "cf-out-normal"
      || connection.sourceHandle === "cf-out-error"
      || connection.targetHandle === "cf-in-normal"
      || connection.targetHandle === "cf-in-error";

    let nextEdges: EditorTaskEdge[];
    if (isDataflow)
    {
      const fromOutput = connection.sourceHandle!.slice(4);
      const toInput = connection.targetHandle!.slice(3);
      const dfEdge: EditorTaskEdge = {
        id: `df:${connection.source}.${fromOutput}->${connection.target}.${toInput}`,
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
      const baseConn = { ...connection, type: "default" } as Connection & { type: string; style?: React.CSSProperties };
      if (isDepHandle && depHandleIdx >= 0)
      {
        (baseConn as unknown as { style: React.CSSProperties }).style = { stroke: FILE_INPUT_COLORS[depHandleIdx % FILE_INPUT_COLORS.length], strokeWidth: 2 };
      }
      nextEdges = addEdge(baseConn, edges) as EditorTaskEdge[];
    }

    // Auto-populate file_inputs on target when connecting ai_call → shell/python
    let updatedNodes = nodes as EditorTaskNode[];
    if (!isDataflow && connection.source && connection.target)
    {
      const sourceNode = updatedNodes.find((n) => n.id === connection.source && n.type === "task");
      const targetNode = updatedNodes.find((n) => n.id === connection.target && n.type === "task");
      if (
        sourceNode && targetNode &&
        sourceNode.data.task.type === "ai_call" &&
        (targetNode.data.task.type === "shell" || targetNode.data.task.type === "python")
      )
      {
        const qb = (sourceNode.data.task.queue_binding ?? {}) as Record<string, unknown>;
        const probFiles = (qb.prob_files ?? []) as Array<{ path: string; content?: string } | string>;
        let sourceWd = ((sourceNode.data.task.working_directory ?? "") as string).replace(/\/+$/, "");
        // If source AI task has no working_directory set, compute the suggested path
        if (sourceWd.length === 0 && sourceNode.data.task.type === "ai_call")
        {
          const wfId = loadedWorkflowId ?? props.workflowId ?? "workflowId";
          const aiNodes = updatedNodes.filter((n) => n.type === "task" && n.data.task.type === "ai_call");
          const aiIdx = aiNodes.findIndex((n) => n.id === sourceNode.id);
          const num = String(aiIdx >= 0 ? aiIdx + 1 : aiNodes.length + 1).padStart(2, "0");
          sourceWd = `../queue/${wfId}/${num}_${sourceNode.id}`;
        }
        let targetWd = ((targetNode.data.task.working_directory ?? "") as string).replace(/\/+$/, "");
        // If target shell/python task has no working_directory set, compute a suggested path
        if (targetWd.length === 0)
        {
          const wfId = loadedWorkflowId ?? props.workflowId ?? "workflowId";
          targetWd = `${wfId}/01_${targetNode.id}`;
        }

        if (probFiles.length > 0 && sourceWd.length > 0)
        {
          // Compute prefix: go up from target WD to workflow base dir
          const targetDepth = targetWd.length > 0
            ? targetWd.split("/").filter((s) => s.length > 0 && s !== ".").length
            : 0;
          const upPrefix = "../".repeat(targetDepth);

          const newInputPaths: string[] = [];
          for (const entry of probFiles)
          {
            const probPath = typeof entry === "string" ? entry : entry.path;
            const outputName = probPath.replace(/\.txt$/, ".output.txt");
            newInputPaths.push(`${upPrefix}${sourceWd}/${outputName}`);
          }

          const existingInputs: string[] = Array.isArray(targetNode.data.task.file_inputs)
            ? [...(targetNode.data.task.file_inputs as string[])]
            : [];
          // Fill empty slots first, then append remaining
          const toPlace = newInputPaths.filter((p) => !existingInputs.includes(p));
          const toAppend: string[] = [];
          for (const path of toPlace)
          {
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
    setStatusText(isDataflow ? "Dataflow edge added." : `Edge added.${updatedNodes !== nodes ? " file_inputs auto-populated from upstream prob_files." : ""}`);
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
    setEditingFilter(structuredClone(filter));
    setShowFilterBuilder(true);
  }, []);

  const onFilterBuilderSave = useCallback((updatedFilter: JcwfFilter) => {
    setShowFilterBuilder(false);
    setEditingFilter(null);

    const nextNodes = nodes.map((n) => {
      if (n.type !== "filter") { return n; }
      const filterNode = n as EditorFilterNode;
      if (filterNode.data.filter.id !== updatedFilter.id && n.id !== `filter:${updatedFilter.id}`)
      {
        return n;
      }
      return {
        ...filterNode,
        id: `filter:${updatedFilter.id}`,
        data: {
          ...filterNode.data,
          filter: updatedFilter,
          title: updatedFilter.id,
          subtitle: updatedFilter.source.kind,
        },
      };
    });

    const graph: EditorGraph = { nodes: nextNodes as EditorNode[], edges };
    recomputeValidation(graph);
    setIsDirty(true);
  }, [nodes, edges, recomputeValidation]);

  const updateSelectedTaskField = useCallback((patch: Partial<JcwfTask>) => {
    if (!selectedNode)
    {
      return;
    }

    let nextNodes: EditorTaskNode[] = (nodes as EditorTaskNode[]).map((n) => {
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

    // Propagate PROB filename renames to downstream file_inputs
    if (selectedNode.data.task.type === "ai_call" && patch.queue_binding)
    {
      const oldQb = (selectedNode.data.task.queue_binding ?? {}) as Record<string, unknown>;
      const oldProbs = (oldQb.prob_files ?? []) as Array<{ path: string; content?: string } | string>;
      const newQb = (patch.queue_binding ?? {}) as Record<string, unknown>;
      const newProbs = (newQb.prob_files ?? []) as Array<{ path: string; content?: string } | string>;

      const renames: Array<{ oldSuffix: string; newSuffix: string }> = [];
      for (let i = 0; i < Math.max(oldProbs.length, newProbs.length); i++)
      {
        const oldEntry = oldProbs[i];
        const newEntry = newProbs[i];
        if (!oldEntry || !newEntry) continue;
        const oldPath = typeof oldEntry === "string" ? oldEntry : oldEntry.path;
        const newPath = typeof newEntry === "string" ? newEntry : newEntry.path;
        if (oldPath !== newPath)
        {
          renames.push({
            oldSuffix: "/" + oldPath.replace(/\.txt$/, ".output.txt"),
            newSuffix: "/" + newPath.replace(/\.txt$/, ".output.txt"),
          });
        }
      }

      if (renames.length > 0)
      {
        nextNodes = nextNodes.map((n) => {
          if (!Array.isArray(n.data.task.file_inputs)) return n;
          const fi = n.data.task.file_inputs as string[];
          let changed = false;
          const updatedFi = fi.map((f) => {
            for (const r of renames)
            {
              if (f.endsWith(r.oldSuffix))
              {
                changed = true;
                return f.slice(0, f.length - r.oldSuffix.length) + r.newSuffix;
              }
            }
            return f;
          });
          if (!changed) return n;
          const nextTask = { ...(n.data.task as JcwfTask), file_inputs: updatedFi };
          const { title: t, subtitle: st } = nodeTitleFromTask(nextTask);
          return { ...n, data: { ...n.data, task: nextTask, title: t, subtitle: st } };
        });
      }
    }

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
  }, [loadedWorkflowId, props.workflowId, onSave]);

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
            <button className="btn" type="button" style={{ borderColor: "rgba(255,180,80,0.35)", color: "rgba(255,200,140,0.95)" }} onClick={addBranchNode}>+ Branch</button>
            <hr style={{ border: "none", borderTop: "1px solid rgba(255,255,255,0.08)", margin: "4px 0" }} />
            <button className="btn" type="button" style={{ borderColor: "rgba(180,140,255,0.35)", color: "rgba(200,170,255,0.95)" }} onClick={addFilterNode}>+ Filter</button>
          </div>
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
              <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={wfDefaultAiProvider} onChange={(e) => { setWfDefaultAiProvider(e.target.value); setIsDirty(true); }} placeholder="e.g. openai" />
            </label>
            <label className="field">
              <div className="small">ai.model</div>
              <input className="input" style={{ fontSize: 12, padding: "4px 8px" }} value={wfDefaultAiModel} onChange={(e) => { setWfDefaultAiModel(e.target.value); setIsDirty(true); }} placeholder="e.g. gpt-4.1-mini" />
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
        <div style={{ flex: 1, position: "relative", minHeight: 0 }}>
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
            onSelectionChange={onSelectionChange}
            deleteKeyCode={["Backspace", "Delete"]}
            minZoom={MIN_ZOOM}
            onInit={(instance) => { setReactFlowInstance(instance); instance.fitView(); }}
            onMoveEnd={(_event, viewport) => { setCurrentZoom(viewport.zoom); }}
          >
            <Background />
            <Controls />
            <MiniMap />
          </ReactFlow>
        </div>
        <AiPromptArea
          getCurrentJcwf={getCurrentJcwf}
          onJcwfGenerated={onJcwfGenerated}
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

                  <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12 }}>
                    <input
                      type="checkbox"
                      checked={selectedNode.data.task.expose_error_signal === true}
                      onChange={(e) => { updateSelectedTaskField({ expose_error_signal: e.target.checked || undefined } as Partial<JcwfTask>); }}
                    />
                    Expose error signal output
                  </label>

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

                  {selectedNode.data.task.mode === "per_item" && (
                    <label className="field">
                      <div className="small">Filter ID</div>
                      <input
                        className="input"
                        value={(selectedNode.data.task.filter ?? "") as string}
                        onChange={(e) => { updateSelectedTaskField({ filter: e.target.value || undefined } as Partial<JcwfTask>); }}
                        placeholder="filter id (e.g. work_items)"
                      />
                    </label>
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

                  <label className="field">
                    <div className="small">
                      {selectedNode.data.task.type === "ai_call"
                        ? <>working_directory <span style={{ opacity: 0.5 }}>(optional, tab to accept)</span></>
                        : <>working_directory relative to jcwf <span style={{ opacity: 0.5 }}>(tab to accept)</span></>}
                    </div>
                    <input
                      className="input"
                      value={(selectedNode.data.task.working_directory ?? "") as string}
                      onChange={(e) => { updateSelectedTaskField({ working_directory: e.target.value }); }}
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
                  </label>

                  {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python") && (
                    <div className="field">
                      <div className="small">file_inputs</div>
                      {(Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : []).map((fi, idx) => (
                        <div key={`fi-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                          <span style={{
                            display: "inline-block", width: 8, height: 8, borderRadius: "50%",
                            background: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], flexShrink: 0,
                          }} title={`Input ${idx + 1}`} />
                          <span style={{ fontSize: 10, color: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], fontWeight: 600, flexShrink: 0 }}>{idx + 1}</span>
                          <input
                            className="input"
                            style={{ fontSize: 12, padding: "4px 8px", flex: 1 }}
                            value={fi}
                            onChange={(e) => {
                              const list = [...(Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : [])];
                              list[idx] = e.target.value;
                              updateSelectedTaskField({ file_inputs: list } as Partial<JcwfTask>);
                            }}
                          />
                          <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                            const list = (Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : []).filter((_, i) => i !== idx);
                            updateSelectedTaskField({ file_inputs: list.length > 0 ? list : undefined } as Partial<JcwfTask>);
                          }}>x</button>
                        </div>
                      ))}
                      <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                        const list = [...(Array.isArray(selectedNode.data.task.file_inputs) ? selectedNode.data.task.file_inputs as string[] : []), ""];
                        updateSelectedTaskField({ file_inputs: list } as Partial<JcwfTask>);
                      }}>+ file_input</button>
                    </div>
                  )}

                  {(selectedNode.data.task.type === "shell" || selectedNode.data.task.type === "python") && (
                    <div className="field">
                      <div className="small">file_outputs</div>
                      {(Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : []).map((fo, idx) => (
                        <div key={`fo-${idx}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                          <input
                            className="input"
                            style={{ fontSize: 12, padding: "4px 8px", flex: 1 }}
                            value={fo}
                            onChange={(e) => {
                              const list = [...(Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : [])];
                              list[idx] = e.target.value;
                              updateSelectedTaskField({ file_outputs: list } as Partial<JcwfTask>);
                            }}
                          />
                          <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                            const list = (Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : []).filter((_, i) => i !== idx);
                            updateSelectedTaskField({ file_outputs: list.length > 0 ? list : undefined } as Partial<JcwfTask>);
                          }}>x</button>
                        </div>
                      ))}
                      <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                        const list = [...(Array.isArray(selectedNode.data.task.file_outputs) ? selectedNode.data.task.file_outputs as string[] : []), ""];
                        updateSelectedTaskField({ file_outputs: list } as Partial<JcwfTask>);
                      }}>+ file_output</button>
                    </div>
                  )}

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

                  <label className="field">
                    <div className="small">doc</div>
                    <textarea
                      className="input"
                      value={typeof selectedNode.data.task.doc === "string" ? selectedNode.data.task.doc : ""}
                      onChange={(e) => { updateSelectedTaskField({ doc: e.target.value }); }}
                      placeholder="(optional, string)"
                      rows={4}
                    />
                  </label>

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
                            const isFileRef = arg.startsWith("${input[") || arg.startsWith("${output[");
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
                                        <option key={`fi-${fiIdx}`} value={`\${input[${fiIdx}]}`}>
                                          input[{fiIdx}]: {fiName}
                                        </option>
                                      );
                                    })}
                                    {curFileOutputs.map((_, foIdx) => {
                                      const foSegs = curFileOutputs[foIdx].split("/");
                                      const foName = foSegs[foSegs.length - 1] || curFileOutputs[foIdx];
                                      return (
                                        <option key={`fo-${foIdx}`} value={`\${output[${foIdx}]}`}>
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
                  })() : (
                    <label className="field">
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

                  <label className="field">
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

                  {selectedNode.data.task.type === "ai_call" && (
                    <QueueBindingEditor
                      queueBinding={selectedNode.data.task.queue_binding as JcwfQueueBinding | undefined}
                      onChange={(binding) => { updateSelectedTaskField({ queue_binding: binding } as Partial<JcwfTask>); }}
                    />
                  )}

                  <div className="field">
                    <div className="small" style={{ fontWeight: 600 }}>inputs</div>
                    {Object.entries((selectedNode.data.task.inputs as Record<string, Record<string, unknown>> | undefined) ?? {}).map(([name, slot]) => (
                      <div key={`in-${name}`} style={{ display: "flex", gap: 4, alignItems: "center", flexWrap: "wrap" }}>
                        <input className="input" style={{ fontSize: 11, padding: "3px 6px", width: 80 }} value={name} readOnly title="Input name (edit via rename)" />
                        <select className="input" style={{ fontSize: 11, padding: "3px 4px", width: 72 }} value={(slot.type as string) ?? "string"} onChange={(e) => {
                          const inputs = { ...((selectedNode.data.task.inputs as Record<string, Record<string, unknown>>) ?? {}) };
                          inputs[name] = { ...inputs[name], type: e.target.value };
                          updateSelectedTaskField({ inputs } as Partial<JcwfTask>);
                        }}>
                          <option value="string">string</option>
                          <option value="object">object</option>
                          <option value="number">number</option>
                        </select>
                        <label style={{ fontSize: 10, display: "flex", alignItems: "center", gap: 3 }}>
                          <input type="checkbox" checked={slot.required === true} onChange={(e) => {
                            const inputs = { ...((selectedNode.data.task.inputs as Record<string, Record<string, unknown>>) ?? {}) };
                            inputs[name] = { ...inputs[name], required: e.target.checked || undefined };
                            updateSelectedTaskField({ inputs } as Partial<JcwfTask>);
                          }} />req
                        </label>
                        <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                          const inputs = { ...((selectedNode.data.task.inputs as Record<string, Record<string, unknown>>) ?? {}) };
                          delete inputs[name];
                          updateSelectedTaskField({ inputs: Object.keys(inputs).length > 0 ? inputs : undefined } as Partial<JcwfTask>);
                        }}>x</button>
                      </div>
                    ))}
                    <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                      const inputs = { ...((selectedNode.data.task.inputs as Record<string, Record<string, unknown>>) ?? {}) };
                      let idx = Object.keys(inputs).length + 1;
                      while (inputs[`input_${idx}`]) { idx++; }
                      inputs[`input_${idx}`] = { type: "string" };
                      updateSelectedTaskField({ inputs } as Partial<JcwfTask>);
                    }}>+ input</button>
                  </div>

                  <div className="field">
                    <div className="small" style={{ fontWeight: 600 }}>outputs</div>
                    {Object.entries((selectedNode.data.task.outputs as Record<string, Record<string, unknown>> | undefined) ?? {}).map(([name, slot]) => (
                      <div key={`out-${name}`} style={{ display: "flex", gap: 4, alignItems: "center" }}>
                        <input className="input" style={{ fontSize: 11, padding: "3px 6px", width: 80 }} value={name} readOnly title="Output name (edit via rename)" />
                        <select className="input" style={{ fontSize: 11, padding: "3px 4px", width: 72 }} value={(slot.type as string) ?? "string"} onChange={(e) => {
                          const outputs = { ...((selectedNode.data.task.outputs as Record<string, Record<string, unknown>>) ?? {}) };
                          outputs[name] = { ...outputs[name], type: e.target.value };
                          updateSelectedTaskField({ outputs } as Partial<JcwfTask>);
                        }}>
                          <option value="string">string</option>
                          <option value="object">object</option>
                          <option value="number">number</option>
                        </select>
                        <button className="btn" type="button" style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }} onClick={() => {
                          const outputs = { ...((selectedNode.data.task.outputs as Record<string, Record<string, unknown>>) ?? {}) };
                          delete outputs[name];
                          updateSelectedTaskField({ outputs: Object.keys(outputs).length > 0 ? outputs : undefined } as Partial<JcwfTask>);
                        }}>x</button>
                      </div>
                    ))}
                    <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={() => {
                      const outputs = { ...((selectedNode.data.task.outputs as Record<string, Record<string, unknown>>) ?? {}) };
                      let idx = Object.keys(outputs).length + 1;
                      while (outputs[`output_${idx}`]) { idx++; }
                      outputs[`output_${idx}`] = { type: "string" };
                      updateSelectedTaskField({ outputs } as Partial<JcwfTask>);
                    }}>+ output</button>
                  </div>

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

                  {runtimeTasksById[selectedNode.id]
                    ? (
                      <div style={{ marginTop: 10, padding: 8, background: "rgba(255,255,255,0.04)", borderRadius: 4 }}>
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
    </div>
  );
}
