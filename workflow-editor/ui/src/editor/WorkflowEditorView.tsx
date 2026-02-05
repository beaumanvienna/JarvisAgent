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
import { jcwfToGraph } from "./jcwfToGraph";
import { graphToJcwf } from "./graphToJcwf";
import { validateGraph } from "./validation";
import type { EditorGraph, EditorTaskEdge, EditorTaskNode, EditorTaskNodeData, RuntimeTaskState } from "./types";
import type { JcwfFile, JcwfTask, JcwfTaskType } from "../jcwf/types";
import {
  cancelRun,
  createWorkflowWithId,
  loadWorkflow,
  runWorkflow,
  saveWorkflow,
  validateDraft,
  type WorkflowValidationFinding,
} from "../api/workflows";
import CreateWorkflowModal from "../components/CreateWorkflowModal";

const nodeTypes = { task: TaskNode };

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
  if (normalized.includes("queue") || normalized.includes("pending"))
  {
    return "queued";
  }
  if (normalized.includes("run"))
  {
    return "running";
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

function computeGraphSignature(nodes: EditorTaskNode[], edges: EditorTaskEdge[]): string
{
  const signatureObject = {
    nodes: nodes
      .map((n) => ({
        id: n.id,
        task: {
          id: n.data.task.id,
          type: n.data.task.type,
          label: n.data.task.label,
          doc: n.data.task.doc,
          working_directory: n.data.task.working_directory,
          params: n.data.task.params,
        },
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
  // Keep stable key ordering for deterministic comparisons.
  const signatureObject = {
    id: task.id,
    type: task.type,
    label: task.label,
    doc: task.doc,
    working_directory: task.working_directory,
    params: task.params,
  };

  return JSON.stringify(signatureObject);
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
  const [showCreateModal, setShowCreateModal] = useState<boolean>(false);
  const [createModalMode, setCreateModalMode] = useState<"create" | "saveAs">("create");

  const webSocketRef = useRef<WebSocket | null>(null);
  const [isWebSocketConnected, setIsWebSocketConnected] = useState<boolean>(false);
  const [activeRuns, setActiveRuns] = useState<WorkflowRunListItem[]>([]);
  const [selectedRunId, setSelectedRunId] = useState<string | null>(null);
  const [runtimeTasksById, setRuntimeTasksById] = useState<RuntimeTaskSnapshotById>({});
  const [pendingRunId, setPendingRunId] = useState<string | null>(null);
  const [lastRunResult, setLastRunResult] = useState<{ runId: string; state: string } | null>(null);
  const pendingRunSeenRef = useRef<boolean>(false);

  const runtimeTasksByIdRef = useRef<RuntimeTaskSnapshotById>({});
  useEffect(() => {
    runtimeTasksByIdRef.current = runtimeTasksById;
  }, [runtimeTasksById]);

  // Undo/redo history (two-stack model)
  type HistoryEntry = { nodes: EditorTaskNode[]; edges: EditorTaskEdge[] };
  const [undoStack, setUndoStack] = useState<HistoryEntry[]>([]);
  const [redoStack, setRedoStack] = useState<HistoryEntry[]>([]);
  const isUndoRedoRef = useRef<boolean>(false);

  const initialGraph: EditorGraph = useMemo(() => ({ nodes: [], edges: [] }), []);

  const [nodes, setNodes] = useNodesState<EditorTaskNodeData>(
    initialGraph.nodes as Node<EditorTaskNodeData>[]
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
    return computeGraphSignature(nodes as EditorTaskNode[], edges as EditorTaskEdge[]);
  }, [nodes, edges]);

  const resetToNewDraft = useCallback(() => {
    setNodes([]);
    setEdges([]);
    setSelectedNodeId(null);
    setLoadedWorkflowId(null);
    setBackendErrors([]);
    setBackendWarnings([]);
    setBackendInfos([]);
    setErrorText(null);

    const emptySignature = computeGraphSignature([], []);
    setLastSavedSignature(emptySignature);
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

    const nextNodes: EditorTaskNode[] = graph.nodes.map((n) => {
      const clientErrors = validation.nodeErrorsById.get(n.id) ?? [];
      const serverErrors = backendErrorsByTaskId.get(n.id) ?? [];
      const mergedErrors = [...clientErrors, ...serverErrors];

      const clientWarnings = validation.nodeWarningsById ? (validation.nodeWarningsById.get(n.id) ?? []) : [];
      const serverWarnings = backendWarningsByTaskId.get(n.id) ?? [];
      const mergedWarnings = [...clientWarnings, ...serverWarnings];

      const clientInfos = validation.nodeInfosById ? (validation.nodeInfosById.get(n.id) ?? []) : [];
      const serverInfos = backendInfosByTaskId.get(n.id) ?? [];
      const mergedInfos = [...clientInfos, ...serverInfos];

      const savedTaskSignature = lastSavedNodeSnapshot[n.id];
      const nodeIsDirty = savedTaskSignature ? computeTaskSignature(n.data.task) !== savedTaskSignature : true;
      const runtimeSnapshot = runtimeTasksByIdRef.current[n.id];


      return {
        ...n,
        data: {
          ...n.data,
          validationErrors: mergedErrors.length > 0 ? mergedErrors : undefined,
          validationWarnings: mergedWarnings.length > 0 ? mergedWarnings : undefined,
          validationInfos: mergedInfos.length > 0 ? mergedInfos : undefined,
          isDirty: nodeIsDirty ? true : undefined,
          hideTierDWarnings: props.hideTierDWarnings,
          runtimeState: runtimeSnapshot ? runtimeSnapshot.state : undefined,
          runtimeRunId: runtimeSnapshot ? runtimeSnapshot.runId : undefined,
        },
      };
    });

    setNodes(nextNodes);
    setEdges(graph.edges);
  }, [setNodes, setEdges, backendErrors, backendWarnings, backendInfos, lastSavedNodeSnapshot, props.hideTierDWarnings]);

  useEffect(() => {
    setNodes((current) => {
      let changed = false;
      const next = (current as EditorTaskNode[]).map((n) => {
        const snapshot = runtimeTasksById[n.id];
        const nextRuntimeState = snapshot ? snapshot.state : undefined;
        const nextRuntimeRunId = snapshot ? snapshot.runId : undefined;

        if (n.data.runtimeState === nextRuntimeState && n.data.runtimeRunId === nextRuntimeRunId)
        {
          return n;
        }

        changed = true;
        return {
          ...n,
          data: {
            ...n.data,
            runtimeState: nextRuntimeState,
            runtimeRunId: nextRuntimeRunId,
          },
        };
      });

      return changed ? next : current;
    });
  }, [runtimeTasksById, setNodes]);

  // Update nodes when hideTierDWarnings setting changes
  useEffect(() => {
    setNodes((current) => {
      return (current as EditorTaskNode[]).map((n) => {
        if (n.data.hideTierDWarnings === props.hideTierDWarnings)
        {
          return n;
        }
        return { ...n, data: { ...n.data, hideTierDWarnings: props.hideTierDWarnings } };
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
      setLastSavedSignature(computeGraphSignature(graph.nodes, graph.edges));
      setLastSavedNodeSnapshot(computeNodeSnapshot(graph.nodes));
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
    setLoadedWorkflowId(workflowId);
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

  // WebSocket run monitoring.
  useEffect(() => {
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

      const obj = message as Record<string, unknown>;
      const messageType = typeof obj.type === "string" ? obj.type : "";

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

        const targetRunId = selectedRunId ?? (nextActiveRuns.length > 0 ? nextActiveRuns[0].runId : null);
        if (targetRunId && !selectedRunId)
        {
          setSelectedRunId(targetRunId);
        }

        if (!targetRunId)
        {
          setRuntimeTasksById({});
          return;
        }

        const matchingRun = runs.find((r) => (typeof r.runId === "string" ? r.runId : "") === targetRunId);
        if (!matchingRun)
        {
          setRuntimeTasksById({});
          return;
        }

        const tasksUnknown = matchingRun.tasks;
        if (!Array.isArray(tasksUnknown))
        {
          setRuntimeTasksById({});
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
        return;
      }
    };

    return () => {
      try
      {
        socket.close();
      }
      catch
      {
        // ignore
      }
    };
  }, [selectedRunId]);

  // Detect when a pending run completes
  useEffect(() => {
    if (!pendingRunId)
    {
      pendingRunSeenRef.current = false;
      return;
    }

    const run = activeRuns.find((r) => r.runId === pendingRunId);
    if (run)
    {
      pendingRunSeenRef.current = true;
    }

    if (!run)
    {
      // Run not found in active runs - if we saw it before and now it's gone, it completed
      if (pendingRunSeenRef.current)
      {
        setLastRunResult({ runId: pendingRunId, state: "completed" });
        setStatusText(`✓ Run completed. runId=${pendingRunId}`);
        setPendingRunId(null);
        pendingRunSeenRef.current = false;
      }
      return;
    }

    const terminalStates = ["completed", "failed", "cancelled"];
    if (terminalStates.includes(run.state))
    {
      setLastRunResult({ runId: run.runId, state: run.state });
      setPendingRunId(null);
      pendingRunSeenRef.current = false;

      const stateLabel = run.state === "completed" ? "✓ Run completed successfully" :
                         run.state === "failed" ? "✗ Run failed" :
                         "Run cancelled";
      setStatusText(`${stateLabel}. runId=${run.runId}`);
    }
  }, [activeRuns, pendingRunId]);

  const selectedNode = useMemo(() => {
    if (!selectedNodeId)
    {
      return null;
    }
    return (nodes as EditorTaskNode[]).find((n) => n.id === selectedNodeId) ?? null;
  }, [nodes, selectedNodeId]);

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
    selectNodeById(selected ? selected.id : null);
    setSelectedEdgeIds(params.edges ? params.edges.map((e) => e.id) : []);
  }, [selectNodeById]);

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
    // Create an edge and immediately validate DAG.
    const nextEdges = addEdge(
      { ...connection, type: "default" },
      edges
    ) as EditorTaskEdge[];

    const graph: EditorGraph = {
      nodes: nodes as EditorTaskNode[],
      edges: nextEdges,
    };

    const validation = validateGraph(graph);
    if (validation.cycleNodes.length > 0)
    {
      setStatusText("Blocked: edge would create a cycle.");
      setErrorText(null);
      // do not apply the edge
      recomputeValidation({ nodes: graph.nodes, edges });
      return;
    }

    recomputeValidation({ nodes: graph.nodes, edges: nextEdges });
    setStatusText("Edge added.");
    setErrorText(null);
  }, [nodes, edges, recomputeValidation]);

  const findNonOverlappingPosition = useCallback((
    startX: number,
    startY: number,
    existingNodes: EditorTaskNode[]
  ): { x: number; y: number } => {
    const nodeWidth = 200;
    const nodeHeight = 100;
    const padding = 20;

    const isOverlapping = (x: number, y: number): boolean => {
      for (const node of existingNodes)
      {
        const nx = node.position.x;
        const ny = node.position.y;
        if (
          x < nx + nodeWidth + padding &&
          x + nodeWidth + padding > nx &&
          y < ny + nodeHeight + padding &&
          y + nodeHeight + padding > ny
        )
        {
          return true;
        }
      }
      return false;
    };

    let x = startX;
    let y = startY;
    let attempts = 0;
    const maxAttempts = 50;

    while (isOverlapping(x, y) && attempts < maxAttempts)
    {
      // Spiral outward to find a free spot
      const offset = (Math.floor(attempts / 4) + 1) * (nodeWidth + padding);
      const direction = attempts % 4;
      switch (direction)
      {
        case 0: x = startX + offset; break;
        case 1: y = startY + offset; break;
        case 2: x = startX - offset; break;
        case 3: y = startY - offset; break;
      }
      attempts++;
    }

    return { x, y };
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

  const updateSelectedTaskField = useCallback((patch: Partial<JcwfTask>) => {
    if (!selectedNode)
    {
      return;
    }

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
    return result.jcwf;
  }, [nodes, edges, loadedWorkflowId, props.workflowId]);

  const notifyPersisted = useCallback((event: WorkflowPersistEvent) => {
    if (props.onWorkflowPersisted)
    {
      props.onWorkflowPersisted(event);
    }
  }, [props.onWorkflowPersisted]);

  const updateSavedBaseline = useCallback(() => {
    setLastSavedSignature(computeGraphSignature(nodes as EditorTaskNode[], edges as EditorTaskEdge[]));
    setLastSavedNodeSnapshot(computeNodeSnapshot(nodes as EditorTaskNode[]));
    setBackendErrors([]);
    setBackendWarnings([]);
    setIsDirty(false);
    if (props.onDirtyStateChange)
    {
      props.onDirtyStateChange(false);
    }
  }, [nodes, edges, props.onDirtyStateChange]);

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

      const result = await createWorkflowWithId(newId, exportResult.jcwf);
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
  }, [loadedWorkflowId, props.workflowId]);

  const onCancelRun = useCallback(async () => {
    if (!selectedRunId)
    {
      return;
    }

    try
    {
      setStatusText("Cancelling run…");
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
    const currentNodes = nodes as EditorTaskNode[];
    const currentEdges = edges as EditorTaskEdge[];

    if (currentNodes.length === 0)
    {
      return;
    }

    // Build dependency map: nodeId -> set of nodes it depends on
    const dependsOn = new Map<string, Set<string>>();
    for (const node of currentNodes)
    {
      dependsOn.set(node.id, new Set());
    }
    for (const edge of currentEdges)
    {
      const deps = dependsOn.get(edge.target);
      if (deps)
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

    for (const node of currentNodes)
    {
      computeLevel(node.id, new Set());
    }

    // Group nodes by level
    const nodesByLevel = new Map<number, EditorTaskNode[]>();
    for (const node of currentNodes)
    {
      const level = levels.get(node.id) ?? 0;
      const group = nodesByLevel.get(level) ?? [];
      group.push(node);
      nodesByLevel.set(level, group);
    }

    // Position nodes
    const NODE_WIDTH = 200;
    const NODE_HEIGHT = 80;
    const HORIZONTAL_GAP = 100;
    const VERTICAL_GAP = 40;

    const layoutedNodes: EditorTaskNode[] = [];
    const sortedLevels = Array.from(nodesByLevel.keys()).sort((a, b) => a - b);

    for (const level of sortedLevels)
    {
      const nodesAtLevel = nodesByLevel.get(level) ?? [];
      const x = level * (NODE_WIDTH + HORIZONTAL_GAP) + 50;

      for (let i = 0; i < nodesAtLevel.length; i++)
      {
        const node = nodesAtLevel[i];
        const y = i * (NODE_HEIGHT + VERTICAL_GAP) + 50;
        layoutedNodes.push({
          ...node,
          position: { x, y },
        });
      }
    }

    const graph: EditorGraph = { nodes: layoutedNodes, edges: currentEdges };
    recomputeValidation(graph);
    setStatusText("Auto-layout applied.");
  }, [nodes, edges, recomputeValidation]);

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
          </div>
        </div>

        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Workflow</div>
          <div className="small">
            Loaded: <code>{loadedWorkflowId ?? props.workflowId ?? "none"}</code>
            {isDirty ? <span className="dirtyBadge">unsaved</span> : null}
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
            <button className="btn" type="button" onClick={onRun} disabled={!loadedWorkflowId && !props.workflowId}>Run</button>
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
            ? (
              <div style={{ marginTop: 10 }}>
                <div className="small">Selected: <code>{selectedRunId}</code></div>
                <button
                  className="btn"
                  type="button"
                  onClick={onCancelRun}
                  style={{ marginTop: 6 }}
                >
                  Cancel Run
                </button>
              </div>
            )
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

      <div style={{ position: "relative" }}>
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
          onInit={setReactFlowInstance}
          fitView
        >
          <Background />
          <Controls />
          <MiniMap />
        </ReactFlow>
      </div>

      <aside className="inspector">
        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 8 }}>Inspector</div>

          {!selectedNode
            ? <div className="muted">Select a node to edit its properties.</div>
            : (
              <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
                <div className="small">taskId: <code>{selectedNode.id}</code></div>

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
                  <div className="small">working_directory</div>
                  <input
                    className="input"
                    value={(selectedNode.data.task.working_directory ?? "") as string}
                    onChange={(e) => { updateSelectedTaskField({ working_directory: e.target.value }); }}
                    placeholder="(optional)"
                  />
                </label>

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
                    </div>
                  )
                  : null}
              </div>
            )}
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
    </div>
  );
}
