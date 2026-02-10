export type JcwfVersion = "1.0" | "1.1";
export type JcwfDoc = string | string[];
export type JcwfTaskType = "python" | "shell" | "ai_call" | "internal";
export type JcwfTaskMode = "single" | "per_item";
export type JcwfFilterSourceKind = "csv" | "text_lines" | "query" | "polarion_query";
export type JcwfTriggerType = "auto" | "cron" | "file_watch" | "structure" | "manual";

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

  params?: Record<string, unknown>;

  // editor must preserve extra fields for round-trips
  [key: string]: unknown;
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
