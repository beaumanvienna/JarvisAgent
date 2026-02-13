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

#include "workflow/templateEngine.h"

namespace AIAssistant
{
    namespace
    {
        std::string JoinFileList(std::vector<std::string> const& files)
        {
            std::string joined;
            for (size_t i = 0; i < files.size(); ++i)
            {
                if (i > 0)
                {
                    joined += ' ';
                }
                joined += files[i];
            }
            return joined;
        }
    } // anonymous namespace

    bool ExpandTemplate(std::string const& templateText, TemplateContext const& context, TemplateMode mode,
                        std::string& expandedOut, std::string& errorMessage)
    {
        expandedOut.clear();
        errorMessage.clear();

        std::size_t currentIndex = 0;

        while (currentIndex < templateText.size())
        {
            std::size_t const openPos = templateText.find("{{", currentIndex);
            if (openPos == std::string::npos)
            {
                expandedOut += templateText.substr(currentIndex);
                break;
            }

            // Copy literal text before the placeholder.
            expandedOut += templateText.substr(currentIndex, openPos - currentIndex);

            std::size_t const closePos = templateText.find("}}", openPos + 2);
            if (closePos == std::string::npos)
            {
                if (mode == TemplateMode::Strict)
                {
                    errorMessage = "malformed template (missing closing '}}') near position " + std::to_string(openPos);
                    return false;
                }

                // Lenient: copy remainder as literal.
                expandedOut += templateText.substr(openPos);
                break;
            }

            std::string const key = templateText.substr(openPos + 2, closePos - (openPos + 2));
            std::string replacement;
            bool resolved = false;

            // ---- {{inputs}} ----
            if (key == "inputs")
            {
                if (context.m_FileInputs != nullptr)
                {
                    replacement = JoinFileList(*context.m_FileInputs);
                }
                resolved = true;
            }
            // ---- {{outputs}} ----
            else if (key == "outputs")
            {
                if (context.m_FileOutputs != nullptr)
                {
                    replacement = JoinFileList(*context.m_FileOutputs);
                }
                resolved = true;
            }
            // ---- {{input[N]}} ----
            else if (key.size() > 6 && key.rfind("input[", 0) == 0 && key.back() == ']')
            {
                std::string const indexStr = key.substr(6, key.size() - 7);
                try
                {
                    size_t const idx = static_cast<size_t>(std::stoul(indexStr));
                    if (context.m_FileInputs == nullptr || idx >= context.m_FileInputs->size())
                    {
                        if (mode == TemplateMode::Strict)
                        {
                            errorMessage = "input index " + indexStr + " out of range";
                            return false;
                        }
                    }
                    else
                    {
                        replacement = (*context.m_FileInputs)[idx];
                    }
                }
                catch (...)
                {
                    if (mode == TemplateMode::Strict)
                    {
                        errorMessage = "invalid input index '" + indexStr + "'";
                        return false;
                    }
                }
                resolved = true;
            }
            // ---- {{output[N]}} ----
            else if (key.size() > 7 && key.rfind("output[", 0) == 0 && key.back() == ']')
            {
                std::string const indexStr = key.substr(7, key.size() - 8);
                try
                {
                    size_t const idx = static_cast<size_t>(std::stoul(indexStr));
                    if (context.m_FileOutputs == nullptr || idx >= context.m_FileOutputs->size())
                    {
                        if (mode == TemplateMode::Strict)
                        {
                            errorMessage = "output index " + indexStr + " out of range";
                            return false;
                        }
                    }
                    else
                    {
                        replacement = (*context.m_FileOutputs)[idx];
                    }
                }
                catch (...)
                {
                    if (mode == TemplateMode::Strict)
                    {
                        errorMessage = "invalid output index '" + indexStr + "'";
                        return false;
                    }
                }
                resolved = true;
            }
            // ---- {{env.NAME}} ----
            else if (key.size() > 4 && key.rfind("env.", 0) == 0)
            {
                std::string const envName = key.substr(4);
                if (context.m_EnvVariables != nullptr)
                {
                    auto const it = context.m_EnvVariables->find(envName);
                    if (it != context.m_EnvVariables->end())
                    {
                        replacement = it->second;
                    }
                }
                // Missing env variable expands to "" (same as old shell behaviour).
                resolved = true;
            }
            // ---- {{slot.NAME}} ----
            else if (key.size() > 5 && key.rfind("slot.", 0) == 0)
            {
                std::string const slotName = key.substr(5);
                if (context.m_InputValues != nullptr)
                {
                    auto const it = context.m_InputValues->find(slotName);
                    if (it != context.m_InputValues->end())
                    {
                        replacement = it->second;
                        resolved = true;
                    }
                }

                if (!resolved && mode == TemplateMode::Strict)
                {
                    errorMessage = "unknown slot '" + slotName + "'";
                    return false;
                }
                resolved = true;
            }
            // ---- {{inputs.NAME}} (dataflow cross-reference) ----
            else if (key.size() > 7 && key.rfind("inputs.", 0) == 0)
            {
                std::string const inputName = key.substr(7);
                if (context.m_InputValues != nullptr)
                {
                    auto const it = context.m_InputValues->find(inputName);
                    if (it != context.m_InputValues->end())
                    {
                        replacement = it->second;
                        resolved = true;
                    }
                }

                if (!resolved && mode == TemplateMode::Strict)
                {
                    errorMessage = "unknown input reference '" + inputName + "'";
                    return false;
                }
                resolved = true;
            }

            // ---- bare key: lookup in inputValues ----
            if (!resolved)
            {
                if (context.m_InputValues != nullptr)
                {
                    auto const it = context.m_InputValues->find(key);
                    if (it != context.m_InputValues->end())
                    {
                        replacement = it->second;
                        resolved = true;
                    }
                }

                if (!resolved && mode == TemplateMode::Strict)
                {
                    errorMessage = "unknown template key '" + key + "'";
                    return false;
                }
            }

            expandedOut += replacement;
            currentIndex = closePos + 2;
        }

        return true;
    }

} // namespace AIAssistant
