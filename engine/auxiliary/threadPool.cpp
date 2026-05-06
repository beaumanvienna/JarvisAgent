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

#include "auxiliary/threadPool.h"
#include "engine.h"

namespace AIAssistant
{
    ThreadPool::ThreadPool() {}

    void ThreadPool::RequestStop()
    {
        m_Stopped.store(true);
        LOG_CORE_INFO("[shutdown] ThreadPool::RequestStop() - stop flag set, curl callbacks will abort");
    }

    void ThreadPool::Shutdown()
    {
        // Take m_Mutex around the m_Stopped flip so any concurrent SubmitTask
        // either commits BEFORE Shutdown observes the stop (and the task runs
        // as part of the drain below) or sees m_Stopped=true and short-
        // circuits.  Idempotency is guarded by m_ShutdownDrained — NOT by
        // m_Stopped — so a prior RequestStop (which also sets m_Stopped) does
        // not cause Shutdown to skip the drain.
        {
            std::lock_guard<std::mutex> guard(m_Mutex);
            if (m_ShutdownDrained.load())
            {
                return;
            }
            m_Stopped.store(true);
        }
        LOG_CORE_INFO("[shutdown] ThreadPool::Shutdown() - refusing new tasks, waiting for {} queued",
                      m_Pool.get_tasks_queued());
        m_Pool.wait();
        m_ShutdownDrained.store(true);
        LOG_CORE_INFO("[shutdown] ThreadPool::Shutdown() complete");
    }

    void ThreadPool::Reset(size_t const numThreads)
    {
        // Reject post-Shutdown calls: the wrapper would otherwise hold
        // m_Stopped=true while m_Pool spawns fresh worker threads, leaving
        // SubmitTask short-circuiting on every call (perpetually-stopped
        // pool + live workers = wasted threads + silently-dropped tasks).
        // Treat as a programming error — log + skip rather than restart.
        if (m_Stopped.load())
        {
            LOG_CORE_WARN("[ThreadPool] Reset({}) called after Shutdown — ignored", numThreads);
            return;
        }
        m_Pool.reset(numThreads);
    }

    [[nodiscard]] size_t ThreadPool::Size() const { return m_Pool.get_thread_count(); }

    void ThreadPool::LogPostShutdownSubmit() const
    {
        LOG_CORE_WARN("[ThreadPool] SubmitTask called after Shutdown - returning default-valued future");
    }

} // namespace AIAssistant
