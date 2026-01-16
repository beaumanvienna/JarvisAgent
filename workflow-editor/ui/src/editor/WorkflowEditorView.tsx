import React, { useCallback, useEffect, useMemo, useState } from "react";
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  addEdge,
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
import type { EditorGraph, EditorTaskEdge, EditorTaskNode, EditorTaskNodeData } from "./types";
import type { JcwfFile, JcwfTask, JcwfTaskType } from "../jcwf/types";
import {
  createWorkflowWithId,
  loadWorkflow,
  runWorkflow,
  saveWorkflow,
  validateDraft,
  type WorkflowValidationFinding,
} from "../api/workflows";

const nodeTypes = { task: TaskNode };

export type WorkflowPersistEvent = {
  kind: "save" | "create" | "saveAs";
  workflowId: string;
};

type NodeSnapshot = Record<string, string>;

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
  onWorkflowCreated: (workflowId: string) => void;
  onWorkflowPersisted?: (event: WorkflowPersistEvent) => void;
  onDirtyStateChange?: (isDirty: boolean) => void;
  onNavigateBack: () => void;
}): JSX.Element
{
  const [reactFlowInstance, setReactFlowInstance] = useState<ReactFlowInstance | null>(null);

  const [statusText, setStatusText] = useState<string>("");
  const [errorText, setErrorText] = useState<string | null>(null);
  const [backendErrors, setBackendErrors] = useState<WorkflowValidationFinding[]>([]);
  const [backendWarnings, setBackendWarnings] = useState<WorkflowValidationFinding[]>([]);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [loadedWorkflowId, setLoadedWorkflowId] = useState<string | null>(null);
  const [lastSavedSignature, setLastSavedSignature] = useState<string>("");
  const [lastSavedNodeSnapshot, setLastSavedNodeSnapshot] = useState<NodeSnapshot>({});
  const [isDirty, setIsDirty] = useState<boolean>(false);
  const [showBackToListHint, setShowBackToListHint] = useState<boolean>(false);

  const initialGraph: EditorGraph = useMemo(() => ({ nodes: [], edges: [] }), []);

  const [nodes, setNodes, onNodesChange] = useNodesState<EditorTaskNodeData>(
    initialGraph.nodes as Node<EditorTaskNodeData>[]
  );

  const [edges, setEdges, onEdgesChange] = useEdgesState<EditorTaskEdge>(
    initialGraph.edges as Edge[]
  );

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
    setErrorText(null);
    setShowBackToListHint(false);

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
  }) => {
    const validation = validateGraph(graph);

    const parseTaskIdFromMessage = (message: string): string | null =>
    {
      const match = message.match(/Task '([^']+)'/);
      return match && match.length >= 2 ? match[1] : null;
    };

    const backendErrorsByTaskId = new Map<string, string[]>();
    const sourceErrors = backendFindings ? backendFindings.errors : backendErrors;
    for (const finding of sourceErrors)
    {
      const taskId = parseTaskIdFromMessage(finding.message);
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
      const taskId = parseTaskIdFromMessage(finding.message);
      if (!taskId)
      {
        continue;
      }

      const existing = backendWarningsByTaskId.get(taskId) ?? [];
      existing.push(finding.message);
      backendWarningsByTaskId.set(taskId, existing);
    }

    const nextNodes: EditorTaskNode[] = graph.nodes.map((n) => {
      const clientErrors = validation.nodeErrorsById.get(n.id) ?? [];
      const serverErrors = backendErrorsByTaskId.get(n.id) ?? [];
      const mergedErrors = [...clientErrors, ...serverErrors];

      const clientWarnings = validation.nodeWarningsById ? (validation.nodeWarningsById.get(n.id) ?? []) : [];
      const serverWarnings = backendWarningsByTaskId.get(n.id) ?? [];
      const mergedWarnings = [...clientWarnings, ...serverWarnings];

      const savedTaskSignature = lastSavedNodeSnapshot[n.id];
      const nodeIsDirty = savedTaskSignature ? computeTaskSignature(n.data.task) !== savedTaskSignature : true;

      return {
        ...n,
        data: {
          ...n.data,
          validationErrors: mergedErrors.length > 0 ? mergedErrors : undefined,
          validationWarnings: mergedWarnings.length > 0 ? mergedWarnings : undefined,
          isDirty: nodeIsDirty ? true : undefined,
        },
      };
    });

    setNodes(nextNodes);
    setEdges(graph.edges);
  }, [setNodes, setEdges, backendErrors, backendWarnings, lastSavedNodeSnapshot]);

  const loadFromJcwf = useCallback((workflowId: string, jcwfUnknown: unknown) => {
    const jcwf = jcwfUnknown as JcwfFile;
    const graph = jcwfToGraph(jcwf);
    setLastSavedSignature(computeGraphSignature(graph.nodes, graph.edges));
    setLastSavedNodeSnapshot(computeNodeSnapshot(graph.nodes));
    setIsDirty(false);
    if (props.onDirtyStateChange)
    {
      props.onDirtyStateChange(false);
    }
    recomputeValidation(graph);
    setLoadedWorkflowId(workflowId);
    setBackendErrors([]);
    setBackendWarnings([]);
    setSelectedNodeId(null);
    setShowBackToListHint(false);
    setStatusText(`Loaded workflow '${workflowId}'.`);
    setErrorText(null);
  }, [recomputeValidation, props.onDirtyStateChange]);

  // Respond to workflowId changes.
  useEffect(() => {
    let isCancelled = false;

    async function load(): Promise<void>
    {
      if (!props.workflowId)
      {
        // "New" workflow.
        resetToNewDraft();
        return;
      }

      try
      {
        setStatusText("Loading…");
        setErrorText(null);
        setShowBackToListHint(false);

        const jcwf = await loadWorkflow(props.workflowId);
        if (!isCancelled)
        {
          loadFromJcwf(props.workflowId, jcwf);
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
  }, [props.workflowId, loadFromJcwf, resetToNewDraft]);

  const selectedNode = useMemo(() => {
    if (!selectedNodeId)
    {
      return null;
    }
    return (nodes as EditorTaskNode[]).find((n) => n.id === selectedNodeId) ?? null;
  }, [nodes, selectedNodeId]);

  const onSelectionChange = useCallback((params: { nodes?: Node[]; edges?: Edge[]; }) => {
    const selected = params.nodes && params.nodes.length > 0 ? params.nodes[0] : null;
    setSelectedNodeId(selected ? selected.id : null);
  }, []);

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

  const addTaskNode = useCallback((taskType: JcwfTaskType) => {
    const existingIds = new Set<string>((nodes as EditorTaskNode[]).map((n) => n.id));
    const newId = nextId(existingIds, taskType);

    const task = buildDefaultTask(newId, taskType);
    const { title, subtitle } = nodeTitleFromTask(task);

    const viewportCenter = reactFlowInstance ? reactFlowInstance.project({
      x: window.innerWidth / 2,
      y: window.innerHeight / 2,
    }) : { x: 0, y: 0 };

    const newNode: EditorTaskNode = {
      id: newId,
      type: "task",
      position: { x: viewportCenter.x, y: viewportCenter.y },
      data: { task, title, subtitle },
    };

    const graph: EditorGraph = { nodes: [...(nodes as EditorTaskNode[]), newNode], edges };
    recomputeValidation(graph);
    setSelectedNodeId(newId);
    setStatusText(`Added node '${newId}'.`);
    setErrorText(null);
  }, [nodes, edges, reactFlowInstance, recomputeValidation]);

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

  const onSave = useCallback(async () => {
    const workflowId = loadedWorkflowId ?? props.workflowId;

    const jcwf = exportJcwfObject();
    if (!jcwf)
    {
      return;
    }

    try
    {
      setStatusText("Saving…");
      setErrorText(null);
      setShowBackToListHint(false);

      if (workflowId)
      {
        const result = await saveWorkflow(workflowId, jcwf);
        setStatusText(result.ok ? `Saved '${workflowId}'.` : `Save returned ok=false for '${workflowId}'.`);
        updateSavedBaseline();
        notifyPersisted({ kind: "save", workflowId });
      }
      else
      {
        const proposed = window.prompt("New workflow id:", "workflow");
        const newId = proposed ? proposed.trim() : "";
        if (newId.length === 0)
        {
          setStatusText("");
          setErrorText("Create cancelled: workflow id is required.");
          return;
        }

        // Ensure exported JCWF uses the new id.
        const graph: EditorGraph = { nodes: nodes as EditorTaskNode[], edges };
        const exportResult = graphToJcwf(graph, newId);
        if (!exportResult.ok)
        {
          setErrorText(`${exportResult.message} Cycle nodes: ${exportResult.cycleNodes.join(", ")}`);
          setStatusText("");
          return;
        }

        const createResult = await createWorkflowWithId(newId, exportResult.jcwf);
        setLoadedWorkflowId(newId);
        props.onWorkflowCreated(newId);
        setStatusText(createResult.ok ? `Created '${newId}'.` : `Create returned ok=false for '${newId}'.`);
        updateSavedBaseline();
        notifyPersisted({ kind: "create", workflowId: newId });
        setShowBackToListHint(true);
      }
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Save failed: ${message}`);
      setStatusText("");
    }
  }, [loadedWorkflowId, props.workflowId, exportJcwfObject, nodes, edges, props.onWorkflowCreated, updateSavedBaseline, notifyPersisted]);

  const onSaveAs = useCallback(async () => {
    const baseId = loadedWorkflowId ?? props.workflowId ?? "workflow";
    const proposed = window.prompt("Save As… New workflow id:", `${baseId}_copy`);
    const newId = proposed ? proposed.trim() : "";
    if (newId.length === 0)
    {
      setStatusText("");
      setErrorText("Save As cancelled: workflow id is required.");
      return;
    }

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
      setStatusText("Saving As…");
      setErrorText(null);

      const result = await createWorkflowWithId(newId, exportResult.jcwf);
      setLoadedWorkflowId(newId);
      props.onWorkflowCreated(newId);
      setStatusText(result.ok ? `Saved as '${newId}'.` : `Save As returned ok=false for '${newId}'.`);
      updateSavedBaseline();
      notifyPersisted({ kind: "saveAs", workflowId: newId });
      setShowBackToListHint(true);
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Save As failed: ${message}`);
      setStatusText("");
    }
  }, [loadedWorkflowId, props.workflowId, nodes, edges, props.onWorkflowCreated, updateSavedBaseline, notifyPersisted]);

  const onNavigateBack = useCallback(() => {
    if (isDirty)
    {
      const confirmed = window.confirm("You have unsaved changes. Discard them and go back?");
      if (!confirmed)
      {
        return;
      }
    }
    props.onNavigateBack();
  }, [isDirty, props.onNavigateBack]);

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
      recomputeValidation({ nodes: nodes as EditorTaskNode[], edges }, { errors: result.errors, warnings: result.warnings });

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
      setStatusText(result.ok ? `Run started. runId=${result.runId}` : "Run returned ok=false.");
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setErrorText(`Run failed: ${message}`);
      setStatusText("");
    }
  }, [loadedWorkflowId, props.workflowId]);

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
            <button className="btn" type="button" onClick={onSave}>Save</button>
            <button className="btn" type="button" onClick={onSaveAs}>Save As…</button>
            <button className="btn" type="button" onClick={onValidate}>Validate</button>
            <button className="btn" type="button" onClick={onRun} disabled={!loadedWorkflowId && !props.workflowId}>Run</button>
            <button className="btn" type="button" onClick={onExportToConsole}>Export (console)</button>
            <button className="btn" type="button" onClick={onNavigateBack}>
              {showBackToListHint ? "Back to list" : "Back"}
            </button>
          </div>

          {statusText ? <div className="small" style={{ marginTop: 10 }}>{statusText}</div> : null}
          {errorText ? <div className="errorText" style={{ marginTop: 10 }}>{errorText}</div> : null}

          {(backendErrors.length > 0 || backendWarnings.length > 0)
            ? (
              <div style={{ marginTop: 12 }}>
                {backendErrors.length > 0
                  ? (
                    <div style={{ marginBottom: 10 }}>
                      <div style={{ fontWeight: 700, marginBottom: 6 }}>Backend errors</div>
                      <ul style={{ margin: 0, paddingLeft: 18 }}>
                        {backendErrors.map((e) => (
                          <li key={`${e.code}:${e.message}`} className="errorText" style={{ marginBottom: 4 }}>
                            <code>{e.code}</code>: {e.message}
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
                          <li key={`${w.code}:${w.message}`} className="small" style={{ marginBottom: 4 }}>
                            <code>{w.code}</code>: {w.message}
                          </li>
                        ))}
                      </ul>
                    </div>
                  )
                  : null}
              </div>
            )
            : null}
        </div>

        <div className="card">
          <div style={{ fontWeight: 700, marginBottom: 6 }}>Delete edge</div>
          <div className="small">
            Click the edge, then press <code>Delete</code> (or <code>Backspace</code>).
          </div>
        </div>
      </aside>

      <div style={{ position: "relative" }}>
        <ReactFlow
          nodes={nodes}
          edges={edges}
          nodeTypes={nodeTypes}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
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
              </div>
            )}
        </div>
      </aside>
    </div>
  );
}
