/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    // Unified template expansion engine for {{...}} placeholders.
    //
    // Supported patterns:
    //   {{inputs}}        — space-separated list of file_inputs
    //   {{outputs}}       — space-separated list of file_outputs
    //   {{input[N]}}      — N-th file_input  (0-based)
    //   {{output[N]}}     — N-th file_output (0-based)
    //   {{slot.NAME}}     — lookup NAME in inputValues
    //   {{env.NAME}}      — lookup NAME in envVariables
    //   {{inputs.NAME}}   — lookup NAME in inputValues (dataflow cross-reference)
    //   {{anything_else}} — bare lookup in inputValues
    //
    // Strict mode:  unknown keys and bad indices produce an error (shell tasks).
    // Lenient mode: unknown keys expand to "" (ai_call / per-item templates).

    enum class TemplateMode
    {
        Strict,
        Lenient
    };

    struct TemplateContext
    {
        std::vector<std::string> const* m_FileInputs = nullptr;
        std::vector<std::string> const* m_FileOutputs = nullptr;
        std::unordered_map<std::string, std::string> const* m_InputValues = nullptr;
        std::unordered_map<std::string, std::string> const* m_EnvVariables = nullptr;
    };

    // Expand all {{...}} placeholders in `templateText`.
    // Returns true on success.  On failure (strict mode only), returns false
    // and sets `errorMessage`.
    bool ExpandTemplate(std::string const& templateText, TemplateContext const& context, TemplateMode mode,
                        std::string& expandedOut, std::string& errorMessage);

    // Build a flat key-value map from a workflow "defaults" JSON object, prefixed with "defaults.".
    // E.g. {"ai":{"provider":"openai","model":"gpt-4.1-mini"},"timeout_ms":30000} becomes:
    //   "defaults.ai.provider" -> "openai"
    //   "defaults.ai.model"    -> "gpt-4.1-mini"
    //   "defaults.timeout_ms"  -> "30000"
    // Parse failures (malformed JSON, non-object root, exceptions) yield an empty/partial map
    // and emit a LOG_APP_WARN — callers proceed with whatever was parsed.  Used by both
    // workflow-load time (workflowRegistry) and dispatch time (aiCallTaskExecutor).
    std::unordered_map<std::string, std::string> BuildDefaultsMap(std::string const& defaultsJson);

    // Expand `{{defaults.X.Y}}` placeholders in `raw` using a defaults map built via
    // `BuildDefaultsMap`.  Unknown keys are expanded to "" (Lenient mode).  Pure pass-through
    // when `raw` contains no `{{` or `defaultsMap` is empty.  Returns `raw` unchanged if
    // `ExpandTemplate` reports an error.
    std::string ExpandWithDefaults(std::string const& raw,
                                   std::unordered_map<std::string, std::string> const& defaultsMap);

} // namespace AIAssistant
