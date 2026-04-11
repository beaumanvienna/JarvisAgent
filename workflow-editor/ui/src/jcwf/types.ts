export type JcwfVersion = "1.0" | "1.1";
export type JcwfDoc = string | string[];
export type JcwfTaskType = "python" | "shell" | "ai_call" | "internal" | "sub_workflow" | "polarion_write" | "s3" | "db_query" | "onedrive_upload" | "onedrive_download";
export type JcwfTaskMode = "single" | "per_item";
export type JcwfFilterSourceKind = "csv" | "text_lines" | "query" | "polarion_query";
export type JcwfTriggerType = "auto" | "cron" | "file_watch" | "structure" | "manual" | "webhook" | "s3_watch" | "onedrive_watch";

export type JcwfTrigger = {
  type: JcwfTriggerType;
  id: string;
  enabled?: boolean;
  params?: Record<string, unknown>;
};

export type JcwfFilterSource = {
  kind: JcwfFilterSourceKind;
  path?: string;
  query?: string;
  index_path?: string;
  fields?: string[];
  delimiter?: string;
  columns?: string[];
  header_row?: number;
  range?: string;
  connection?: string;
  base_url?: string;
  project_id?: string;
  key_name?: string;
  page_size?: number;
};

export type JcwfFilter = {
  id: string;
  source: JcwfFilterSource;
  binding?: string;
  max_items?: number;
};

export type JcwfTask = {
  id: string;
  type: JcwfTaskType;

  mode?: JcwfTaskMode;
  filter?: string;

  label?: string;
  doc?: JcwfDoc;

  working_directory?: string;

  depends_on?: string[];

  expose_error_signal?: boolean;

  params?: Record<string, unknown>;

  // sub_workflow tasks: path to child .jcwf file (relative to parent workflow directory)
  workflow_file?: string;

  // editor must preserve extra fields for round-trips
  [key: string]: unknown;
};

export type JcwfControlNodeType = "branch";

export type JcwfControlNode = {
  id: string;
  type: JcwfControlNodeType;
  label?: string;

  // editor must preserve extra fields for round-trips
  [key: string]: unknown;
};

export type JcwfControlflowKind = "normal" | "error_signal" | "on_error";

export type JcwfControlflowEdge = {
  from: string;
  to: string;
  kind: JcwfControlflowKind;
  from_port: string;
  to_port: string;
};

export type JcwfDataflowEntry = {
  from_task: string;
  from_output: string;
  to_task: string;
  to_input: string;
};

export type JcwfQueueFileEntry = {
  path: string;
  content: string;
};

export type JcwfQueueBinding = {
  stng_files?: (JcwfQueueFileEntry | string)[];
  task_files?: (JcwfQueueFileEntry | string)[];
  cntx_files?: (JcwfQueueFileEntry | string)[];
  prob_files?: (JcwfQueueFileEntry | string)[];
};

export type JcwfFile = {
  version: JcwfVersion;
  id: string;

  label?: string;
  doc?: JcwfDoc;

  manual_start?: boolean;
  triggers?: JcwfTrigger[];

  tasks: Record<string, JcwfTask>;

  filters?: JcwfFilter[];

  control_nodes?: JcwfControlNode[];
  controlflow?: JcwfControlflowEdge[];

  [key: string]: unknown;
};

export function coerceTasksToRecord(tasksValue: unknown): Record<string, JcwfTask>
{
  if (tasksValue && typeof tasksValue === "object" && !Array.isArray(tasksValue))
  {
    return tasksValue as Record<string, JcwfTask>;
  }

  return {};
}
