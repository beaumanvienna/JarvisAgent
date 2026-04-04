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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    class PythonEngine;
    class Event;
    struct TaskDef;

    // Manages N PythonEngine instances, each backed by a CPython sub-interpreter
    // with its own GIL (PEP 684, Python 3.12+).  Workflow tasks are dispatched
    // to the engine with the smallest queue depth (load balancing).  Hook
    // callbacks (OnStart/OnUpdate/OnEvent/OnShutdown) are routed to the primary
    // engine (index 0) only.
    class PythonEnginePool
    {
    public:
        PythonEnginePool();
        ~PythonEnginePool();

        bool Initialize(std::string const& scriptPath, size_t engineCount);
        void Stop();
        void SignalStop();
        void WaitStop();

        void OnStart();
        void OnUpdate();
        void OnEvent(std::shared_ptr<Event> eventPtr);

        bool IsRunning() const { return m_Running; }
        size_t GetEngineCount() const { return m_Engines.size(); }

        bool ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                 std::unordered_map<std::string, std::string> const& inputValues,
                                 std::unordered_map<std::string, std::string> const& contextValues,
                                 std::unordered_map<std::string, std::string>& outputValuesOut,
                                 std::string& errorMessage, std::string& capturedStdout,
                                 std::string& capturedStderr);

    private:
        PythonEngine* SelectEngine();

        std::vector<std::unique_ptr<PythonEngine>> m_Engines;
        bool m_Running{false};
        bool m_PythonInitialized{false};
    };
} // namespace AIAssistant
