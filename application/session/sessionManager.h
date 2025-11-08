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
#include <future>
#include <array>

#include "engine.h"
#include "curlWrapper/curlManager.h"
#include "file/trackedFile.h"
#include "file/fileCategorizer.h"
#include "json/replyParser.h"
#include "log/statusLineRenderer.h"

namespace AIAssistant
{
    class SessionManager
    {
    private:
        class StateMachine
        {
        public:
            enum State
            {
                CompilingEnvironment = 0,
                SendingQueries,
                AllQueriesSent,
                AllResponsesReceived,
                NumStates
            };

            static constexpr std::array<std::string_view, State::NumStates> StateNames = {
                "CompilingEnvironment", //
                "SendingQueries",       //
                "AllQueriesSent",       //
                "AllResponsesReceived"  //
            };

            struct StateInfo
            {
                bool m_EnvironmentChanged{false};
                bool m_EnvironmentComplete{false};
                bool m_QueriesChanged{false};
                bool m_AllQueriesSent{false};
                bool m_AllResponsesReceived{false};
            };

            void OnUpdate(StateInfo& stateInfo);
            State GetState() const { return m_State; }

        private:
            State m_State{State::CompilingEnvironment};
        };

    public:
        SessionManager(std::string const& name);
        virtual ~SessionManager() = default;

        void OnUpdate();
        void OnEvent(Event&);
        void OnShutdown();

        bool IsIdle() const;

    public:
        std::string const& GetName() const { return m_Name; }

    private:
        void DispatchQuery(TrackedFile& requirementFile);
        void CheckForUpdates();
        void TrackInFlightQueries();
        void AssembleSettings();
        void AssembleContext();
        void AssembleTask();
        bool IsQueryRequired(TrackedFile& requirementFile) const;

    private:
        class Environment
        {
        public:
            bool GetDirty() const { return m_Dirty; };
            bool GetEnvironmentComplete() const { return m_EnvironmentComplete; };
            void Assemble(std::string& settings, std::string& context, std::string& tasks, CategorizedFiles&);
            std::string& GetEnvironmentAndResetDirtyFlag();
            void Reset();

        public:
            fs::file_time_type GetTimestamp() const { return m_Timestamp; }

        private:
            fs::file_time_type ComputeTimestamp(CategorizedFiles& categorized) const;
            fs::file_time_type m_Timestamp{};

        private:
            std::string m_EnvironmentCombined;
            bool m_EnvironmentComplete{false};
            bool m_Dirty{true};
        };

    private:
        std::string m_Name; // session name = folder name
        StateMachine m_StateMachine;

        FileCategorizer m_FileCategorizer;

        // Environment content
        Environment m_Environment;
        std::string m_Settings;
        std::string m_Context;
        std::string m_Tasks;

        // handles to queries
        std::vector<std::future<bool>> m_QueryFutures;

        std::string m_Url;
        std::string m_Model;

        std::unique_ptr<ReplyParser> m_ReplyParser;
        StatusLineRenderer m_StatusLineRenderer;
        size_t m_CompletedQueriesThisRun{0};
    };
} // namespace AIAssistant
