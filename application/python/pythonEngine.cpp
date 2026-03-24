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

#include "jarvisAgent.h"

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

    PythonEngine::PythonEngine() = default;
    PythonEngine::~PythonEngine() {}

    void PythonEngine::Reset()
    {
        m_Running = false;

        m_ScriptPath.clear();
        m_ScriptDir.clear();
        m_ModuleName.clear();

        Py_XDECREF(m_OnStartFunc);
        Py_XDECREF(m_OnUpdateFunc);
        Py_XDECREF(m_OnEventFunc);
        Py_XDECREF(m_OnShutdownFunc);

        Py_XDECREF(m_MainModule);
        m_MainModule = nullptr;

        m_MainDict = nullptr;
    }

    // ============================================================================
    //   Initialize()
    // ============================================================================
    bool PythonEngine::Initialize(std::string const& scriptPath)
    {
        if (m_Running)
        {
            return true;
        }

        Reset();
        m_StopRequested = false;
        m_ScriptPath = scriptPath;

        // Resolve script directory + module name
        try
        {
            fs::path pythonScriptPath(scriptPath);
            m_ScriptDir = pythonScriptPath.parent_path().string();
            m_ModuleName = pythonScriptPath.stem().string();
        }
        catch (std::exception const& exception)
        {
            LOG_APP_ERROR("PythonEngine: invalid script path '{}': {}", scriptPath, exception.what());
            return false;
        }

        LOG_APP_INFO("Initializing PythonEngine with script '{}'", m_ScriptPath);

        Py_Initialize();

        if (!Py_IsInitialized())
        {
            LOG_APP_ERROR("PythonEngine: Py_Initialize() failed");
            return false;
        }

        // IMPORTANT:
        // All interpreter setup must happen BEFORE releasing GIL.
        {
            PyGILState_STATE gilState = PyGILState_Ensure();
            std::lock_guard<std::mutex> interpreterLock(m_InterpreterMutex);

            // -------------------------------------------------------------
            // Install stdout/stderr redirection *before* importing module
            // -------------------------------------------------------------
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
                LOG_APP_ERROR("PythonEngine: failed to install stdout/stderr redirect");
            }

            // -------------------------------------------------------------
            // Add script directory to sys.path
            // -------------------------------------------------------------
            PyObject* sysModule = PyImport_ImportModule("sys");
            if (!sysModule)
            {
                PyErr_Print();
                LOG_APP_ERROR("PythonEngine: failed to import 'sys'");
                PyGILState_Release(gilState);
                return false;
            }

            PyObject* sysPathList = PyObject_GetAttrString(sysModule, "path");
            if (!sysPathList || !PyList_Check(sysPathList))
            {
                Py_XDECREF(sysPathList);
                Py_DECREF(sysModule);
                LOG_APP_ERROR("PythonEngine: sys.path is not a list");
                PyGILState_Release(gilState);
                return false;
            }

            PyObject* directoryString = PyUnicode_FromString(m_ScriptDir.c_str());
            PyList_Append(sysPathList, directoryString);
            Py_DECREF(directoryString);

            // Also add the parent of m_ScriptDir (i.e. the launch CWD) so that
            // dotted package imports like "scripts.parseOpenSSHLog" work.
            // (scripts/ must contain __init__.py to be a package.)
            {
                fs::path const scriptDirAbsolute = fs::absolute(fs::path(m_ScriptDir)).lexically_normal();
                std::string const parentDir = scriptDirAbsolute.parent_path().string();
                if (!parentDir.empty())
                {
                    PyObject* parentDirString = PyUnicode_FromString(parentDir.c_str());
                    PyList_Append(sysPathList, parentDirString);
                    Py_DECREF(parentDirString);
                    LOG_APP_INFO("PythonEngine: added '{}' to sys.path (package imports)", parentDir);
                }
            }

            Py_DECREF(sysPathList);
            Py_DECREF(sysModule);

            // -------------------------------------------------------------
            // Import main Python module
            // -------------------------------------------------------------
            PyObject* moduleNameObj = PyUnicode_FromString(m_ModuleName.c_str());
            if (!moduleNameObj)
            {
                LOG_APP_ERROR("PythonEngine: failed to allocate module name '{}'", m_ModuleName);
                PyGILState_Release(gilState);
                return false;
            }

            m_MainModule = PyImport_Import(moduleNameObj);
            Py_DECREF(moduleNameObj);

            if (!m_MainModule)
            {
                PyErr_Print();
                LOG_APP_ERROR("PythonEngine: failed to import module '{}'", m_ModuleName);
                PyGILState_Release(gilState);
                return false;
            }

            m_MainDict = PyModule_GetDict(m_MainModule);
            if (!m_MainDict)
            {
                LOG_APP_ERROR("PythonEngine: failed to retrieve module dict");
                Py_DECREF(m_MainModule);
                m_MainModule = nullptr;
                PyGILState_Release(gilState);
                return false;
            }

            // -------------------------------------------------------------
            // Load hook functions
            // -------------------------------------------------------------
            auto loadHook = [&](char const* hookName, PyObject*& outFunc)
            {
                outFunc = nullptr;

                PyObject* functionObject = PyDict_GetItemString(m_MainDict, hookName);
                if (functionObject && PyCallable_Check(functionObject))
                {
                    Py_INCREF(functionObject);
                    outFunc = functionObject;
                    LOG_APP_INFO("PythonEngine: found hook '{}()'", hookName);
                }
                else
                {
                    LOG_APP_INFO("PythonEngine: hook '{}()' not defined", hookName);
                }
            };

            loadHook("OnStart", m_OnStartFunc);
            loadHook("OnUpdate", m_OnUpdateFunc);
            loadHook("OnEvent", m_OnEventFunc);
            loadHook("OnShutdown", m_OnShutdownFunc);

            PyGILState_Release(gilState);
        }

        // Release GIL so worker thread can reacquire it
        PyEval_SaveThread();

        m_Running = true;
        StartWorkerThread();

        LOG_APP_INFO("PythonEngine initialized successfully");
        return true;
    }
    // ============================================================================
    //   Worker thread
    // ============================================================================
    void PythonEngine::StartWorkerThread() { m_WorkerThread = std::thread(&PythonEngine::WorkerLoop, this); }

    void PythonEngine::WorkerLoop()
    {
        while (true)
        {
            PythonTask task;

            {
                std::unique_lock<std::mutex> lock(m_QueueMutex);
                m_QueueCondition.wait(lock, [&]() { return m_StopRequested || !m_TaskQueue.empty(); });

                if (m_StopRequested)
                {
                    break;
                }

                task = m_TaskQueue.front();
                m_TaskQueue.pop();
            }

            PyGILState_STATE gilState = PyGILState_Ensure();
            std::lock_guard<std::mutex> interpreterLock(m_InterpreterMutex);

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
                    }
                    break;
                }
            }

            PyGILState_Release(gilState);

            // Check stop request between tasks so we don't block on a backlog
            // of queued OnEvent tasks during shutdown.
            {
                std::unique_lock<std::mutex> lock(m_QueueMutex);
                if (m_StopRequested)
                {
                    break;
                }
            }
        }

        // Drain remaining WorkflowTask requests so callers don't block forever
        // on resultFuture.get() during shutdown.
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            while (!m_TaskQueue.empty())
            {
                PythonTask remaining = std::move(m_TaskQueue.front());
                m_TaskQueue.pop();

                if (remaining.m_Type == PythonTask::Type::WorkflowTask && remaining.m_WorkflowRequest)
                {
                    remaining.m_WorkflowRequest->m_ErrorMessage = "PythonEngine shutting down";
                    remaining.m_WorkflowRequest->m_Success = false;
                    remaining.m_WorkflowRequest->m_Promise.set_value(false);
                }
            }
        }
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
            LOG_APP_ERROR("PythonEngine: exception in hook '{}()'", hookName);
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
            LOG_APP_ERROR("PythonEngine: failed to build event dictionary for '{}'", hookName);
            return;
        }

        PyObject* args = PyTuple_New(1);
        PyTuple_SetItem(args, 0, eventDict); // steals reference

        PyObject* result = PyObject_CallObject(functionObject, args);

        Py_DECREF(args);

        if (!result)
        {
            LOG_APP_ERROR("PythonEngine: exception in hook '{}(event)'", hookName);
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
        if (!m_Running)
        {
            return;
        }

        PythonTask task;
        task.m_Type = PythonTask::Type::OnStart;
        EnqueueTask(task);
    }

    void PythonEngine::OnUpdate()
    {
        if (!m_Running)
        {
            return;
        }

        PythonTask task;
        task.m_Type = PythonTask::Type::OnUpdate;
        EnqueueTask(task);
    }

    void PythonEngine::OnEvent(std::shared_ptr<Event> eventPtr)
    {
        if (!m_Running)
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
    void PythonEngine::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void PythonEngine::SignalStop()
    {
        if (!m_Running)
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
        m_StopRequested = true;
        m_QueueCondition.notify_all();
    }

    void PythonEngine::WaitStop()
    {
        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }

        if (!m_Running)
        {
            return;
        }

        // clean up Python references safely under the GIL
        PyGILState_STATE gilState = PyGILState_Ensure();

        std::lock_guard<std::mutex> interpreterLock(m_InterpreterMutex);

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

        PyGILState_Release(gilState);

        m_Running = false;

        LOG_APP_INFO("Python engine stopped");
    }

    // ============================================================================
    //   ExecuteWorkflowTask — public API (called from thread pool worker)
    //
    //   Enqueues work onto the PythonEngine worker thread and blocks until
    //   completion.  This avoids a GIL deadlock where the thread pool worker
    //   and the PythonEngine worker thread both compete for GIL + m_InterpreterMutex.
    // ============================================================================
    bool PythonEngine::ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                           std::unordered_map<std::string, std::string> const& inputValues,
                                           std::unordered_map<std::string, std::string> const& contextValues,
                                           std::unordered_map<std::string, std::string>& outputValuesOut,
                                           std::string& errorMessage, std::string& capturedStdout,
                                           std::string& capturedStderr)
    {
        outputValuesOut.clear();

        if (!m_Running)
        {
            errorMessage = "PythonEngine::ExecuteWorkflowTask: Python engine is not running";
            return false;
        }

        if (taskDefinition.m_ParamsJson.empty())
        {
            errorMessage = "PythonEngine::ExecuteWorkflowTask: task params are missing (expected JSON with module/function)";
            return false;
        }

        // Build the request and enqueue it onto the worker thread.
        auto request = std::make_shared<WorkflowTaskRequest>();
        request->m_TaskDefinition = &taskDefinition;
        request->m_TaskWorkingDirectory = taskWorkingDirectory;
        request->m_InputValues = &inputValues;
        request->m_ContextValues = &contextValues;

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
        std::unordered_map<std::string, std::string> const& inputValues = *request->m_InputValues;
        std::unordered_map<std::string, std::string> const& contextValues = *request->m_ContextValues;

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

            return message;
        };

        auto fail = [&](std::string const& msg)
        {
            request->m_ErrorMessage = msg;
            request->m_Success = false;
            request->m_Promise.set_value(false);
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
                        LOG_APP_INFO("PythonEngine: evicted '{}' from sys.modules (hot-reload)", moduleName);
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
                            LOG_APP_INFO("PythonEngine: evicted '{}' from sys.modules (hot-reload parent)", parentPackage);
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
            {
                PyObject* taskIdString = PyUnicode_FromString(taskDefinition.m_Id.c_str());
                PyObject* workingDirectoryString = PyUnicode_FromString(taskWorkingDirectory.c_str());

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
            request->m_Success = true;
            request->m_Promise.set_value(true);
            return;
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

        request->m_Success = true;
        request->m_Promise.set_value(true);
    }

} // namespace AIAssistant
