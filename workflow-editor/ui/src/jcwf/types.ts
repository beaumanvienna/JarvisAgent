export type JcwfTriggerDef = {
  type: string;
  id: string;
  enabled?: boolean;
  params?: Record<string, unknown>;
};

export type JcwfAiDefaults = {
  provider?: string;
  model?: string;
};

export type JcwfDefaults = {
  timeout_ms?: number;
  ai?: JcwfAiDefaults;
};

export type JcwfTaskDef = {
  id: string;
  type: string;
  label?: string;
  doc?: string;
  working_directory?: string;
  depends_on?: string[];
  params?: Record<string, unknown>;
};

export type JcwfWorkflow = {
  version: string;
  id: string;
  label?: string;
  doc?: string;
  triggers?: JcwfTriggerDef[];
  defaults?: JcwfDefaults;
  tasks: Record<string, JcwfTaskDef>;
};
