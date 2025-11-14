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
#include <iostream>

#include <Python.h>

#include "log/log.h"
#include "event/event.h"
#include "event/filesystemEvent.h"

namespace fs = std::filesystem;

namespace AIAssistant
{

    PythonEngine::PythonEngine() = default;

    PythonEngine::~PythonEngine() { Shutdown(); }

    void PythonEngine::Reset()
    {
        m_Running = false;
        m_ScriptPath.clear();
        m_ScriptDir.clear();
        m_ModuleName.clear();

        if (m_OnStartFunc)
        {
            Py_DECREF(m_OnStartFunc);
            m_OnStartFunc = nullptr;
        }
        if (m_OnUpdateFunc)
        {
            Py_DECREF(m_OnUpdateFunc);
            m_OnUpdateFunc = nullptr;
        }
        if (m_OnEventFunc)
        {
            Py_DECREF(m_OnEventFunc);
            m_OnEventFunc = nullptr;
        }
        if (m_OnShutdownFunc)
        {
            Py_DECREF(m_OnShutdownFunc);
            m_OnShutdownFunc = nullptr;
        }

        if (m_MainModule)
        {
            Py_DECREF(m_MainModule);
            m_MainModule = nullptr;
        }

        m_MainDict = nullptr;
    }

    bool PythonEngine::Initialize(std::string const& scriptPath)
    {
        if (m_Running)
        {
            return true;
        }

        Reset();

        m_ScriptPath = scriptPath;

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

        // Acquire GIL before modifying sys.path
        PyGILState_STATE gilState = PyGILState_Ensure();

        // -----------------------------------------
        // Add script directory to sys.path
        // -----------------------------------------
        PyObject* sysModule = PyImport_ImportModule("sys");
        if (!sysModule)
        {
            PyGILState_Release(gilState);
            PyErr_Print();
            LOG_APP_ERROR("PythonEngine: failed to import 'sys'");
            return false;
        }

        PyObject* sysPathList = PyObject_GetAttrString(sysModule, "path");
        if (!sysPathList || !PyList_Check(sysPathList))
        {
            Py_XDECREF(sysPathList);
            Py_DECREF(sysModule);
            PyGILState_Release(gilState);
            LOG_APP_ERROR("PythonEngine: sys.path is not a list");
            return false;
        }

        PyObject* directoryString = PyUnicode_FromString(m_ScriptDir.c_str());
        if (!directoryString)
        {
            Py_DECREF(sysPathList);
            Py_DECREF(sysModule);
            PyGILState_Release(gilState);
            LOG_APP_ERROR("PythonEngine: failed to build directory string");
            return false;
        }

        if (PyList_Append(sysPathList, directoryString) != 0)
        {
            PyErr_Print();
            LOG_APP_ERROR("PythonEngine: failed to append '{}' to sys.path", m_ScriptDir);
            Py_DECREF(directoryString);
            Py_DECREF(sysPathList);
            Py_DECREF(sysModule);
            PyGILState_Release(gilState);
            return false;
        }

        Py_DECREF(directoryString);
        Py_DECREF(sysPathList);
        Py_DECREF(sysModule);

        // -----------------------------------------
        // Load module
        // -----------------------------------------
        bool moduleLoaded = LoadModule();
        if (!moduleLoaded)
        {
            PyGILState_Release(gilState);
            LOG_APP_ERROR("PythonEngine: failed to load module '{}'", m_ModuleName);
            return false;
        }

        // -----------------------------------------
        // Load hooks
        // -----------------------------------------
        LoadHooks();

        m_Running = true;

        PyGILState_Release(gilState);

        LOG_APP_INFO("PythonEngine initialized successfully");
        return true;
    }

    bool PythonEngine::LoadModule()
    {
        PyObject* moduleNameString = PyUnicode_FromString(m_ModuleName.c_str());
        if (!moduleNameString)
        {
            LOG_APP_ERROR("PythonEngine: failed to create module name '{}'", m_ModuleName);
            return false;
        }

        PyObject* importedModule = PyImport_Import(moduleNameString);
        Py_DECREF(moduleNameString);

        if (!importedModule)
        {
            LOG_APP_ERROR("PythonEngine: PyImport_Import failed for '{}'", m_ModuleName);
            PyErr_Print();
            return false;
        }

        m_MainModule = importedModule;

        m_MainDict = PyModule_GetDict(m_MainModule);
        if (!m_MainDict)
        {
            LOG_APP_ERROR("PythonEngine: failed to get module dict");
            Py_DECREF(m_MainModule);
            m_MainModule = nullptr;
            return false;
        }

        return true;
    }

