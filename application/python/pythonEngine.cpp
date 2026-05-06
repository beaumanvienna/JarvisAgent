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

#include "engine.h"
#include "pythonEngine.h"

#include <cassert>
#include <filesystem>

// Wrap Python.h to force linking against the Release library (python314.lib)
// even in Debug builds, because python314_d.lib is rarely available.
#if defined(_WIN32) && defined(_DEBUG)
#undef _DEBUG
#include <Python.h>
#define _DEBUG
#else
#include <Python.h>
#endif

#include "log/log.h"
#include "event/event.h"
#include "event/filesystemEvent.h"
#include "event/pythonErrorEvent.h"

#include "file/pathConfinement.h"
#include "file/scriptRegistry.h"
#include "jarvisAgent.h"
#include "workflow/workflowTypes.h" // SanitizeUtf8 + TruncateUtf8Safe

namespace fs = std::filesystem;

#ifdef _WIN32
#define JARVIS_PY_EXPORT extern "C" __declspec(dllexport)
#else
#define JARVIS_PY_EXPORT extern "C"
#endif

JARVIS_PY_EXPORT void JarvisRedirectPython(char const* message)
{
    if (message == nullptr)
    {
        return;
    }

    // Send into std::cout so it flows through TerminalLogStreamBuf
    std::cout << message << std::endl;
}

JARVIS_PY_EXPORT void JarvisPyStatus(char const* message)
{
    if (message == nullptr)
    {
        return;
    }

    // Log Python-side error for visibility
    std::cout << "[PYTHON-ERROR] " << message << std::endl;

    // stop python
    {
        auto event = std::make_shared<AIAssistant::PythonCrashedEvent>(message);

        AIAssistant::Core::g_Core->PushEvent(event);
    }
}

namespace AIAssistant
{

    namespace
    {
        // Truncate sanitised UTF-8 to a sensible bound for log lines / error
        // payloads — Python tracebacks can run kilobytes long; nobody reads
        // past the first frame.
        constexpr size_t kMaxPythonErrorChars = 4096;

        std::string SanitizePythonErrorMessage(std::string const& raw)
        {
            return TruncateUtf8Safe(SanitizeUtf8(raw), kMaxPythonErrorChars);
        }
    }

    PythonEngine::PythonEngine(size_t engineIndex) : m_EngineIndex(engineIndex) {}
    PythonEngine::~PythonEngine() {}

