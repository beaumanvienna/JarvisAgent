import React, { useEffect, useState, useCallback } from "react";
import { authFetch } from "@shared/api/auth";

type TreeChild = {
  id: string;
  label: string;
  folderPath: string;
  parentId: string;
};

type TreeNode = {
  id: string;
  label: string;
  children: TreeNode[];
};

export default function WorkflowTreeView(props: {
  currentWorkflowId: string | null;
  rootWorkflowId: string | null;
  onNavigate: (workflowId: string) => void;
}): JSX.Element | null
{
  const [tree, setTree] = useState<TreeNode | null>(null);
  const [expanded, setExpanded] = useState<Set<string>>(new Set());

  const fetchTree = useCallback(async () =>
  {
    const rootId = props.rootWorkflowId ?? props.currentWorkflowId;
    if (!rootId) return;

    try
    {
      // Try the container tree endpoint first.
      const treeResponse = await authFetch(`/api/workflows/${encodeURIComponent(rootId)}/tree`);
      if (treeResponse.ok)
      {
        const data = await treeResponse.json();
        if (data.ok && Array.isArray(data.children) && data.children.length > 0)
        {
          const children = data.children as TreeChild[];
          const rootNode = buildTreeFromChildren(rootId, data.label || rootId, children);
          setTree(rootNode);
          return;
        }
      }

      // Fall back to dependency graph.
      const graphResponse = await authFetch("/api/workflows/dependency-graph");
      if (!graphResponse.ok) return;
      const graphData = await graphResponse.json();
      if (Array.isArray(graphData.edges) && graphData.edges.length > 0)
      {
        const edges = graphData.edges as { parent: string; child: string }[];
        const rootNode = buildTreeFromEdges(edges);
        setTree(rootNode);
        return;
      }

      setTree(null);
    }
    catch
    {
      // ignore fetch errors
    }
  }, [props.rootWorkflowId, props.currentWorkflowId]);

  useEffect(() =>
  {
    fetchTree();
  }, [fetchTree]);

  if (!tree) return null;

  function buildTreeFromChildren(rootId: string, rootLabel: string, children: TreeChild[]): TreeNode
  {
    const childrenByParent: Record<string, TreeChild[]> = {};
    for (const child of children)
    {
      if (!childrenByParent[child.parentId])
      {
        childrenByParent[child.parentId] = [];
      }
      childrenByParent[child.parentId].push(child);
    }

    function build(nodeId: string, nodeLabel: string): TreeNode
    {
      const kids = childrenByParent[nodeId] ?? [];
      return {
        id: nodeId,
        label: nodeLabel,
        children: kids.map((k) => build(k.id, k.label)),
      };
    }

    return build(rootId, rootLabel);
  }

  function buildTreeFromEdges(edges: { parent: string; child: string }[]): TreeNode | null
  {
    const childSet = new Set(edges.map((e) => e.child));
    const parentSet = new Set(edges.map((e) => e.parent));
    const rootIds = [...parentSet].filter((id) => !childSet.has(id));
    if (rootIds.length === 0) return null;

    const childrenOf: Record<string, string[]> = {};
    for (const edge of edges)
    {
      if (!childrenOf[edge.parent]) childrenOf[edge.parent] = [];
      childrenOf[edge.parent].push(edge.child);
    }

    function build(nodeId: string, visited: Set<string>): TreeNode
    {
      const node: TreeNode = { id: nodeId, label: nodeId, children: [] };
      if (visited.has(nodeId)) return node;
      visited.add(nodeId);
      const kids = childrenOf[nodeId] ?? [];
      for (const kid of kids)
      {
        node.children.push(build(kid, new Set(visited)));
      }
      return node;
    }

    return build(rootIds[0], new Set());
  }

  function toggleExpanded(nodeId: string)
  {
    setExpanded((prev) =>
    {
      const next = new Set(prev);
      if (next.has(nodeId)) next.delete(nodeId);
      else next.add(nodeId);
      return next;
    });
  }

  function renderNode(node: TreeNode, depth: number): JSX.Element
  {
    const isCurrent = node.id === props.currentWorkflowId;
    const hasChildren = node.children.length > 0;
    const isExpanded = expanded.has(node.id);

    return (
      <div key={node.id} style={{ paddingLeft: depth * 12 }}>
        <div
          style={{
            display: "flex", alignItems: "center", gap: 4, padding: "2px 4px",
            borderRadius: 3, cursor: "pointer", fontSize: 11,
            background: isCurrent ? "rgba(100,200,255,0.12)" : "transparent",
            fontWeight: isCurrent ? 700 : 400,
            color: isCurrent ? "rgba(140,220,255,0.95)" : "rgba(220,230,240,0.8)",
          }}
          onClick={() => props.onNavigate(node.id)}
        >
          {hasChildren && (
            <span
              style={{ opacity: 0.5, fontSize: 9, width: 10, textAlign: "center", flexShrink: 0 }}
              onClick={(e) => { e.stopPropagation(); toggleExpanded(node.id); }}
            >
              {isExpanded ? "\u25BC" : "\u25B6"}
            </span>
          )}
          {!hasChildren && <span style={{ width: 10, flexShrink: 0 }} />}
          <span style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
            {depth === 0 ? "\uD83D\uDCC1" : "\u29C9"} {node.label}
          </span>
        </div>
        {hasChildren && isExpanded && node.children.map((child) => renderNode(child, depth + 1))}
      </div>
    );
  }

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
      <div style={{ fontWeight: 700, marginBottom: 4, fontSize: 12 }}>Workflow Tree</div>
      {renderNode(tree, 0)}
    </div>
  );
}
