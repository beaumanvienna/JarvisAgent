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
#include "pythonEnginePool.h"
#include "pythonEngine.h"

#include <filesystem>

// Wrap Python.h for Debug builds (see pythonEngine.cpp for rationale)
#if defined(_WIN32) && defined(_DEBUG)
#undef _DEBUG
#include <Python.h>
#define _DEBUG
#else
#include <Python.h>
#endif

#include "log/log.h"

namespace fs = std::filesystem;

namespace AIAssistant
{
    PythonEnginePool::PythonEnginePool() = default;

    PythonEnginePool::~PythonEnginePool()
    {
        if (m_Running)
        {
            Stop();
        }
    }

    // ============================================================================
    //   Initialize — create N sub-interpreters, each with its own GIL
    // ============================================================================
    bool PythonEnginePool::Initialize(std::string const& scriptPath, size_t engineCount)
    {
        if (m_Running)
        {
            return true;
        }

        if (engineCount == 0)
        {
            LOG_APP_ERROR("PythonEnginePool: engineCount must be >= 1");
            return false;
        }

        // Resolve script directory + module name
        std::string scriptDir;
        std::string moduleName;
        try
        {
            fs::path pythonScriptPath(scriptPath);
            scriptDir = pythonScriptPath.parent_path().string();
            moduleName = pythonScriptPath.stem().string();
        }
        catch (std::exception const& exception)
        {
            LOG_APP_ERROR("PythonEnginePool: invalid script path '{}': {}", scriptPath, exception.what());
            return false;
        }

        LOG_APP_INFO("PythonEnginePool: initializing {} engine(s) with script '{}'", engineCount, scriptPath);

        // ---- Global Python initialization (once per process) ----
        Py_Initialize();
        if (!Py_IsInitialized())
        {
            LOG_APP_ERROR("PythonEnginePool: Py_Initialize() failed");
            return false;
        }
        m_PythonInitialized = true;

        PyThreadState* mainTS = PyThreadState_Get();

        // ---- Create sub-interpreters ----
        for (size_t i = 0; i < engineCount; ++i)
        {
            auto engine = std::make_unique<PythonEngine>(i);

            // Configure sub-interpreter with its own GIL (PEP 684)
            // Create sub-interpreter with shared GIL and shared obmalloc.
            // use_main_obmalloc=1 is essential: it shares the main allocator
            // so that single-phase C extension modules (builtins like _abc,
            // _signal, etc.) work correctly.
            //
            // check_multi_interp_extensions=0 disables the extension
            // compatibility check so sub-interpreters can coexist with any
            // C extension loaded in the main interpreter.
            //
            // With shared GIL, worker threads still serialize on the GIL but
            // each has its own interpreter state (separate sys.modules,
            // __main__, globals).  I/O-bound Python tasks (file reads,
            // network calls) release the GIL naturally, so multiple engines
            // can interleave execution during those windows.
            //
            // True per-interpreter GIL (PEP 684) requires Python 3.13+ where
            // the obmalloc/extension restrictions are fully relaxed.  When the
            // build machine upgrades, switch to PyInterpreterConfig_OWN_GIL
            // for full parallelism.
            PyInterpreterConfig config;
            config.use_main_obmalloc = 1;
            config.allow_fork = 0;
            config.allow_exec = 0;
            config.allow_threads = 1;
            config.allow_daemon_threads = 0;
            config.check_multi_interp_extensions = 0;
            config.gil = PyInterpreterConfig_SHARED_GIL;

            PyThreadState* subTS = nullptr;
            PyStatus status = Py_NewInterpreterFromConfig(&subTS, &config);

            if (PyStatus_IsError(status) || subTS == nullptr)
            {
                LOG_APP_ERROR("PythonEnginePool: failed to create sub-interpreter {}: {}", i,
                              status.err_msg ? status.err_msg : "unknown error");
                // Switch back to main interpreter for next attempt
                PyThreadState_Swap(mainTS);
                continue;
            }
            // Sub-interpreter is active on this thread; GIL is held.

            // Set up the sub-interpreter (redirect stdout, sys.path, import hooks for primary)
            bool const isPrimary = (i == 0);
            bool setupOk = engine->SetupSubInterpreter(scriptDir, moduleName, isPrimary);

            // Save the interpreter state so the worker thread can create its own thread state
            engine->SetInterpreterState(subTS->interp);

            if (!setupOk)
            {
                LOG_APP_ERROR("PythonEnginePool: setup failed for engine {}", i);
                Py_EndInterpreter(subTS);
                PyEval_RestoreThread(mainTS);
                continue;
            }

            // Switch back to main interpreter for the next iteration.
            // With shared GIL, PyThreadState_Swap is sufficient — there is
            // only one GIL and we already hold it.  The main thread's thread
            // state for this sub-interpreter (subTS) is intentionally kept
            // alive — it will be cleaned up when the process exits.
            PyThreadState_Swap(mainTS);

            m_Engines.push_back(std::move(engine));
        }

        if (m_Engines.empty())
        {
            LOG_APP_ERROR("PythonEnginePool: no engines created successfully");
            PyEval_SaveThread();
            return false;
        }

        // Release main GIL — worker threads will each create their own thread states
        PyEval_SaveThread();

        // Start all worker threads
        for (auto& engine : m_Engines)
        {
            engine->StartWorkerThread();
        }

        m_Running = true;
        LOG_APP_INFO("PythonEnginePool: {} engine(s) initialized successfully", m_Engines.size());
        return true;
    }