    // ============================================================================
    //   SetupSubInterpreter — called by PythonEnginePool on the main thread
    //   while the sub-interpreter's GIL is held.
    // ============================================================================
    bool PythonEngine::SetupSubInterpreter(std::string const& scriptDir, std::string const& moduleName, bool loadHooks)
    {
        // Validate scriptDir before storing or using it on sys.path.  A path
        // that escapes the project root would let an attacker place importable
        // modules (e.g. a forged `os.py`) on Python's import path, then have a
        // workflow's `module` field resolve to the forged copy.  Reject up
        // front; the launcher invariant is that scriptDir lives under
        // <project root>/scripts/.
        fs::path const scriptDirCanonical = ConfineUnderProjectRoot(scriptDir);
        if (scriptDirCanonical.empty())
        {
            LOG_APP_ERROR("PythonEngine[{}]: rejected scriptDir '{}' — does not resolve under project root",
                          m_EngineIndex, scriptDir);
            return false;
        }

        m_ScriptDir = scriptDirCanonical.string();
        m_ModuleName = moduleName;

        // NOTE: Caller (PythonEnginePool) holds this sub-interpreter's GIL.

        // -----------------------------------------------------------------
        // Install stdout/stderr redirection
        // -----------------------------------------------------------------
        char const* redirectCode = "import sys\n"
                                   "import ctypes\n"
                                   "def _jarvis_cdll():\n"
                                   "    if sys.platform == 'win32':\n"
                                   "        return ctypes.CDLL(sys.executable)\n"
                                   "    return ctypes.CDLL(None)\n"
                                   "_jarvis_C = _jarvis_cdll()\n"
                                   "class _JarvisRedirect:\n"
                                   "    def write(self, msg):\n"
                                   "        try:\n"
                                   "            _jarvis_C.JarvisRedirectPython(msg.encode('utf-8'))\n"
                                   "        except Exception:\n"
                                   "            pass\n"
                                   "    def flush(self):\n"
                                   "        pass\n"
                                   "r = _JarvisRedirect()\n"
                                   "sys.stdout = r\n"
                                   "sys.stderr = r\n";

        if (PyRun_SimpleString(redirectCode) != 0)
        {
            LOG_APP_ERROR("PythonEngine[{}]: failed to install stdout/stderr redirect", m_EngineIndex);
        }

        // -----------------------------------------------------------------
        // Add script directory to sys.path
        // -----------------------------------------------------------------
        PyObject* sysModule = PyImport_ImportModule("sys");
        if (!sysModule)
        {
            PyErr_Print();
            LOG_APP_ERROR("PythonEngine[{}]: failed to import 'sys'", m_EngineIndex);
            return false;
        }

        PyObject* sysPathList = PyObject_GetAttrString(sysModule, "path");
        if (!sysPathList || !PyList_Check(sysPathList))
        {
            Py_XDECREF(sysPathList);
            Py_DECREF(sysModule);
            LOG_APP_ERROR("PythonEngine[{}]: sys.path is not a list", m_EngineIndex);
            return false;
        }

        PyObject* directoryString = PyUnicode_FromString(m_ScriptDir.c_str());
        PyList_Append(sysPathList, directoryString);
        Py_DECREF(directoryString);

        // Also add the parent of m_ScriptDir (i.e. the launch CWD) so that
        // dotted package imports like "scripts.parseOpenSSHLog" work.
        // m_ScriptDir is already canonical and confined under project root by
        // the validation above, so parent_path() is the project root itself —
        // no further check needed here, but the assertion makes the invariant
        // visible to a future reader.
        {
            fs::path const parentPath = fs::path(m_ScriptDir).parent_path();
            std::string const parentDir = parentPath.string();
            if (!parentDir.empty())
            {
                PyObject* parentDirString = PyUnicode_FromString(parentDir.c_str());
                PyList_Append(sysPathList, parentDirString);
                Py_DECREF(parentDirString);
                LOG_APP_INFO("PythonEngine[{}]: added '{}' to sys.path (package imports)", m_EngineIndex, parentDir);
            }
        }

        Py_DECREF(sysPathList);
        Py_DECREF(sysModule);

        // -----------------------------------------------------------------
        // Import hook module (primary engine only)
        // -----------------------------------------------------------------
        if (loadHooks)
        {
            PyObject* moduleNameObj = PyUnicode_FromString(m_ModuleName.c_str());
            if (!moduleNameObj)
            {
                LOG_APP_ERROR("PythonEngine[{}]: failed to allocate module name '{}'", m_EngineIndex, m_ModuleName);
                return false;
            }

            m_MainModule = PyImport_Import(moduleNameObj);
            Py_DECREF(moduleNameObj);

            if (!m_MainModule)
            {
                PyErr_Print();
                LOG_APP_ERROR("PythonEngine[{}]: failed to import module '{}'", m_EngineIndex, m_ModuleName);
                return false;
            }

            m_MainDict = PyModule_GetDict(m_MainModule);
            if (!m_MainDict)
            {
                LOG_APP_ERROR("PythonEngine[{}]: failed to retrieve module dict", m_EngineIndex);
                Py_DECREF(m_MainModule);
                m_MainModule = nullptr;
                return false;
            }

            auto loadHookFunc = [&](char const* hookName, PyObject*& outFunc)
            {
                outFunc = nullptr;

                PyObject* functionObject = PyDict_GetItemString(m_MainDict, hookName);
                if (functionObject && PyCallable_Check(functionObject))
                {
                    Py_INCREF(functionObject);
                    outFunc = functionObject;
                    LOG_APP_INFO("PythonEngine[{}]: found hook '{}()'", m_EngineIndex, hookName);
                }
                else
                {
                    LOG_APP_INFO("PythonEngine[{}]: hook '{}()' not defined", m_EngineIndex, hookName);
                }
            };

            loadHookFunc("OnStart", m_OnStartFunc);
            loadHookFunc("OnUpdate", m_OnUpdateFunc);
            loadHookFunc("OnEvent", m_OnEventFunc);
            loadHookFunc("OnShutdown", m_OnShutdownFunc);
        }

        LOG_APP_INFO("PythonEngine[{}]: sub-interpreter setup complete (hooks={})", m_EngineIndex, loadHooks);
        return true;
    }

    // ============================================================================
    //   Worker thread
    // ============================================================================
    void PythonEngine::StartWorkerThread()
    {
        m_Running.store(true, std::memory_order_release);
        m_WorkerThread = std::thread(&PythonEngine::WorkerLoop, this);
    }

