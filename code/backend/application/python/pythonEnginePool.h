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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{
    class PythonEngine;
    class Event;
    class ScriptRegistry;
    struct TaskDef;

    // Manages N PythonEngine instances, each backed by a CPython sub-interpreter
    // with its own GIL (PEP 684, Python 3.12+).  Workflow tasks are dispatched
    // to the engine with the smallest queue depth (load balancing).  Hook
    // callbacks (OnStart/OnUpdate/OnEvent/OnShutdown) are routed to the primary
    // engine (index 0) only.
    //
    // On Python < 3.12, the pool falls back to a single engine using the legacy
    // Py_NewInterpreter API (shared GIL, no configuration options).
    //
    // Threading & lifetime contract:
    //   * Initialize() and WaitStop() are the only mutators of m_Engines.  They
    //     hold m_Mutex for the entire mutation (push_back loop / clear).  After
    //     Initialize returns true and before SignalStop is invoked, m_Engines is
    //     stable and individual engine pointers can be dereferenced lock-free
    //     from worker threads (each engine owns its own state + queue).
    //   * m_Running is atomic so IsRunning() / ExecuteWorkflowTask() can read it
    //     without taking m_Mutex.  Its store happens after the pool is fully
    //     initialized; the load establishes the ordering needed to safely use
    //     m_Engines.
    //   * Pool is non-copyable / non-movable — the engines own raw Python state
    //     that must not migrate.
    class PythonEnginePool
    {
    public:
        PythonEnginePool();
        ~PythonEnginePool();

        PythonEnginePool(PythonEnginePool const&) = delete;
        PythonEnginePool& operator=(PythonEnginePool const&) = delete;
        PythonEnginePool(PythonEnginePool&&) = delete;
        PythonEnginePool& operator=(PythonEnginePool&&) = delete;

        // scriptRegistry is borrowed (owned by JarvisAgent).  Must outlive the
        // pool — JarvisAgent destroys the pool first, then the registry.  Used
        // to allowlist Python module names before PyImport_ImportModule.
        [[nodiscard]] bool Initialize(std::string const& scriptPath, size_t engineCount,
                                      ScriptRegistry const* scriptRegistry);
        void Stop();
        void SignalStop();
        void WaitStop();

        void OnStart();
        void OnUpdate();
        void OnEvent(std::shared_ptr<Event> eventPtr);

        bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }
        size_t GetEngineCount() const;
        size_t GetTasksCompleted(size_t engineIndex) const;

        [[nodiscard]] bool ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                               std::string const& workflowId, std::string const& runId,
                                               std::unordered_map<std::string, std::string> const& inputValues,
                                               std::unordered_map<std::string, std::string> const& contextValues,
                                               std::unordered_map<std::string, std::string>& outputValuesOut,
                                               std::string& errorMessage, std::string& capturedStdout,
                                               std::string& capturedStderr);

    private:
        PythonEngine* SelectEngine();

        // Guards mutation of m_Engines (Initialize push_back loop, WaitStop
        // clear).  Lock-free reads from worker threads after Initialize is
        // complete are valid per the class-level threading contract.
        mutable std::mutex m_Mutex;
        std::vector<std::unique_ptr<PythonEngine>> m_Engines;
        std::atomic<bool> m_Running{false};
        bool m_PythonInitialized{false};
    };
} // namespace AIAssistant
