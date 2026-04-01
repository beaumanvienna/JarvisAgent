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
  session_managers_total: number;
  session_managers_with_inflight: number;
  session_managers_inflight_total: number;
  websocket_clients: number;
}

export interface WorkflowEntry {
  id: string;
  label?: string;
  path?: string;
  manual_start?: boolean;
  has_ai_call?: boolean;
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

export interface SessionStatus {
  name: string;
  state: string;
  outputs: number;
  inflight: number;
  completed: number;
  failed: number;
  last_error_code?: number;
  last_error_message?: string;
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

export type WsMessage = WorkflowRunsSnapshot | SessionStatus | PythonStatus;

export interface LogResponse {
  ok: boolean;
  lines: string[];
  byteOffset: number;
  totalSize: number;
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
