# integration/n8n-node

Internal n8n custom node bundle for JarvisAgent.

## Node(s)

- `JarvisAgent: Start Workflow`
  - Calls `POST /api/integrations/n8n/start`
  - Returns the response under `item.json.jarvisAgent`

## Install into n8n (manual / internal)

This bundle is meant to be used as a repo-local custom node.

1. Copy (or symlink) this folder into your n8n custom extensions directory.
2. Restart n8n.
3. Add the node "JarvisAgent: Start Workflow" to a workflow.

You will need to adapt the exact installation step to your n8n deployment method.
