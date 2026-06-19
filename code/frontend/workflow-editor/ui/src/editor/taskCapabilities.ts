// U4 — one source of truth for "what does each task type do with files" (S4). Replaces the scattered
// `targetReadsFileInputs` / `isInputTarget` predicates and the per-type branches in onConnect,
// validation, and path synthesis. Mirrors the backend executors; adding a task type is a one-row
// change here instead of touching five predicates.
//
// The Record is keyed by the *closed* JcwfTaskType union, so TypeScript flags a missing row when a
// new variant is added (the editor analogue of the "no default: arm over a closed enum" rule).

import type { JcwfTaskType } from "../jcwf/types";

// How a task receives / emits data.
export type InputMechanism = "file_inputs" | "queue_cntx" | "params" | "none";
export type OutputMechanism = "file_outputs" | "ai_reply" | "params_file" | "none";

export type TaskCapability = {
  input: InputMechanism;
  output: OutputMechanism;
  queueBinding: boolean; // ai_call STNG/TASK/CNTX/PROB assembly
  namedSlots: boolean;   // dataflow named value inputs/outputs
};

export const taskCapabilities: Record<JcwfTaskType, TaskCapability> = {
  python: { input: "file_inputs", output: "file_outputs", queueBinding: false, namedSlots: true },
  shell: { input: "file_inputs", output: "file_outputs", queueBinding: false, namedSlots: true },
  internal: { input: "file_inputs", output: "file_outputs", queueBinding: false, namedSlots: false },
  ai_call: { input: "queue_cntx", output: "ai_reply", queueBinding: true, namedSlots: true },
  sub_workflow: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  db_query: { input: "none", output: "params_file", queueBinding: false, namedSlots: false },
  // Cloud transfer types — file paths are `params` strings (param-level templating only); they do
  // NOT participate in the data-edge / file-port model.
  s3: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  onedrive_upload: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  onedrive_download: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  azure_blob_upload: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  azure_blob_download: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  gcs_upload: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  gcs_download: { input: "params", output: "params_file", queueBinding: false, namedSlots: false },
  // External-system actions — no file ports.
  polarion_write: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  snowflake_query: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  slack_message: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  email_send: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  email_read: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  github_issue: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  jira_issue: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  redmine_issue: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  sheets_read: { input: "none", output: "none", queueBinding: false, namedSlots: false },
  sheets_write: { input: "none", output: "none", queueBinding: false, namedSlots: false },
};

export function capabilityOf(type: JcwfTaskType): TaskCapability
{
  return taskCapabilities[type];
}

// Does an edge into this task carry a data input the editor should wire? True for the file-port
// model (file_inputs) and for ai_call's queue cntx. The cloud/`params` and `none` types are outside
// the edge model. This is the centralised replacement for the inlined
// `type === shell || python || internal || ai_call` predicates.
export function acceptsWiredInput(type: JcwfTaskType): boolean
{
  const input = taskCapabilities[type].input;
  return input === "file_inputs" || input === "queue_cntx";
}

// Does this task read its inputs as a `file_inputs` array (shell/python/internal)? ai_call reads via
// queue_binding cntx, so it is intentionally excluded here.
export function readsFileInputs(type: JcwfTaskType): boolean
{
  return taskCapabilities[type].input === "file_inputs";
}

// Does this task write its outputs as a declared `file_outputs` array?
export function producesFileOutputs(type: JcwfTaskType): boolean
{
  return taskCapabilities[type].output === "file_outputs";
}
