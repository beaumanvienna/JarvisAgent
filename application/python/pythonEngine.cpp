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
#include <Python.h>

#include "log/log.h"
#include "event/event.h"
#include "event/filesystemEvent.h"
#include "event/pythonErrorEvent.h"

#include "jarvisAgent.h"

namespace fs = std::filesystem;

extern "C" void JarvisRedirectPython(char const* message)
{
    if (message == nullptr)
    {
        return;
    }

    // Send into std::cout so it flows through TerminalLogStreamBuf
    std::cout << message << std::endl;
}

extern "C" void JarvisPyStatus(char const* message)
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

            // -------------------------------------------------------------
            // Install stdout/stderr redirection *before* importing module
            // -------------------------------------------------------------
            char const* redirectCode = "import sys\n"
                                       "import ctypes\n"
                                       "class _JarvisRedirect:\n"
                                       "    def write(self, msg):\n"
                                       "        try:\n"
                                       "            _C = ctypes.CDLL(None)\n"
                                       "            _C.JarvisRedirectPython(msg.encode('utf-8'))\n"
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
            }

            PyGILState_Release(gilState);
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

        if (m_WorkerThread.joinable())
        {
            m_WorkerThread.join();
        }

        // clean up Python references safely under the GIL
        PyGILState_STATE gilState = PyGILState_Ensure();

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
} // namespace AIAssistant
