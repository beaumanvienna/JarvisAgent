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
    class ScriptRegistry;

    struct WorkflowTaskRequest
    {
        TaskDef const* m_TaskDefinition{nullptr};
        std::string m_TaskWorkingDirectory;
        // Owned copies — the worker thread runs after the caller's stack frame
        // returns from ExecuteWorkflowTask only on the cancel-during-shutdown
        // drain path, so the originals must not be referenced.  Copy at enqueue
        // time; the maps are small and not on a hot path.
        std::unordered_map<std::string, std::string> m_InputValues;
        std::unordered_map<std::string, std::string> m_ContextValues;

        // Run identity — included in fail-path logs so the dashboard's Run
        // Analyzer can attribute failures to the originating run.
        std::string m_WorkflowId;
        std::string m_RunId;

        // Result (filled by worker)
        std::unordered_map<std::string, std::string> m_OutputValues;
        std::string m_ErrorMessage;
        std::string m_CapturedStdout;
        std::string m_CapturedStderr;
        bool m_Success{false};

        std::promise<bool> m_Promise;
        // Guards against a double set_value() between the worker-loop main
        // path and the shutdown-drain path racing on the same request — a
        // double-set throws std::future_error: broken_promise.
        std::atomic<bool> m_PromiseSatisfied{false};
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
    //
    // Lifetime invariants:
    //   - m_InterpreterState: set once by PythonEnginePool::Initialize via
    //     SetInterpreterState() and read by the worker thread under the GIL.
    //     The pointed-to PyInterpreterState lives for the lifetime of the
    //     CPython process (sub-interpreters are torn down only by Py_Finalize
    //     at process exit; PythonEnginePool intentionally leaks the main-thread
    //     state per its Initialize() comment).  Therefore m_InterpreterState is
    //     valid for the entire WorkerLoop, including any PyThreadState_New call.
    //   - m_ScriptRegistry: borrowed pointer owned by JarvisAgent.  The pool is
    //     destroyed before the registry, so the pointer is valid for the
    //     engine's whole lifetime (asserted at use sites).
    class PythonEngine
    {
    public:
        explicit PythonEngine(size_t engineIndex);
        ~PythonEngine();

        // Called by PythonEnginePool on the main thread while the sub-interpreter's
        // GIL is held.  Sets up stdout redirect, sys.path, and (for the primary
        // engine) imports the hook module.  Returns false if scriptDir does not
        // resolve to a directory inside the project root — the path-traversal
        // gate that defends sys.path from arbitrary-location injection.
        bool SetupSubInterpreter(std::string const& scriptDir, std::string const& moduleName, bool loadHooks);

        void SetInterpreterState(PyInterpreterState* interpState) { m_InterpreterState = interpState; }
        void SetScriptRegistry(ScriptRegistry const* registry) { m_ScriptRegistry = registry; }
        void StartWorkerThread();

        void SignalStop();
        void WaitStop();

        void OnStart();
        void OnUpdate();
        void OnEvent(std::shared_ptr<Event> eventPtr);

        bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }
        size_t GetQueueDepth() const;
        size_t GetEngineIndex() const { return m_EngineIndex; }
        size_t GetTasksCompleted() const { return m_TasksCompleted.load(std::memory_order_relaxed); }

        bool ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                 std::string const& workflowId, std::string const& runId,
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
        // m_Running is set once in StartWorkerThread (main thread) and cleared
        // in WaitStop (main thread); read in IsRunning() and the public
        // OnStart/OnUpdate/OnEvent/SignalStop early-returns (any thread that
        // owns the engine pointer).  Atomic so cross-thread reads see the
        // correct value without depending on m_QueueMutex acquisition.
        std::atomic<bool> m_Running{false};
        // m_StopRequested is also guarded by m_QueueMutex on every read+write
        // path (the cv predicate needs the lock anyway), but atomic semantics
        // are the contract — drop the mutex without changing behaviour.
        std::atomic<bool> m_StopRequested{false};
        size_t m_EngineIndex{0};
        std::atomic<size_t> m_TasksCompleted{0};

        std::string m_ScriptDir;
        std::string m_ModuleName;

        // Sub-interpreter state (set by PythonEnginePool after creation).
        // See class-level comment for the lifetime invariant.
        PyInterpreterState* m_InterpreterState{nullptr};

        // Script registry (borrowed pointer owned by JarvisAgent).
        // Used to allowlist module names against the registered scripts catalog
        // before passing them to PyImport_ImportModule — without this gate, any
        // workflow author could import 'os' / 'subprocess' / 'ctypes' and call
        // arbitrary system functions.
        ScriptRegistry const* m_ScriptRegistry{nullptr};

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