    void PythonEngine::LoadHooks()
    {
        auto loadHook = [this](char const* hookName, PyObject*& outFunction)
        {
            outFunction = nullptr;

            if (m_MainDict == nullptr)
            {
                return;
            }

            PyObject* object = PyDict_GetItemString(m_MainDict, hookName);
            if (object != nullptr && PyCallable_Check(object))
            {
                Py_INCREF(object);
                outFunction = object;
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
    }

    void PythonEngine::CallHook(PyObject* function, char const* hookName)
    {
        if (!m_Running || function == nullptr)
        {
            return;
        }

        PyGILState_STATE gilState = PyGILState_Ensure();

        PyObject* result = PyObject_CallObject(function, nullptr);
        if (!result)
        {
            LOG_APP_ERROR("PythonEngine: exception in hook '{}()'", hookName);
            PyErr_Print();
        }
        else
        {
            Py_DECREF(result);
        }

        PyGILState_Release(gilState);
    }

    void PythonEngine::CallHookWithEvent(PyObject* function, char const* hookName, Event const& event)
    {
        if (!m_Running || function == nullptr)
        {
            return;
        }

        PyGILState_STATE gilState = PyGILState_Ensure();

        PyObject* eventDictionary = BuildEventDict(event);
        if (!eventDictionary)
        {
            LOG_APP_ERROR("PythonEngine: failed to build event dictionary for '{}'", hookName);
            PyGILState_Release(gilState);
            return;
        }

        PyObject* argumentsTuple = PyTuple_New(1);
        if (!argumentsTuple)
        {
            Py_DECREF(eventDictionary);
            LOG_APP_ERROR("PythonEngine: failed to allocate tuple for '{}'", hookName);
            PyGILState_Release(gilState);
            return;
        }

        PyTuple_SetItem(argumentsTuple, 0, eventDictionary);

        PyObject* result = PyObject_CallObject(function, argumentsTuple);
        Py_DECREF(argumentsTuple);

        if (!result)
        {
            LOG_APP_ERROR("PythonEngine: exception in hook '{}(event)'", hookName);
            PyErr_Print();
        }
        else
        {
            Py_DECREF(result);
        }

        PyGILState_Release(gilState);
    }

    PyObject* PythonEngine::BuildEventDict(Event const& event)
    {
        PyObject* dictionary = PyDict_New();
        if (!dictionary)
        {
            return nullptr;
        }

        PyObject* typeString = PyUnicode_FromString(event.GetName());
        if (!typeString || PyDict_SetItemString(dictionary, "type", typeString) != 0)
        {
            Py_XDECREF(typeString);
            Py_DECREF(dictionary);
            return nullptr;
        }
        Py_DECREF(typeString);

        auto fileSystemEvent = dynamic_cast<AIAssistant::FileSystemEvent const*>(&event);
        if (fileSystemEvent != nullptr)
        {
            PyObject* pathString = PyUnicode_FromString(fileSystemEvent->GetPath().c_str());
            if (!pathString || PyDict_SetItemString(dictionary, "path", pathString) != 0)
            {
                Py_XDECREF(pathString);
                Py_DECREF(dictionary);
                return nullptr;
            }
            Py_DECREF(pathString);
        }

        return dictionary;
    }

    void PythonEngine::OnStart()
    {
        if (!m_Running)
        {
            return;
        }

        if (m_OnStartFunc)
        {
            CallHook(m_OnStartFunc, "OnStart");
        }
    }

    void PythonEngine::OnUpdate()
    {
        if (!m_Running)
        {
            return;
        }

        if (m_OnUpdateFunc)
        {
            CallHook(m_OnUpdateFunc, "OnUpdate");
        }
    }

    void PythonEngine::OnEvent(Event const& event)
    {
        if (!m_Running)
        {
            return;
        }

        if (m_OnEventFunc)
        {
            CallHookWithEvent(m_OnEventFunc, "OnEvent", event);
        }
    }

    void PythonEngine::OnShutdown()
    {
        if (!m_Running)
        {
            return;
        }

        if (m_OnShutdownFunc)
        {
            CallHook(m_OnShutdownFunc, "OnShutdown");
        }
    }

    void PythonEngine::Shutdown()
    {
        if (!m_Running)
        {
            return;
        }

        m_Running = false;

        Reset();

        if (Py_IsInitialized())
        {
            Py_Finalize();
        }

        LOG_APP_INFO("Python engine stopped");
    }

} // namespace AIAssistant
