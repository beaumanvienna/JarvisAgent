export type JcwfVersion = "1.0";
export type JcwfDoc = string | string[];
export type JcwfTaskType = "python" | "shell" | "ai_call" | "internal";
export type JcwfTriggerType = "auto" | "cron" | "file_watch" | "structure" | "manual";

export type JcwfTrigger = {
  type: JcwfTriggerType;
  id: string;
  enabled?: boolean;
  params?: Record<string, unknown>;
};

export type JcwfTask = {
  id: string;
  type: JcwfTaskType;

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
