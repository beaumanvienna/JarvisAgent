export type ConfigSettings = {
  ok: boolean;
  api_index: number;
  max_threads: number;
  verbose: boolean;
  max_file_size_kb: number;
  jcwf_batch_size: number;
  jcwf_ai_interface: number;
  queue_folder: string;
  workflows_folder: string;
  interface_count: number;
};

export type ConfigSettingsUpdateResponse = {
  ok: boolean;
  api_index?: number;
  max_threads?: number;
  verbose?: boolean;
  max_file_size_kb?: number;
  jcwf_batch_size?: number;
  jcwf_ai_interface?: number;
  error?: string;
  message?: string;
};

export async function getConfigSettings(): Promise<ConfigSettings>
{
  const response = await fetch("/api/settings/config");
  return (await response.json()) as ConfigSettings;
}

export async function updateConfigSettings(
  settings: Partial<Pick<ConfigSettings, "api_index" | "max_threads" | "verbose" | "max_file_size_kb" | "jcwf_batch_size" | "jcwf_ai_interface">>
): Promise<ConfigSettingsUpdateResponse>
{
  const response = await fetch("/api/settings/config", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(settings),
  });
  return (await response.json()) as ConfigSettingsUpdateResponse;
}
