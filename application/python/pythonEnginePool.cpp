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

#include "file/pathConfinement.h"

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
        if (m_Running.load(std::memory_order_acquire))
        {
            Stop();
        }
    }

    // ============================================================================
    //   Initialize — create N sub-interpreters, each with its own GIL
    // ============================================================================
    bool PythonEnginePool::Initialize(std::string const& scriptPath, size_t engineCount,
                                       ScriptRegistry const* scriptRegistry)
    {
        if (m_Running.load(std::memory_order_acquire))
        {
            return true;
        }

        if (engineCount == 0)
        {
            LOG_APP_ERROR("PythonEnginePool: engineCount must be >= 1");
            return false;
        }

        if (scriptRegistry == nullptr)
        {
            LOG_APP_ERROR("PythonEnginePool: scriptRegistry must be non-null — required for module allowlist gate");
            return false;
        }

        // Resolve script directory + module name.  Confine the resolved script
        // directory under the project root before letting any of it touch
        // Python state — an unconfined scriptPath would let an attacker place
        // a forged module on Python's import path.  Mirrors the gate already
        // applied inside PythonEngine::SetupSubInterpreter (defense in depth
        // at the pool boundary; cyber-sec audit MEDIUM).
        std::string scriptDir;
        std::string moduleName;
        try
        {
            fs::path pythonScriptPath(scriptPath);
            fs::path const parentConfined = ConfineUnderProjectRoot(pythonScriptPath.parent_path());
            if (parentConfined.empty())
            {
                LOG_APP_ERROR("PythonEnginePool: script path '{}' parent does not resolve under project root",
                              scriptPath);
                return false;
            }
            scriptDir = parentConfined.string();
            moduleName = pythonScriptPath.stem().string();
        }
        catch (std::exception const& exception)
        {
            LOG_APP_ERROR("PythonEnginePool: invalid script path '{}': {}", scriptPath, exception.what());
            return false;
        }
        catch (...)
        {
            LOG_APP_ERROR("PythonEnginePool: unknown exception while parsing script path '{}'", scriptPath);
            return false;
        }

        // Python 3.12+ provides Py_NewInterpreterFromConfig (PEP 684) for
        // sub-interpreters with configurable GIL sharing.  Older versions only
        // offer Py_NewInterpreter which creates sub-interpreters that all share
        // the main GIL with no configuration options.  On < 3.12 we clamp to a
        // single engine to avoid the pitfalls of legacy sub-interpreters.
#if PY_VERSION_HEX < 0x030C0000
        if (engineCount > 1)
        {
            LOG_APP_WARN("PythonEnginePool: Python {}.{} detected — sub-interpreter parallelization requires "
                         "Python 3.12+; clamping engine count from {} to 1",
                         PY_MAJOR_VERSION, PY_MINOR_VERSION, engineCount);
            engineCount = 1;
        }
#endif

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

            PyThreadState* subTS = nullptr;

#if PY_VERSION_HEX >= 0x030C0000
            // Python 3.12+: Py_NewInterpreterFromConfig (PEP 684)
            //
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

            PyStatus status = Py_NewInterpreterFromConfig(&subTS, &config);

            if (PyStatus_IsError(status) || subTS == nullptr)
            {
                LOG_APP_ERROR("PythonEnginePool: failed to create sub-interpreter {}: {}", i,
                              status.err_msg ? status.err_msg : "unknown error");
                PyThreadState_Swap(mainTS);
                continue;
            }
#else
            // Python < 3.12: legacy Py_NewInterpreter (shared GIL, no config).
            // engineCount is already clamped to 1 above.
            subTS = Py_NewInterpreter();

            if (subTS == nullptr)
            {
                LOG_APP_ERROR("PythonEnginePool: Py_NewInterpreter() failed for engine {}", i);
                PyThreadState_Swap(mainTS);
                continue;
            }
#endif
            // Sub-interpreter is active on this thread; GIL is held.

            // Wire the borrowed registry pointer before any task executes —
            // module-allowlist gate inside ExecuteWorkflowTaskOnWorker fails
            // closed if the registry isn't set.
            engine->SetScriptRegistry(scriptRegistry);

            // Set up the sub-interpreter (redirect stdout, sys.path, import hooks for primary)
            bool const isPrimary = (i == 0);
            bool setupOk = engine->SetupSubInterpreter(scriptDir, moduleName, isPrimary);

            if (!setupOk)
            {
                // Tear down the sub-interpreter without ever calling
                // SetInterpreterState — leaving the engine with a stale handle
                // would invite later code to act on half-initialised state.
                LOG_APP_ERROR("PythonEnginePool: setup failed for engine {}", i);
                Py_EndInterpreter(subTS);
                PyEval_RestoreThread(mainTS);
                continue;
            }

            // Save the interpreter state — only after a successful setup.
            // The engine is about to be moved into m_Engines; the state pointer
            // is what the worker thread will use to create its own thread state.
            engine->SetInterpreterState(subTS->interp);

            // Switch back to main interpreter for the next iteration.
            // With shared GIL, PyThreadState_Swap is sufficient — there is
            // only one GIL and we already hold it.  The main thread's thread
            // state for this sub-interpreter (subTS) is intentionally kept
            // alive — it will be cleaned up when the process exits.
            PyThreadState_Swap(mainTS);

            // Per the class threading contract, m_Engines mutation goes under
            // m_Mutex.  This is the only writer that runs concurrently with
            // anything (the loop happens at startup; readers come later).
            {
                std::scoped_lock<std::mutex> const lock(m_Mutex);
                m_Engines.push_back(std::move(engine));
            }
        }

        bool enginesEmpty;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            enginesEmpty = m_Engines.empty();
        }
        if (enginesEmpty)
        {
            LOG_APP_ERROR("PythonEnginePool: no engines created successfully");
            PyEval_SaveThread();
            return false;
        }

        // Release main GIL — worker threads will each create their own thread states
        PyEval_SaveThread();

        // Start all worker threads.  After this point m_Engines is read-only
        // for the lifetime of the pool until SignalStop / WaitStop, so the
        // iteration is safe lock-free per the threading contract.
        size_t engineCountForLog;
        {
            std::scoped_lock<std::mutex> const lock(m_Mutex);
            for (auto& engine : m_Engines)
            {
                engine->StartWorkerThread();
            }
            engineCountForLog = m_Engines.size();
        }

        // Release-store ordered with the prior writes — readers that load
        // m_Running with acquire and observe true also observe the fully
        // populated m_Engines.
        m_Running.store(true, std::memory_order_release);
        LOG_APP_INFO("PythonEnginePool: {} engine(s) initialized successfully", engineCountForLog);
        return true;
    }

    // ============================================================================
    //   Load-balanced workflow task dispatch
    // ============================================================================
    PythonEngine* PythonEnginePool::SelectEngine()
    {
        // Lock-free read per the threading contract — only valid when called
        // by a path that has already verified m_Running == true (so Initialize
        // is complete and the vector is stable).  Defensive size check covers
        // the unreachable-but-cheap "called after WaitStop" case.
        if (m_Engines.empty())
        {
            return nullptr;
        }
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
                                               std::string const& workflowId, std::string const& runId,
                                               std::unordered_map<std::string, std::string> const& inputValues,
                                               std::unordered_map<std::string, std::string> const& contextValues,
                                               std::unordered_map<std::string, std::string>& outputValuesOut,
                                               std::string& errorMessage, std::string& capturedStdout,
                                               std::string& capturedStderr)
    {
        if (!m_Running.load(std::memory_order_acquire))
        {
            errorMessage = "PythonEnginePool: not running";
            return false;
        }

        // SelectEngine reads m_Engines lock-free per the threading contract:
        // m_Running is true => Initialize is complete => m_Engines is stable
        // until SignalStop flips m_Running back to false.  ExecuteWorkflowTask
        // callers are workflow worker threads that come and go BEFORE
        // SignalStop is invoked.
        PythonEngine* engine = SelectEngine();
        if (engine == nullptr)
        {
            errorMessage = "PythonEnginePool: no engine available";
            LOG_APP_ERROR("PythonEnginePool::ExecuteWorkflowTask: SelectEngine returned null run='{}' workflow='{}' "
                          "task='{}'", runId, workflowId, taskDefinition.m_Id);
            return false;
        }
        return engine->ExecuteWorkflowTask(taskDefinition, taskWorkingDirectory, workflowId, runId, inputValues,
                                           contextValues, outputValuesOut, errorMessage, capturedStdout, capturedStderr);
    }

    // ============================================================================
    //   Hook callbacks — primary engine (index 0) only
    // ============================================================================
    void PythonEnginePool::OnStart()
    {
        if (!m_Running.load(std::memory_order_acquire) || m_Engines.empty())
        {
            return;
        }
        m_Engines[0]->OnStart();
    }

    void PythonEnginePool::OnUpdate()
    {
        if (!m_Running.load(std::memory_order_acquire) || m_Engines.empty())
        {
            return;
        }
        m_Engines[0]->OnUpdate();
    }

    void PythonEnginePool::OnEvent(std::shared_ptr<Event> eventPtr)
    {
        if (!m_Running.load(std::memory_order_acquire) || m_Engines.empty())
        {
            return;
        }
        m_Engines[0]->OnEvent(std::move(eventPtr));
    }

    size_t PythonEnginePool::GetEngineCount() const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        return m_Engines.size();
    }

    size_t PythonEnginePool::GetTasksCompleted(size_t engineIndex) const
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        if (engineIndex >= m_Engines.size())
        {
            // Was a silent return-zero; surfacing as ERROR keeps stats requests
            // from masking a wiring bug between the dashboard and the pool.
            LOG_APP_ERROR("PythonEnginePool::GetTasksCompleted: engineIndex {} out of bounds (engineCount={})",
                          engineIndex, m_Engines.size());
            return 0;
        }
        return m_Engines[engineIndex]->GetTasksCompleted();
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
        // Flip m_Running first so any in-flight reader noticing the false-load
        // can bail before we start tearing engines down.
        m_Running.store(false, std::memory_order_release);
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        for (auto& engine : m_Engines)
        {
            engine->SignalStop();
        }
    }

    void PythonEnginePool::WaitStop()
    {
        std::scoped_lock<std::mutex> const lock(m_Mutex);
        for (auto& engine : m_Engines)
        {
            engine->WaitStop();
        }

        m_Engines.clear();
        // SignalStop already flipped m_Running.  Repeating the store here is a
        // no-op when SignalStop ran first, and a defense for any caller that
        // calls WaitStop directly.
        m_Running.store(false, std::memory_order_release);

        LOG_APP_INFO("PythonEnginePool: all engines stopped");
    }

} // namespace AIAssistant
