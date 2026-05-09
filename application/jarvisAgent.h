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
#include <chrono>
#include <memory>

#include "application.h"
#include "log/statusRenderer.h"
#include "task/internalTaskRegistry.h"

namespace AIAssistant
{
    class FileWatcher;
    class WebServer;
    class PythonEnginePool;
    class WorkflowRegistry;
    class TriggerEngine;
    class ScriptRegistry;

    class AiRequestPool;
    class CurlMultiDispatcher;
    class WorkflowFileIndex;
    class WorkflowRuntimeManager;
    class SubWorkflowTaskExecutor;

    // ─── Threading & lifetime contract ─────────────────────────────────
    //
    // Construction & ownership
    //   • Single instance per process, owned by the engine via a unique_ptr.
    //   • Default-constructed (cheap), bound to App::g_App in OnStart, and
    //     unbound in OnShutdown — see App::g_App below for the pointer
    //     visibility rules.
    //   • Non-copyable, non-movable: subordinate subsystems hold raw pointers
    //     into JarvisAgent and would break under a move.  Copy + move ctor /
    //     assignment are deleted (the rule-of-five-via-deletion form).
    //
    // Exception safety
    //   • OnStart constructs subsystems via std::make_unique in member-
    //     declaration order.  If a constructor throws, partially-
    //     constructed members unwind via unique_ptr RAII — this is correct
    //     for memory ownership but does NOT call SignalStop / WaitStop on
    //     subsystems that may have started threads before the throw.
    //     Subsystem constructors that start threads SHOULD set their own
    //     "started" flag inside the ctor and call SignalStop+join from
    //     their dtor on partial-init paths; the agent's ctor-throws-mid-
    //     OnStart contract is "the unwinding subsystem dtors clean up
    //     their own threads", not "JarvisAgent re-runs OnShutdown."  Any
    //     new subsystem that violates this must add a JarvisAgent-side
    //     try / catch wrapper around its make_unique and explicitly stop
    //     prior subsystems.
    //
    // Lifecycle
    //   • OnStart RELEASE-stores `this` into App::g_App early (before
    //     subsystem construction) because some subsystem constructors
    //     register callbacks that legitimately read App::g_App during
    //     setup.  Consequence: a background thread that wakes up
    //     mid-OnStart can observe a non-null App::g_App with a still-
    //     null subsystem getter — every Get*() call site must therefore
    //     null-check the returned subsystem pointer separately.  The
    //     release ordering still guarantees that, for any given
    //     subsystem, observers either see nullptr (safe early-return)
    //     or a fully-constructed pointer.
    //   • OnShutdown runs in two phases:
    //       Phase 1 — non-blocking signals (Core::SignalShutdown +
    //                 SignalStop on every subsystem) so I/O loops and
    //                 worker threads see the cancel flag promptly.
    //       Phase 2 — blocking WaitStop + reset on each subsystem in
    //                 reverse-dependency order; App::g_App.store(nullptr,
    //                 release) sits between the curl dispatcher reset and
    //                 the Python engine pool reset, AFTER every subsystem
    //                 that legitimately reads g_App has been signalled but
    //                 BEFORE the Python engine pool (whose user scripts
    //                 might still try to touch the agent on stop) is
    //                 finalised.
    //   • OnShutdown ALSO clears any external lambda capture of `this` that
    //     would otherwise outlive the agent — see the SetStatusCallbacks({},
    //     {}) call at the very top of OnShutdown for the engine-owned TUI
    //     callbacks.  New external [this] captures must be cleared on the
    //     same boundary.
    //
    // Thread-safety of public getters
    //   • All `Get*()` methods return raw pointers to subordinate subsystems.
    //     Callers must hold a happens-before edge to OnStart's release-store
    //     into App::g_App; the standard idiom is to load App::g_App via
    //     acquire and call methods through the loaded pointer.
    //   • Subordinate subsystems are constructed in OnStart and reset in
    //     OnShutdown phase 2 in a fixed order; readers that observe a
    //     non-null pointer must NOT continue to deref past the matching
    //     phase-2 reset (background workers carry their own SignalStop /
    //     WaitStop discipline to enforce this).
    //
    // Lambda capture discipline
    //   • Sync lambdas captured by `[&]` in OnEvent execute inside the
    //     dispatcher's stack frame and are safe by construction.
    //   • Async lambdas with `[this]` are restricted to two sites:
    //       — TerminalManager status callbacks (cleared in OnShutdown)
    //       — TriggerEngine fired-event callback (reset blocks on the
    //         trigger thread before JarvisAgent destruction proceeds)
    //     Both sites carry an explicit lifetime comment at the capture.
    class JarvisAgent : public Application
    {
    public:
        JarvisAgent() = default;
        virtual ~JarvisAgent() = default;