    void PythonEngine::WorkerLoop()
    {
        // m_InterpreterState is set once by PythonEnginePool::Initialize before
        // StartWorkerThread, and points to a CPython sub-interpreter that lives
        // for the entire process (sub-interpreters are torn down only at
        // Py_Finalize on process exit; the pool intentionally keeps the
        // main-thread state alive).  See class-level lifetime invariant.
        assert(m_InterpreterState != nullptr && "PythonEngine: m_InterpreterState must be set before StartWorkerThread");
        PyThreadState* threadState = PyThreadState_New(m_InterpreterState);
        if (threadState == nullptr)
        {
            LOG_APP_ERROR("PythonEngine[{}]: failed to create thread state", m_EngineIndex);
            return;
        }

        while (true)
        {
            PythonTask task;

            {
                std::unique_lock<std::mutex> lock(m_QueueMutex);
                m_QueueCondition.wait(lock, [&]()
                                      { return m_StopRequested.load(std::memory_order_acquire) || !m_TaskQueue.empty(); });

                if (m_StopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                task = m_TaskQueue.front();
                m_TaskQueue.pop();
            }

            // Acquire this sub-interpreter's GIL
            PyEval_RestoreThread(threadState);

            switch (task.m_Type)
            {
                case PythonTask::Type::OnStart:
                {
                    if (m_OnStartFunc)
                    {
                        CallHook(m_OnStartFunc, "OnStart");
                    }
                    break;
                }

                case PythonTask::Type::OnUpdate:
                {
                    if (m_OnUpdateFunc)
                    {
                        CallHook(m_OnUpdateFunc, "OnUpdate");
                    }
                    break;
                }

                case PythonTask::Type::OnEvent:
                {
                    if (m_OnEventFunc && task.m_EventPtr)
                    {
                        CallHookWithEvent(m_OnEventFunc, "OnEvent", *task.m_EventPtr);
                    }
                    break;
                }

                case PythonTask::Type::Shutdown:
                {
                    if (m_OnShutdownFunc)
                    {
                        CallHook(m_OnShutdownFunc, "OnShutdown");
                    }
                    break;
                }

                case PythonTask::Type::WorkflowTask:
                {
                    if (task.m_WorkflowRequest)
                    {
                        ExecuteWorkflowTaskOnWorker(task.m_WorkflowRequest);
                        m_TasksCompleted.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }

            // Release this sub-interpreter's GIL
            threadState = PyEval_SaveThread();

            // Check stop request between tasks so we don't block on a backlog
            // of queued OnEvent tasks during shutdown.
            if (m_StopRequested.load(std::memory_order_acquire))
            {
                break;
            }
        }

        // Drain remaining WorkflowTask requests so callers don't block forever
        // on resultFuture.get() during shutdown.  m_PromiseSatisfied guards
        // against double-set racing with the main path: if
        // ExecuteWorkflowTaskOnWorker already completed a request and this
        // drain runs (e.g. on a logic bug or shutdown-mid-task), the
        // exchange short-circuits the second set_value.
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            while (!m_TaskQueue.empty())
            {
                PythonTask remaining = std::move(m_TaskQueue.front());
                m_TaskQueue.pop();

                if (remaining.m_Type == PythonTask::Type::WorkflowTask && remaining.m_WorkflowRequest)
                {
                    bool expected = false;
                    if (remaining.m_WorkflowRequest->m_PromiseSatisfied.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel))
                    {
                        remaining.m_WorkflowRequest->m_ErrorMessage = "PythonEngine shutting down";
                        remaining.m_WorkflowRequest->m_Success = false;
                        remaining.m_WorkflowRequest->m_Promise.set_value(false);
                    }
                }
            }
        }

        // Clean up Python references under the GIL
        PyEval_RestoreThread(threadState);

        Py_XDECREF(m_OnStartFunc);
        Py_XDECREF(m_OnUpdateFunc);
        Py_XDECREF(m_OnEventFunc);
        Py_XDECREF(m_OnShutdownFunc);
        Py_XDECREF(m_MainModule);

        m_OnStartFunc = nullptr;
        m_OnUpdateFunc = nullptr;
        m_OnEventFunc = nullptr;
        m_OnShutdownFunc = nullptr;
        m_MainModule = nullptr;
        m_MainDict = nullptr;

        // Release GIL and delete this thread's state.
        // We do NOT call Py_EndInterpreter here — the sub-interpreter's main-thread
        // state (created during PythonEnginePool::Initialize) is still alive, and
        // Py_EndInterpreter requires it to be the only thread state.  The sub-
        // interpreter will be cleaned up when Py_Finalize runs at process exit.
        PyThreadState_Clear(threadState);
        PyThreadState_DeleteCurrent(); // releases GIL and deletes thread state
    }

    // ============================================================================
    //   Queue depth (for load balancing)
    // ============================================================================
    size_t PythonEngine::GetQueueDepth() const
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        return m_TaskQueue.size();
    }

    // ============================================================================
    //   Enqueue + hook callers
    // ============================================================================
    void PythonEngine::EnqueueTask(PythonTask const& task)
    {
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_TaskQueue.push(task);
        }

