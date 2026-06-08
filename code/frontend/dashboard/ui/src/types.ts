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
  // True when the encrypted key store has been unlocked (master password
  // accepted). Used by the dashboard to detect a backend restart that
  // resealed the store while a tab is still open from a previous session.
  keys_unlocked?: boolean;
}

export interface ConnectionHealthEntry {
  name: string;
  circuit_state: string;
  consecutive_failures: number;
  // `true` once a Test click or JCWF cloud task has proved this connection works.
  // The dashboard's Cloud LED keys off this so merely-configured connections
  // stay grey until they have actual evidence of health.
  confirmed_healthy?: boolean;
}

export interface WorkflowEntry {
  id: string;
  label?: string;
  path?: string;
  manual_start?: boolean;
  has_ai_call?: boolean;
  is_sub_workflow?: boolean;
  // Resolved at workflow-load time from each ai_call task's provider field.
  // Empty string in the array means "system default provider".  Used by the
  // dashboard to mark rows red when any of these interfaces is degraded
  // (BillingExhausted / AuthFailure / ServiceOverload / ModelNotFound).
  interface_names?: string[];
}

// Wire shape for the ai-call-failed broadcast (webServer.cpp::BroadcastAiCallFailed,
// extended in Sitting 4 / Workstream B + Sitting 6 / Workstream E for cross-provider
// classification).  Branch on `category` (stable wire string), never on the raw
// provider strings.
export type ProviderErrorCategory =
  | "Unknown"
  | "BillingExhausted"
  | "ThrottleRateLimit"
  | "AuthFailure"
  | "ServiceOverload"
  | "ModelNotFound"
  | "InvalidRequest";

export interface AiCallFailedMessage {
  type: "ai-call-failed";
  prob: string;
  error_kind: number;
  http_status: number;
  error_message: string;
  provider_error_code: string;
  provider_error_type: string;
  category: ProviderErrorCategory;
  retry_after_seconds?: number;
  interface_name: string;
}

export interface AiCallCompletedMessage {
  type: "ai-call-completed";
  prob: string;
  interface_name: string;       // added Sitting-7 follow-up — drives the dashboard's auto-dismiss
  input_tokens: number;
  output_tokens: number;
  total_tokens: number;
  finish_reason: string;
}

export interface AiCallStartedMessage {
  type: "ai-call-started";
  prob: string;
  interface: string;
}

// Per-interface health snapshot from /api/providers/health (Sitting 8 /
// Workstream D).  Polled at the same cadence as /api/status; drives the
// dashboard's 6th LED ("AI Health") + click/hover popover.  Field shapes
// mirror webServer.cpp::HandleProvidersHealthGet — `*_ms` fields are Unix
// milliseconds (zero = epoch = "never").
export interface ProviderHealth {
  interface_name: string;
  interface_type_name: string;            // "API1".."API6" — for the shared-cap footnote
  quota_key: string;                      // controller key; empty if the interface has never dispatched
  is_mock: boolean;
  current_cap: number;                    // -1 when no controller exists yet
  max_cap: number;                        // -1 when no controller exists yet
  floor_cap: number;
  last_429_at_ms: number;                 // wall-clock of last actual 429; 0 = never throttled
  last_error_at_ms: number;               // 0 = never errored
  last_error_code: string;
  last_error_type: string;
  last_error_message: string;
  last_error_category: ProviderErrorCategory;
  last_http_status: number;
  retry_after_seconds?: number;
  consecutive_errors: number;
  success_streak_since_last_error: number;
  cap_pinned_at_floor_since_ms: number;   // 0 = not pinned
}

export interface ProvidersHealthResponse {
  ok: boolean;
  interfaces: ProviderHealth[];
}

// One row in the dashboard's per-interface alert state.  Keyed externally by
// `${interface_name}|${category}`; dedup happens at insert time so the count
// reflects burst size without spawning duplicate banners.
export interface ProviderAlertEntry {
  interfaceName: string;
  category: ProviderErrorCategory;
  count: number;
  firstSeenAt: number;     // ms epoch
  lastSeenAt: number;      // ms epoch
  errorCode: string;       // raw m_ProviderErrorCode (Anthropic/Bedrock can be empty)
  errorType: string;       // raw m_ProviderErrorType (always present post-Sitting-6)
  message: string;         // provider's free-form message — shown verbatim in the body
  retryAfterSeconds?: number;
  httpStatus: number;
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