        JarvisAgent(JarvisAgent const&) = delete;
        JarvisAgent& operator=(JarvisAgent const&) = delete;
        JarvisAgent(JarvisAgent&&) = delete;
        JarvisAgent& operator=(JarvisAgent&&) = delete;

        virtual void OnStart() override;
        virtual void OnUpdate() override;
        virtual void OnEvent(std::shared_ptr<Event>&) override;
        virtual void OnShutdown() override;

        virtual bool IsFinished() const override;
        static std::unique_ptr<Application> Create();

        // All getters return raw pointers/refs to subordinate state and do
        // not mutate the agent — marking them const so const-qualified
        // callers (e.g. status snapshot queries) can use them.  The
        // returned subsystem pointers are NOT const-qualified themselves
        // because every legitimate caller needs to invoke mutating methods
        // on the subsystem (Submit, EnqueueWorkflowRun, …).  The const-
        // ness only protects the JarvisAgent's own member layout, not the
        // subsystem behind the pointer.
        WebServer* GetWebServer() const { return m_WebServer.get(); }
        std::chrono::system_clock::time_point GetStartupTime() const { return m_StartupTime; }
        int64_t GetStartupTimestamp() const;
        StatusRenderer& GetStatusRenderer() { return m_StatusRenderer; }
        StatusRenderer const& GetStatusRenderer() const { return m_StatusRenderer; }
        PythonEnginePool* GetPythonEnginePool() const { return m_PythonEnginePool.get(); }
        WorkflowRegistry* GetWorkflowRegistry() const { return m_WorkflowRegistry.get(); }
        ScriptRegistry* GetScriptRegistry() const { return m_ScriptRegistry.get(); }
        WorkflowFileIndex* GetWorkflowFileIndex() const { return m_WorkflowFileIndex.get(); }

        AiRequestPool* GetAiRequestPool() const { return m_AiRequestPool.get(); }
        CurlMultiDispatcher* GetCurlMultiDispatcher() const { return m_CurlMultiDispatcher.get(); }
        WorkflowRuntimeManager* GetWorkflowRuntimeManager() const { return m_WorkflowRuntimeManager.get(); }

        IInternalTaskRegistry* GetInternalTaskRegistry() { return &m_InternalTaskRegistry; }
        IInternalTaskRegistry const* GetInternalTaskRegistry() const { return &m_InternalTaskRegistry; }

    private:
        void CheckIfFinished();
        void InitializeWorkflows();

    private:
        bool m_IsFinished{false};

    private:
        StatusRenderer m_StatusRenderer;
        std::chrono::system_clock::time_point m_StartupTime;

        // submodules
        std::unique_ptr<FileWatcher> m_ScriptFileWatcher;
        std::unique_ptr<ScriptRegistry> m_ScriptRegistry;
        std::unique_ptr<WebServer> m_WebServer;
        std::unique_ptr<PythonEnginePool> m_PythonEnginePool;

        std::unique_ptr<WorkflowRegistry> m_WorkflowRegistry;
        std::unique_ptr<TriggerEngine> m_TriggerEngine;

        InternalTaskRegistry m_InternalTaskRegistry;

        std::unique_ptr<AiRequestPool> m_AiRequestPool;
        std::unique_ptr<CurlMultiDispatcher> m_CurlMultiDispatcher;
        std::unique_ptr<WorkflowRuntimeManager> m_WorkflowRuntimeManager;
        std::unique_ptr<WorkflowFileIndex> m_WorkflowFileIndex;

        std::shared_ptr<SubWorkflowTaskExecutor> m_SubWorkflowExecutor;
    };

    // Process-wide singleton handle for JarvisAgent.  The pointer is set in
    // JarvisAgent::OnStart and cleared in JarvisAgent::OnShutdown after every
    // subordinate subsystem has been signalled, joined, and reset.  Background
    // threads (file watcher, AI request worker pool, etc.) read the pointer
    // throughout the engine's running lifetime, so the storage is
    // `std::atomic<JarvisAgent*>` with acquire/release ordering — a plain
    // pointer would race the read against the OnShutdown nullify in the
    // window between subsystem WaitStop and the final assignment.  Callers
    // load via `App::g_App.load(std::memory_order_acquire)` and null-check
    // before deref.
    class App
    {
    public:
        static std::atomic<JarvisAgent*> g_App;
    };
} // namespace AIAssistant
