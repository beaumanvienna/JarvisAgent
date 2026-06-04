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
    //
    // Every user-origin string (prior turns + the new userMessage) is run through
    // DefangContextSentinels before being placed in the prompt.  This neutralises
    // attacker-supplied tokens that the AI would otherwise interpret as structural
    // boundaries (`<tool_call>`, `</tool_call>`, `<tool_result>`, `</tool_result>`,
    // `=== ... ===` section headers).
    class ContextAssembler
    {
    public:
        // Assemble a prompt from the current session state and user message.
        static AssembledPrompt Assemble(std::vector<AssistantTurn> const& recentTurns,
                                        std::string const& userMessage,
                                        std::string const& toolDescriptions = {});

        // Defang structural sentinels in user-origin text:
        // - delegates `<tool_call>` / `</tool_call>` / `<tool_result>` / `</tool_result>`
        //   to ToolRegistry::DefangToolMarkers (mathematical-angle-bracket replacement);
        // - replaces any run of 3+ `=` characters with the same number of U+2550
        //   (BOX DRAWINGS DOUBLE HORIZONTAL).  The system prompt uses literal `===`
        //   to delimit `=== Tool System ===`-style headers, so a user-supplied `===…===`
        //   would otherwise spoof those boundaries.
        [[nodiscard]] static std::string DefangContextSentinels(std::string const& text);

        // Caps on user-origin text appearing in the prompt — prevents OOM via crafted turns.
        static constexpr size_t kMaxUserMessageBytes = 64 * 1024;
        static constexpr size_t kMaxTurnTextBytes = 32 * 1024;
        static constexpr size_t kMaxConversationContextBytes = 128 * 1024;
        static constexpr size_t kMaxToolDescriptionsBytes = 64 * 1024;

    private:
        static std::string BuildSystemPrompt();
        static std::string BuildConversationContext(std::vector<AssistantTurn> const& turns);
    };
} // namespace AIAssistant
