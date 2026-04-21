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

#include "event/event.h"
#include "workflow/aiReply.h"

namespace AIAssistant
{
    class AiCallStartedEvent final : public Event
    {
    public:
        AiCallStartedEvent(std::string probName, std::string interfaceName)
            : m_ProbName(std::move(probName)), m_InterfaceName(std::move(interfaceName))
        {
        }

        EventType GetEventType() const override { return EventType::AiCallStarted; }
        const char* GetName() const override { return "AiCallStartedEvent"; }
        int GetCategoryFlags() const override { return EventCategoryAi; }

        std::string const& GetProbName() const { return m_ProbName; }
        std::string const& GetInterfaceName() const { return m_InterfaceName; }

    private:
        std::string m_ProbName;
        std::string m_InterfaceName;
    };

    class AiCallCompletedEvent final : public Event
    {
    public:
        AiCallCompletedEvent(std::string probName, AiUsage usage, std::string finishReason)
            : m_ProbName(std::move(probName)), m_Usage(usage), m_FinishReason(std::move(finishReason))
        {
        }

        EventType GetEventType() const override { return EventType::AiCallCompleted; }
        const char* GetName() const override { return "AiCallCompletedEvent"; }
        int GetCategoryFlags() const override { return EventCategoryAi; }

        std::string const& GetProbName() const { return m_ProbName; }
        AiUsage const& GetUsage() const { return m_Usage; }
        std::string const& GetFinishReason() const { return m_FinishReason; }

    private:
        std::string m_ProbName;
        AiUsage m_Usage;
        std::string m_FinishReason;
    };

    class AiCallFailedEvent final : public Event
    {
    public:
        AiCallFailedEvent(std::string probName, AiError error)
            : m_ProbName(std::move(probName)), m_Error(std::move(error))
        {
        }

        EventType GetEventType() const override { return EventType::AiCallFailed; }
        const char* GetName() const override { return "AiCallFailedEvent"; }
        int GetCategoryFlags() const override { return EventCategoryAi; }

        std::string const& GetProbName() const { return m_ProbName; }
        AiError const& GetError() const { return m_Error; }

    private:
        std::string m_ProbName;
        AiError m_Error;
    };
} // namespace AIAssistant
