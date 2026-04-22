export interface StatusCapabilities {
  workflow_crud: boolean;
  workflow_run_endpoint: boolean;
  ai_assistant: boolean;
  ai_jcwf: boolean;
  settings_api: boolean;
}

export interface StatusResponse {
  ok: boolean;
  edition?: string;
  capabilities?: StatusCapabilities;
  workflows_registered: number;
  workflow_runs_active: number;
  ai_calls_inflight: number;
  websocket_clients: number;
  mcp_connected?: boolean;
  mcp_last_heartbeat_secs_ago?: number;
  connection_health?: ConnectionHealthEntry[];
}

export interface ConnectionHealthEntry {
  name: string;
  circuit_state: string;
  consecutive_failures: number;
}

export interface WorkflowEntry {
  id: string;
  label?: string;
  path?: string;
  manual_start?: boolean;
  has_ai_call?: boolean;
  is_sub_workflow?: boolean;
}

export interface WorkflowsListResponse {
  ok: boolean;
  workflows: WorkflowEntry[];
}

export interface TaskSnapshot {
  taskId: string;
  state: string;
  attemptCount: number;
  lastErrorMessage: string;
}

export interface RunSnapshot {
  runId: string;
  workflowId: string;
  state: string;
  tasks: TaskSnapshot[];
}

export interface WorkflowRunsSnapshot {
  type: "workflowRunsSnapshot";
  runs: RunSnapshot[];
}

export interface PythonStatus {
  type: "python-status";
  running: boolean;
}

export interface LastRunInfo {
  runId: string;
  workflowId: string;
  state: string;
  taskCount: number;
  startedAt: string;
  completedAt: string;
}

export interface LastRunsResponse {
  ok: boolean;
  runs: LastRunInfo[];
}

export type WsMessage = WorkflowRunsSnapshot | PythonStatus;

export interface LogResponse {
  ok: boolean;
  lines: string[];
  byteOffset: number;
  totalSize: number;
  totalLines?: number;
  error?: string;
}

export interface AnalyzeIssue {
  line: number;
  severity: string;
  text: string;
}

export interface KeysStatusResponse {
  ok: boolean;
  status: string;
  message: string;
  has_providers: boolean;
}

export interface KeysUnlockResponse {
  ok: boolean;
  status?: string;
  message?: string;
  error?: string;
  bootstrapped?: boolean;
  mcp_keys_loaded?: boolean;
  // Populated when the MCP key store was empty after unlock — the server has
  // created a fresh admin key and handed it back so the UI can display it
  // without the user fishing an enrollment token out of log/log.txt.
  admin_key?: {
    key_id: string;
    api_key: string;
    user: string;
    role: string;
    expires_at: string;
  };
}

export interface AnalyzeLastRunResponse {
  ok: boolean;
  found: boolean;
  message?: string;
  runIndex?: number;
  totalRuns?: number;
  runId?: string;
  workflowId?: string;
  state?: string;
  startedAt?: string;
  completedAt?: string;
  startLine?: number;
  endLine?: number;
  issues?: AnalyzeIssue[];
  issueCount?: number;
}
