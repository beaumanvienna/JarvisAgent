import { authFetch } from "@shared/api/auth";

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
  use_bash: boolean;
  platform: "windows" | "linux" | "macos";
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
  const response = await authFetch("/api/settings/config");
  return (await response.json()) as ConfigSettings;
}

// The config-settings PUT is re-auth gated on the backend (it can change the
// default/jcwf interface, which is routing).  Optional `masterPassword` at the
// type level only so callers can be wired incrementally; omitting it → 401.
export async function updateConfigSettings(
  settings: Partial<Pick<ConfigSettings, "api_index" | "max_threads" | "verbose" | "max_file_size_kb" | "jcwf_batch_size" | "jcwf_ai_interface" | "use_bash">>,
  masterPassword?: string,
): Promise<ConfigSettingsUpdateResponse>
{
  const response = await authFetch("/api/settings/config", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...settings, master_password: masterPassword }),
  });
  return (await response.json()) as ConfigSettingsUpdateResponse;
}
