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

#include <string>
#include <future>
#include <queue>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <unordered_map>

#include "workflow/workflowTypes.h"

// Forward declarations — avoid including Python headers here
struct _object;
typedef _object PyObject;
struct _is;
typedef _is PyInterpreterState;

namespace AIAssistant
{
    class Event;

    struct WorkflowTaskRequest
    {
        TaskDef const* m_TaskDefinition{nullptr};
        std::string m_TaskWorkingDirectory;
        std::unordered_map<std::string, std::string> const* m_InputValues{nullptr};
        std::unordered_map<std::string, std::string> const* m_ContextValues{nullptr};

        // Result (filled by worker)
        std::unordered_map<std::string, std::string> m_OutputValues;
        std::string m_ErrorMessage;
        std::string m_CapturedStdout;
        std::string m_CapturedStderr;
        bool m_Success{false};

        std::promise<bool> m_Promise;
    };

    struct PythonTask
    {
        enum class Type
        {
            OnStart,
            OnUpdate,
            OnEvent,
            Shutdown,
            WorkflowTask
        };

        Type m_Type{};
        std::shared_ptr<Event> m_EventPtr;
        std::shared_ptr<WorkflowTaskRequest> m_WorkflowRequest;
    };

    // A single Python sub-interpreter with its own GIL and dedicated worker thread.
    // Created and managed by PythonEnginePool.
    class PythonEngine
    {
    public:
        explicit PythonEngine(size_t engineIndex);
        ~PythonEngine();

        // Called by PythonEnginePool on the main thread while the sub-interpreter's
        // GIL is held.  Sets up stdout redirect, sys.path, and (for the primary
        // engine) imports the hook module.
        bool SetupSubInterpreter(std::string const& scriptDir, std::string const& moduleName, bool loadHooks);

        void SetInterpreterState(PyInterpreterState* interpState) { m_InterpreterState = interpState; }
        void StartWorkerThread();

        void SignalStop();
        void WaitStop();

        void OnStart();
        void OnUpdate();
        void OnEvent(std::shared_ptr<Event> eventPtr);

        bool IsRunning() const { return m_Running; }
        size_t GetQueueDepth() const;
        size_t GetEngineIndex() const { return m_EngineIndex; }
        size_t GetTasksCompleted() const { return m_TasksCompleted; }

        bool ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                 std::unordered_map<std::string, std::string> const& inputValues,
                                 std::unordered_map<std::string, std::string> const& contextValues,
                                 std::unordered_map<std::string, std::string>& outputValuesOut, std::string& errorMessage,
                                 std::string& capturedStdout, std::string& capturedStderr);

    private:
        void WorkerLoop();
        void EnqueueTask(PythonTask const& task);

        void CallHook(PyObject* function, char const* hookName);
        void CallHookWithEvent(PyObject* function, char const* hookName, Event const& event);
        PyObject* BuildEventDict(Event const& event);

        // Executes a workflow task under the GIL (called on worker thread only)
        void ExecuteWorkflowTaskOnWorker(std::shared_ptr<WorkflowTaskRequest> const& request);

    private:
        bool m_Running{false};
        bool m_StopRequested{false};
        size_t m_EngineIndex{0};
        size_t m_TasksCompleted{0};

        std::string m_ScriptDir;
        std::string m_ModuleName;

        // Sub-interpreter state (set by PythonEnginePool after creation)
        PyInterpreterState* m_InterpreterState{nullptr};

        // Hook module and functions (primary engine only)
        PyObject* m_MainModule{nullptr};
        PyObject* m_MainDict{nullptr};
        PyObject* m_OnStartFunc{nullptr};
        PyObject* m_OnUpdateFunc{nullptr};
        PyObject* m_OnEventFunc{nullptr};
        PyObject* m_OnShutdownFunc{nullptr};

        // Worker thread and task queue
        std::thread m_WorkerThread;
        mutable std::mutex m_QueueMutex;
        std::condition_variable m_QueueCondition;
        std::queue<PythonTask> m_TaskQueue;
    };

} // namespace AIAssistant
