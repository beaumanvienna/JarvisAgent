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
#include <mutex>
#include <utility>
#include "BS_thread_pool/BS_thread_pool.hpp"

namespace AIAssistant
{
    class ThreadPool
    {

    public:
        ThreadPool();

        // RequestStop — flip m_Stopped so curl callbacks observe shutdown and
        // abort their transfers.  Does NOT join workers or refuse new
        // submissions on its own (SubmitTask checks m_Stopped and short-
        // circuits, but the pool keeps running queued work).  Pair with
        // Shutdown for full teardown.
        void RequestStop();

        // Shutdown — refuse new submissions (atomically with concurrent
        // SubmitTask via m_Mutex), then drain queued tasks via m_Pool.wait().
        // Idempotent: re-entering after the first call is a no-op.
        void Shutdown();

        // Reset — reconfigure the underlying BS pool to `numThreads`.  Rejects
        // post-Shutdown calls (would leave the wrapper in an inconsistent
        // state where m_Stopped=true but new threads exist).
        void Reset(size_t const numThreads);

        [[nodiscard]] size_t Size() const;
        [[nodiscard]] bool IsStopped() const { return m_Stopped.load(); }

        template <typename FunctionType, typename ReturnType = std::invoke_result_t<std::decay_t<FunctionType>>>
        [[nodiscard]] std::future<ReturnType> SubmitTask(FunctionType&& task)
        {
            std::lock_guard<std::mutex> guard(m_Mutex);
            if (m_Stopped.load())
            {
                // Log line lives in threadPool.cpp to avoid pulling engine.h
                // (and its core.h dependency) into this header — would create
                // a cycle since core.h includes auxiliary/threadPool.h.
                LogPostShutdownSubmit();
                std::promise<ReturnType> p;
                if constexpr (std::is_void_v<ReturnType>)
                {
                    p.set_value();
                }
                else
                {
                    p.set_value(ReturnType{});
                }
                return p.get_future();
            }
            // Forward `task` so an rvalue callable (e.g. a lambda built with
            // std::move'd captures) is moved into the BS pool instead of
            // copied — preserves move-only state in captures.
            return m_Pool.submit_task(std::forward<FunctionType>(task));
        }
        [[nodiscard]] std::vector<std::thread::id> GetThreadIDs() const { return m_Pool.get_thread_ids(); }

    private:
        // Defined in threadPool.cpp — see SubmitTask's call site for why.
        void LogPostShutdownSubmit() const;

        BS::thread_pool<> m_Pool;
        // m_Mutex serialises SubmitTask with itself AND with Shutdown's
        // flag-flip — so a SubmitTask either commits before Shutdown observes
        // m_Stopped=true (and the task runs as part of Shutdown's drain), or
        // sees m_Stopped=true and short-circuits.  No tasks are accepted after
        // Shutdown returns.
        std::mutex m_Mutex;
        // m_Stopped is set by EITHER RequestStop OR Shutdown — both signal
        // "no new tasks, abort in-flight curl".  m_ShutdownDrained tracks
        // whether m_Pool.wait() has already been called by Shutdown so a
        // second Shutdown is a true no-op (don't re-log, don't re-wait).
        // Distinguished from m_Stopped because RequestStop also sets m_Stopped
        // — without a separate flag, Shutdown's idempotency guard would
        // skip the drain entirely after RequestStop.
        std::atomic<bool> m_Stopped{false};
        std::atomic<bool> m_ShutdownDrained{false};
    };
} // namespace AIAssistant
