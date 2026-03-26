/* Copyright (c) 2025 JC Technolabs

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include "assistant/assistantSession.h"
#include <string>
#include <vector>

namespace AIAssistant
{
    // Assembled prompt ready for AI call (maps to STNG/TASK/CNTX/PROB queue files).
    struct AssembledPrompt
    {
        std::string stng; // system prompt (STNG_settings.txt)
        std::string task; // task instructions (TASK_instructions.txt) — empty for assistant
        std::string cntx; // conversation context (CNTX_context.txt)
        std::string prob; // user message (PROB file — triggers the AI call)
    };

    // Assembles the full prompt for an assistant AI call.
    //
    // Phase 1 (L1): system prompt + conversation history + user message.
    // Phase 3 (L2): adds workspace memory, rules, folder summaries.
    class ContextAssembler
    {
    public:
        // Assemble a prompt from the current session state and user message.
        // conversationBudgetTokens controls how many recent turns are included.
        static AssembledPrompt Assemble(std::vector<AssistantTurn> const& recentTurns, std::string const& userMessage,
                                        std::string const& toolDescriptions = {});

    private:
        static std::string BuildSystemPrompt();
        static std::string BuildConversationContext(std::vector<AssistantTurn> const& turns);
    };
} // namespace AIAssistant