    // ============================================================================
    //   Load-balanced workflow task dispatch
    // ============================================================================
    PythonEngine* PythonEnginePool::SelectEngine()
    {
        if (m_Engines.size() == 1)
        {
            return m_Engines[0].get();
        }

        size_t bestIndex = 0;
        size_t bestDepth = m_Engines[0]->GetQueueDepth();

        for (size_t i = 1; i < m_Engines.size(); ++i)
        {
            size_t depth = m_Engines[i]->GetQueueDepth();
            if (depth < bestDepth)
            {
                bestDepth = depth;
                bestIndex = i;
            }
        }

        return m_Engines[bestIndex].get();
    }

    bool PythonEnginePool::ExecuteWorkflowTask(TaskDef const& taskDefinition, std::string const& taskWorkingDirectory,
                                               std::unordered_map<std::string, std::string> const& inputValues,
                                               std::unordered_map<std::string, std::string> const& contextValues,
                                               std::unordered_map<std::string, std::string>& outputValuesOut,
                                               std::string& errorMessage, std::string& capturedStdout,
                                               std::string& capturedStderr)
    {
        if (!m_Running)
        {
            errorMessage = "PythonEnginePool: not running";
            return false;
        }

        PythonEngine* engine = SelectEngine();
        return engine->ExecuteWorkflowTask(taskDefinition, taskWorkingDirectory, inputValues, contextValues, outputValuesOut,
                                           errorMessage, capturedStdout, capturedStderr);
    }

    // ============================================================================
    //   Hook callbacks — primary engine (index 0) only
    // ============================================================================
    void PythonEnginePool::OnStart()
    {
        if (!m_Engines.empty())
        {
            m_Engines[0]->OnStart();
        }
    }

    void PythonEnginePool::OnUpdate()
    {
        if (!m_Engines.empty())
        {
            m_Engines[0]->OnUpdate();
        }
    }

    void PythonEnginePool::OnEvent(std::shared_ptr<Event> eventPtr)
    {
        if (!m_Engines.empty())
        {
            m_Engines[0]->OnEvent(std::move(eventPtr));
        }
    }

    // ============================================================================
    //   Shutdown
    // ============================================================================
    void PythonEnginePool::Stop()
    {
        SignalStop();
        WaitStop();
    }

    void PythonEnginePool::SignalStop()
    {
        for (auto& engine : m_Engines)
        {
            engine->SignalStop();
        }
    }

    void PythonEnginePool::WaitStop()
    {
        for (auto& engine : m_Engines)
        {
            engine->WaitStop();
        }

        m_Engines.clear();
        m_Running = false;

        LOG_APP_INFO("PythonEnginePool: all engines stopped");
    }

} // namespace AIAssistant