        m_QueueCondition.notify_one();
    }

    void PythonEngine::CallHook(PyObject* functionObject, char const* hookName)
    {
        PyObject* result = PyObject_CallObject(functionObject, nullptr);

        if (!result)
        {
            LOG_APP_ERROR("PythonEngine[{}]: exception in hook '{}()'", m_EngineIndex, hookName);
            PyErr_Print();
        }
        else
        {
            Py_DECREF(result);
        }
    }

    void PythonEngine::CallHookWithEvent(PyObject* functionObject, char const* hookName, Event const& event)
    {
        PyObject* eventDict = BuildEventDict(event);

        if (!eventDict)
        {
            LOG_APP_ERROR("PythonEngine[{}]: failed to build event dictionary for '{}'", m_EngineIndex, hookName);
            return;
        }

        PyObject* args = PyTuple_New(1);
        PyTuple_SetItem(args, 0, eventDict); // steals reference

        PyObject* result = PyObject_CallObject(functionObject, args);

        Py_DECREF(args);

        if (!result)
        {
            LOG_APP_ERROR("PythonEngine[{}]: exception in hook '{}(event)'", m_EngineIndex, hookName);
            PyErr_Print();
        }
        else
        {
            Py_DECREF(result);
        }
    }

    // ============================================================================
    //   Build Python event dict
    // ============================================================================
    PyObject* PythonEngine::BuildEventDict(Event const& event)
    {
        PyObject* dictionary = PyDict_New();
        if (!dictionary)
        {
            return nullptr;
        }

        PyObject* typeString = PyUnicode_FromString(event.GetName());
        PyDict_SetItemString(dictionary, "type", typeString);
        Py_DECREF(typeString);

        if (auto fileSystemEvent = dynamic_cast<FileSystemEvent const*>(&event))
        {
            PyObject* pathString = PyUnicode_FromString(fileSystemEvent->GetPath().c_str());
            PyDict_SetItemString(dictionary, "path", pathString);
            Py_DECREF(pathString);
        }

        return dictionary;
    }

    // ============================================================================
    //   Public API entry points
    // ============================================================================
    void PythonEngine::OnStart()
    {
        if (!m_Running.load(std::memory_order_acquire))
        {
            return;
        }

        PythonTask task;
        task.m_Type = PythonTask::Type::OnStart;
        EnqueueTask(task);
    }

    void PythonEngine::OnUpdate()
    {
        if (!m_Running.load(std::memory_order_acquire))
        {
            return;
        }

        PythonTask task;
        task.m_Type = PythonTask::Type::OnUpdate;
        EnqueueTask(task);
    }

    void PythonEngine::OnEvent(std::shared_ptr<Event> eventPtr)
    {
        if (!m_Running.load(std::memory_order_acquire))
        {
            return;
        }

        PythonTask task;
        task.m_Type = PythonTask::Type::OnEvent;
        task.m_EventPtr = std::move(eventPtr);
        EnqueueTask(task);
    }

    // ============================================================================
    //   Shutdown
    // ============================================================================
    void PythonEngine::SignalStop()
    {
        if (!m_Running.load(std::memory_order_acquire))
        {
            return;
        }

        // enqueue the Shutdown hook so Python gets a clean callback
        if (m_OnShutdownFunc)
        {
            PythonTask task;
            task.m_Type = PythonTask::Type::Shutdown;
            EnqueueTask(task);
        }

        // tell the worker thread to stop
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_StopRequested.store(true, std::memory_order_release);
        }
        m_QueueCondition.notify_all();
    }

    void PythonEngine::WaitStop()
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }

        m_Running.store(false, std::memory_order_release);
        LOG_APP_INFO("PythonEngine[{}] stopped", m_EngineIndex);
    }

    // ============================================================================
    //   ExecuteWorkflowTask — public API (called from thread pool worker)
    //
    //   Enqueues work onto the PythonEngine worker thread and blocks until
    //   completion.  Each engine has its own sub-interpreter with its own GIL,
    //   so multiple engines can execute Python code truly in parallel.
    // ============================================================================
    bool PythonEngine::ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                           std::string const& workflowId, std::string const& runId,
                                           std::unordered_map<std::string, std::string> const& inputValues,
                                           std::unordered_map<std::string, std::string> const& contextValues,
                                           std::unordered_map<std::string, std::string>& outputValuesOut,
                                           std::string& errorMessage, std::string& capturedStdout,
                                           std::string& capturedStderr)
    {
        outputValuesOut.clear();

        if (!m_Running.load(std::memory_order_acquire))
        {
            errorMessage = "PythonEngine::ExecuteWorkflowTask: Python engine is not running";
            LOG_APP_ERROR("PythonEngine[{}]::ExecuteWorkflowTask: engine not running run='{}' workflow='{}' task='{}'",
                          m_EngineIndex, runId, workflowId, taskDefinition.m_Id);
            return false;
        }

        if (taskDefinition.m_ParamsJson.empty())
        {
            errorMessage = "PythonEngine::ExecuteWorkflowTask: task params are missing (expected JSON with module/function)";
            LOG_APP_ERROR("PythonEngine[{}]::ExecuteWorkflowTask: missing params JSON run='{}' workflow='{}' task='{}'",
                          m_EngineIndex, runId, workflowId, taskDefinition.m_Id);
            return false;
        }

        // Build the request and enqueue it onto the worker thread.  Copy the
        // input + context maps into the request so the worker thread holds an
        // owned snapshot — the previous design stored raw `const*` into the
        // caller's stack frame, which is safe only as long as the caller
        // blocks on resultFuture.get() (current behaviour) but breaks
        // catastrophically if a future caller switches to fire-and-forget.
        // Per memory feedback_capture_by_value_async: capture by value into
        // async work sites, no exceptions for "fast paths".
        auto request = std::make_shared<WorkflowTaskRequest>();
        request->m_TaskDefinition = &taskDefinition;
        request->m_TaskWorkingDirectory = taskWorkingDirectory;
        request->m_WorkflowId = workflowId;
        request->m_RunId = runId;
        request->m_InputValues = inputValues;
        request->m_ContextValues = contextValues;

        std::future<bool> resultFuture = request->m_Promise.get_future();

        PythonTask task;
        task.m_Type = PythonTask::Type::WorkflowTask;
        task.m_WorkflowRequest = request;
        EnqueueTask(task);

        // Block until the worker thread completes the Python call.
        bool const ok = resultFuture.get();

        outputValuesOut = std::move(request->m_OutputValues);
        errorMessage = request->m_ErrorMessage;
        capturedStdout = std::move(request->m_CapturedStdout);
        capturedStderr = std::move(request->m_CapturedStderr);
        return ok;
    }

    // ============================================================================
    //   ExecuteWorkflowTaskOnWorker — runs on the PythonEngine worker thread
    //   (GIL is already held by the caller in WorkerLoop)
    // ============================================================================
    void PythonEngine::ExecuteWorkflowTaskOnWorker(std::shared_ptr<WorkflowTaskRequest> const& request)
    {
        TaskDef const& taskDefinition = *request->m_TaskDefinition;
        std::string const& taskWorkingDirectory = request->m_TaskWorkingDirectory;
        std::unordered_map<std::string, std::string> const& inputValues = request->m_InputValues;
        std::unordered_map<std::string, std::string> const& contextValues = request->m_ContextValues;
        std::string const& workflowId = request->m_WorkflowId;
        std::string const& runId = request->m_RunId;

        // NOTE: GIL is already held — we are on the worker thread.

        auto pyObjectToUtf8 = [](PyObject* object) -> std::string
        {
            if (object == nullptr)
            {
                return {};
            }

            PyObject* stringObject = PyObject_Str(object);
            if (stringObject == nullptr)
            {
                return {};
            }

            char const* utf8 = PyUnicode_AsUTF8(stringObject);
            std::string const result = (utf8 != nullptr) ? utf8 : "";

            Py_DECREF(stringObject);
            return result;
        };

        auto consumePythonException = [&]() -> std::string
        {
            PyObject* typeObject = nullptr;
            PyObject* valueObject = nullptr;
            PyObject* traceObject = nullptr;

            PyErr_Fetch(&typeObject, &valueObject, &traceObject);
            PyErr_NormalizeException(&typeObject, &valueObject, &traceObject);

            std::string message;

            if (valueObject != nullptr)
            {
                message = pyObjectToUtf8(valueObject);
            }
            else if (typeObject != nullptr)
            {
                message = pyObjectToUtf8(typeObject);
            }
            else
            {
                message = "unknown python error";
            }

            Py_XDECREF(typeObject);
            Py_XDECREF(valueObject);
            Py_XDECREF(traceObject);

            // Python tracebacks may contain non-UTF-8 bytes (binary literals
            // surfaced via str(), encoding pragmas, mojibake) and unbounded
            // length.  Sanitize + truncate at the boundary so downstream
            // consumers (LOG_APP_ERROR, m_ErrorMessage → dashboard JSON,
            // ncurses TUI) all see well-formed UTF-8 they can render safely.
            return SanitizePythonErrorMessage(message);
        };

        auto fail = [&](std::string const& msg)
        {
            // Atomic guard against a double set_value() race with the
            // shutdown drain in WorkerLoop.  If drain already completed this
            // request (highly unlikely on the synchronous main path, but the
            // guard makes the contract explicit), skip without throwing
            // std::future_error: broken_promise.
            bool expected = false;
            if (!request->m_PromiseSatisfied.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                return;
            }
            request->m_ErrorMessage = msg;
            request->m_Success = false;
            request->m_Promise.set_value(false);
            // Per CLAUDE.md fail-path discipline: every fail-path emits ERROR-level
            // with runId / workflowId / taskId as literal substrings so the
            // dashboard's Run Analyzer can attribute the failure.
            LOG_APP_ERROR("PythonEngine[{}]::ExecuteWorkflowTask: run='{}' workflow='{}' task='{}': {}",
                          m_EngineIndex, runId, workflowId, taskDefinition.m_Id, msg);
        };

        auto succeed = [&]()
        {
            bool expected = false;
            if (!request->m_PromiseSatisfied.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                return;
            }
            request->m_Success = true;
            request->m_Promise.set_value(true);
        };

        // --------------------------------------------------------------------
        // Parse params JSON to get module + function
        // --------------------------------------------------------------------
        std::string moduleName;
        std::string functionName;

        PyObject* jsonModule = PyImport_ImportModule("json");
        if (jsonModule == nullptr)
        {
            return fail("PythonEngine::ExecuteWorkflowTask: failed to import python module 'json': " +
                        consumePythonException());
        }

        PyObject* paramsDict = PyObject_CallMethod(jsonModule, "loads", "s", taskDefinition.m_ParamsJson.c_str());
        Py_DECREF(jsonModule);

        if (paramsDict == nullptr || !PyDict_Check(paramsDict))
        {
            std::string const msg =
                "PythonEngine::ExecuteWorkflowTask: failed to parse task params JSON: " + consumePythonException();
            Py_XDECREF(paramsDict);
            return fail(msg);
        }

        PyObject* moduleObject = PyDict_GetItemString(paramsDict, "module");     // borrowed
        PyObject* functionObject = PyDict_GetItemString(paramsDict, "function"); // borrowed

        if (moduleObject == nullptr || functionObject == nullptr || !PyUnicode_Check(moduleObject) ||
            !PyUnicode_Check(functionObject))
        {
            Py_DECREF(paramsDict);
            return fail(
                "PythonEngine::ExecuteWorkflowTask: task params JSON must contain string fields 'module' and 'function'");
        }

        {
            char const* moduleUtf8 = PyUnicode_AsUTF8(moduleObject);
            char const* functionUtf8 = PyUnicode_AsUTF8(functionObject);

            moduleName = (moduleUtf8 != nullptr) ? moduleUtf8 : "";
            functionName = (functionUtf8 != nullptr) ? functionUtf8 : "";
        }

        Py_DECREF(paramsDict);

        if (moduleName.empty() || functionName.empty())
        {
            return fail("PythonEngine::ExecuteWorkflowTask: empty module/function in task params");
        }

        // --------------------------------------------------------------------
        // Module allowlist — gate PyImport_ImportModule against the script
        // registry so workflow params can't pivot to system modules like
        // 'os' or 'subprocess'.  The registry is populated at startup by
        // ScriptRegistry::ScanDirectory(<scripts root>) and maintained by
        // file-watch events; only files with a properly-formatted header
        // are registered.  Unregistered names are rejected here regardless
        // of whether they would resolve via sys.path.
        //
        // FindByModulePath understands both flat names ("parseLog") and
        // dotted package paths ("scripts.parseLog") — see scriptRegistry.h.
        // --------------------------------------------------------------------
        if (m_ScriptRegistry == nullptr)
        {
            return fail("PythonEngine::ExecuteWorkflowTask: script registry not configured");
        }

        if (m_ScriptRegistry->FindByModulePath(moduleName) == nullptr)
        {
            return fail("PythonEngine::ExecuteWorkflowTask: module '" + moduleName +
                        "' is not in the script registry — only modules under the configured scripts directory may "
                        "be imported");
        }

        // --------------------------------------------------------------------
        // Confine taskWorkingDirectory before it lands in the Python context
        // dict.  An attacker who can influence m_TaskWorkingDirectory could
        // otherwise leak path information or pass an `..`-escaping path that
        // user Python code subsequently uses for file I/O.  Reject anything
        // that doesn't resolve under the project root.
        // --------------------------------------------------------------------
        std::string const taskWorkingDirectoryConfined = ConfineUnderProjectRoot(taskWorkingDirectory).string();
        if (taskWorkingDirectoryConfined.empty())
        {
            return fail("PythonEngine::ExecuteWorkflowTask: taskWorkingDirectory '" + taskWorkingDirectory +
                        "' does not resolve under project root");
        }

        // --------------------------------------------------------------------
        // Import module + look up callable
        // --------------------------------------------------------------------
        // Hot-reload: evict the module (and its parent package for dotted names)
        // from sys.modules so that PyImport_ImportModule re-reads the .py file.
        {
            PyObject* sysModules = PySys_GetObject("modules"); // borrowed ref
            if (sysModules != nullptr && PyDict_Check(sysModules))
            {
                PyObject* key = PyUnicode_FromString(moduleName.c_str());
                if (key != nullptr)
                {
                    if (PyDict_Contains(sysModules, key))
                    {
                        PyDict_DelItem(sysModules, key);
                        LOG_APP_INFO("PythonEngine[{}]: evicted '{}' from sys.modules (hot-reload)", m_EngineIndex,
                                     moduleName);
                    }
                    Py_DECREF(key);
                }

                // For dotted names like "scripts.parseOpenSSHLog", also evict the
                // parent package ("scripts") so its __init__.py is re-evaluated and
                // the submodule binding is refreshed.
                auto dotPos = moduleName.find('.');
                if (dotPos != std::string::npos)
                {
                    std::string const parentPackage = moduleName.substr(0, dotPos);
                    PyObject* parentKey = PyUnicode_FromString(parentPackage.c_str());
                    if (parentKey != nullptr)
                    {
                        if (PyDict_Contains(sysModules, parentKey))
                        {
                            PyDict_DelItem(sysModules, parentKey);
                            LOG_APP_INFO("PythonEngine[{}]: evicted '{}' from sys.modules (hot-reload parent)",
                                         m_EngineIndex, parentPackage);
                        }
                        Py_DECREF(parentKey);
                    }
                }
            }
        }

        PyObject* taskModule = PyImport_ImportModule(moduleName.c_str());
        if (taskModule == nullptr)
        {
            return fail("PythonEngine::ExecuteWorkflowTask: failed to import '" + moduleName +
                        "': " + consumePythonException());
        }

        PyObject* taskFunction = PyObject_GetAttrString(taskModule, functionName.c_str());
        Py_DECREF(taskModule);

        if (taskFunction == nullptr || !PyCallable_Check(taskFunction))
        {
            Py_XDECREF(taskFunction);
            return fail("PythonEngine::ExecuteWorkflowTask: '" + moduleName + "." + functionName + "' is not callable");
        }

        // --------------------------------------------------------------------
        // Build kwargs (inputs...) + context dict
        // --------------------------------------------------------------------
        PyObject* kwargs = PyDict_New();
        if (kwargs == nullptr)
        {
            Py_DECREF(taskFunction);
            return fail("PythonEngine::ExecuteWorkflowTask: failed to allocate kwargs dict");
        }

        for (auto const& inputPair : inputValues)
        {
            PyObject* valueString = PyUnicode_FromString(inputPair.second.c_str());
            if (valueString == nullptr)
            {
                // PyUnicode_FromString sets a Python exception on failure (e.g.
                // input string contains a half-multibyte UTF-8 tail or an
                // embedded NUL). Skipping silently leaks that exception into
                // the next PyObject_Call where it surfaces as the cryptic
                // "<method 'get' of 'dict' objects> returned a result with an
                // exception set" — blaming whichever dict.get the user code
                // hits first. Clear it here so the rest of the call stays sane.
                LOG_APP_ERROR("PythonEngine::ExecuteWorkflowTask: PyUnicode_FromString failed for input '{}' "
                              "task='{}' (value size {} bytes); skipping this input and clearing the leaked "
                              "Python exception. Upstream UTF-8 truncation should have made this impossible "
                              "— investigate if this fires.",
                              inputPair.first, taskDefinition.m_Id, inputPair.second.size());
                PyErr_Clear();
                continue;
            }

            PyDict_SetItemString(kwargs, inputPair.first.c_str(), valueString);
            Py_DECREF(valueString);
        }

        // Always attach the context dict for Python tasks.  Previous behaviour gated
        // this on an explicit "context" key in task inputs, then retried heuristically
        // if the first call failed with "missing ... context".  That breaks when scripts
        // handle None context gracefully and raise a *different* error (e.g. "file not found").
        bool const taskWantsContextInput = true;

        auto attachContextDictToKwargs = [&](PyObject* kwargsDict) -> bool
        {
            PyObject* contextDict = PyDict_New();
            if (contextDict == nullptr)
            {
                return false;
            }

            for (auto const& contextPair : contextValues)
            {
                PyObject* valueString = PyUnicode_FromString(contextPair.second.c_str());
                if (valueString == nullptr)
                {
                    continue;
                }

                PyDict_SetItemString(contextDict, contextPair.first.c_str(), valueString);
                Py_DECREF(valueString);
            }

            // Add a few well-known runtime fields under reserved keys.
            // taskWorkingDirectoryConfined is the canonical project-root-confined
            // path computed above; pass that to Python rather than the raw
            // request value so the gate is consistent end-to-end.
            {
                PyObject* taskIdString = PyUnicode_FromString(taskDefinition.m_Id.c_str());
                PyObject* workingDirectoryString = PyUnicode_FromString(taskWorkingDirectoryConfined.c_str());

                if (taskIdString != nullptr)
                {
                    PyDict_SetItemString(contextDict, "_task_id", taskIdString);
                    Py_DECREF(taskIdString);
                }

                if (workingDirectoryString != nullptr)
                {
                    PyDict_SetItemString(contextDict, "_task_working_directory", workingDirectoryString);
                    Py_DECREF(workingDirectoryString);
                }
            }

            PyDict_SetItemString(kwargsDict, "context", contextDict);
            Py_DECREF(contextDict);

            return true;
        };

        if (taskWantsContextInput)
        {
            if (!attachContextDictToKwargs(kwargs))
            {
                Py_DECREF(kwargs);
                Py_DECREF(taskFunction);
                return fail("PythonEngine::ExecuteWorkflowTask: failed to allocate context dict");
            }
        }

        // --------------------------------------------------------------------
        // Set up stdout/stderr tee capture (real-time + buffer)
        // --------------------------------------------------------------------
        PyRun_SimpleString("import io as _jarvis_io\n"
                           "_jarvis_old_stdout = sys.stdout\n"
                           "_jarvis_old_stderr = sys.stderr\n"
                           "_jarvis_cap_stdout = _jarvis_io.StringIO()\n"
                           "_jarvis_cap_stderr = _jarvis_io.StringIO()\n"
                           "class _JarvisTee:\n"
                           "    def __init__(self, orig, cap):\n"
                           "        self._orig = orig\n"
                           "        self._cap = cap\n"
                           "    def write(self, msg):\n"
                           "        self._orig.write(msg)\n"
                           "        self._cap.write(msg)\n"
                           "    def flush(self):\n"
                           "        self._orig.flush()\n"
                           "        self._cap.flush()\n"
                           "sys.stdout = _JarvisTee(_jarvis_old_stdout, _jarvis_cap_stdout)\n"
                           "sys.stderr = _JarvisTee(_jarvis_old_stderr, _jarvis_cap_stderr)\n");

        // Helper: tear down tee capture and read captured output into request.
        auto teardownCapture = [&]()
        {
            PyRun_SimpleString("sys.stdout = _jarvis_old_stdout\n"
                               "sys.stderr = _jarvis_old_stderr\n");

            PyObject* mainMod = PyImport_AddModule("__main__");                  // borrowed
            PyObject* mainDict2 = mainMod ? PyModule_GetDict(mainMod) : nullptr; // borrowed

            if (mainDict2)
            {
                PyObject* capOut = PyDict_GetItemString(mainDict2, "_jarvis_cap_stdout"); // borrowed
                if (capOut)
                {
                    PyObject* val = PyObject_CallMethod(capOut, "getvalue", nullptr);
                    if (val)
                    {
                        char const* utf8 = PyUnicode_AsUTF8(val);
                        if (utf8)
                            request->m_CapturedStdout = utf8;
                        Py_DECREF(val);
                    }
                }

                PyObject* capErr = PyDict_GetItemString(mainDict2, "_jarvis_cap_stderr"); // borrowed
                if (capErr)
                {
                    PyObject* val = PyObject_CallMethod(capErr, "getvalue", nullptr);
                    if (val)
                    {
                        char const* utf8 = PyUnicode_AsUTF8(val);
                        if (utf8)
                            request->m_CapturedStderr = utf8;
                        Py_DECREF(val);
                    }
                }
            }

            PyRun_SimpleString("del _jarvis_old_stdout, _jarvis_old_stderr, "
                               "_jarvis_cap_stdout, _jarvis_cap_stderr, _JarvisTee, _jarvis_io\n");
        };

        // --------------------------------------------------------------------
        // Execute
        // --------------------------------------------------------------------
        PyObject* argsTuple = PyTuple_New(0);
        if (argsTuple == nullptr)
        {
            teardownCapture();
            Py_DECREF(kwargs);
            Py_DECREF(taskFunction);
            return fail("PythonEngine::ExecuteWorkflowTask: failed to allocate args tuple");
        }

        PyObject* resultObject = PyObject_Call(taskFunction, argsTuple, kwargs);
        Py_DECREF(argsTuple);

        if (resultObject == nullptr && !taskWantsContextInput)
        {
            std::string const firstException = consumePythonException();

            bool const looksLikeMissingContext = (firstException.find("context") != std::string::npos) &&
                                                 ((firstException.find("missing") != std::string::npos) ||
                                                  (firstException.find("required") != std::string::npos));

            if (!looksLikeMissingContext)
            {
                teardownCapture();
                Py_DECREF(kwargs);
                Py_DECREF(taskFunction);
                return fail("PythonEngine::ExecuteWorkflowTask: python exception: " + firstException);
            }

            if (!attachContextDictToKwargs(kwargs))
            {
                teardownCapture();
                Py_DECREF(kwargs);
                Py_DECREF(taskFunction);
                return fail("PythonEngine::ExecuteWorkflowTask: failed to allocate context dict");
            }

            argsTuple = PyTuple_New(0);
            if (argsTuple == nullptr)
            {
                teardownCapture();
                Py_DECREF(kwargs);
                Py_DECREF(taskFunction);
                return fail("PythonEngine::ExecuteWorkflowTask: failed to allocate args tuple");
            }

            resultObject = PyObject_Call(taskFunction, argsTuple, kwargs);
            Py_DECREF(argsTuple);

            if (resultObject == nullptr)
            {
                teardownCapture();
                Py_DECREF(kwargs);
                Py_DECREF(taskFunction);
                return fail("PythonEngine::ExecuteWorkflowTask: python exception: " + consumePythonException());
            }
        }
        else if (resultObject == nullptr)
        {
            teardownCapture();
            Py_DECREF(kwargs);
            Py_DECREF(taskFunction);
            return fail("PythonEngine::ExecuteWorkflowTask: python exception: " + consumePythonException());
        }

        teardownCapture();

        Py_DECREF(kwargs);
        Py_DECREF(taskFunction);

        // A python task may return None; treat it as success with no outputs.
        if (resultObject == Py_None)
        {
            Py_DECREF(resultObject);
            return succeed();
        }

        if (!PyDict_Check(resultObject))
        {
            Py_DECREF(resultObject);
            return fail("PythonEngine::ExecuteWorkflowTask: expected dict return value (or None) from '" + moduleName + "." +
                        functionName + "'");
        }

        // Extract output values as strings.
        PyObject* keyObject = nullptr;
        PyObject* valueObject = nullptr;
        Py_ssize_t position = 0;

        while (PyDict_Next(resultObject, &position, &keyObject, &valueObject))
        {
            if (keyObject == nullptr || !PyUnicode_Check(keyObject))
            {
                continue;
            }

            char const* keyUtf8 = PyUnicode_AsUTF8(keyObject);
            if (keyUtf8 == nullptr)
            {
                continue;
            }

            std::string const key = keyUtf8;
            std::string const value = pyObjectToUtf8(valueObject);

            request->m_OutputValues[key] = value;
        }

        Py_DECREF(resultObject);

        succeed();
    }

} // namespace AIAssistant
