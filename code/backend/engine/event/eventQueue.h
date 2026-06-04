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
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <vector>

#include "event/event.h"

namespace AIAssistant
{

    // Tag identifying which subsystem invoked Push.  Used purely for
    // diagnostic instrumentation: when the queue's hard cap fires (main loop
    // wedged), the per-producer breakdown reveals which subsystem was busy
    // while the consumer was stuck.  Add new variants by category, not by
    // call site — sites in the same subsystem share an id.
    enum class ProducerId : uint8_t
    {
        SignalHandler,
        KeyboardInput,
        JarvisAgent,
        AiRequestPool,
        FileWatcher,
        PythonEngine,
        WebServer,
        NumVariants
    };

    constexpr std::string_view ProducerIdToString(ProducerId producer) noexcept
    {
        switch (producer)
        {
            case ProducerId::SignalHandler: return "SignalHandler";
            case ProducerId::KeyboardInput: return "KeyboardInput";
            case ProducerId::JarvisAgent:   return "JarvisAgent";
            case ProducerId::AiRequestPool: return "AiRequestPool";
            case ProducerId::FileWatcher:   return "FileWatcher";
            case ProducerId::PythonEngine:  return "PythonEngine";
            case ProducerId::WebServer:     return "WebServer";
            case ProducerId::NumVariants:   return "<invalid>";
        }
        return "<invalid>";
    }

    // Thread-safe queue of events for main-thread consumption.
    //
    // Producers (any thread): keyboardInput, fileWatcher, aiRequestPool, pythonEngine, webServer,
    // and Core::CheckSignalFlags push events via Push().  Consumer (main thread only): Core::Run
    // drains the queue once per loop iteration via PopAll().  Push is fully thread-safe; PopAll
    // assumes a single consumer (the main loop).
    //
    // Hard cap: a wedged main loop (long-running embedded Python, synchronous
    // curl slip, deadlocked subsystem) would otherwise let producers push
    // indefinitely → OOM.  At kMaxUnprocessedEvents queued, Push performs an
    // emergency exit (LOG_CORE_ERROR with per-producer breakdown +
    // synchronous log flush + std::exit) from the producer thread that
    // detected the cap.  Intentionally non-recoverable — lossy buffering
    // would mask the underlying bug.
    class EventQueue
    {
    public:
        using EventPtr = std::shared_ptr<AIAssistant::Event>;

        // Cap on unprocessed (queued-but-not-yet-drained) events.  Hitting
        // this means the main loop is wedged.  Sized generously above any
        // realistic per-tick burst (file-watcher storm, dispatch completion
        // wave) so the cap signals genuine wedge, not a brief spike.
        static constexpr size_t kMaxUnprocessedEvents = 1000;

        // Append an event to the back of the queue.  Takes ownership.  Safe
        // to call from any thread.  `producer` tags the calling subsystem
        // for the per-producer breakdown emitted on cap-hit.
        void Push(EventPtr event, ProducerId producer);

        // Drain every queued event into a vector and return it; queue is empty afterwards.
        // Holds the mutex only long enough to swap out the underlying queue (O(1)) — vector
        // construction and event destruction happen outside the lock so producers aren't blocked
        // by main-thread housekeeping.  Also resets the per-producer push counters.
        std::vector<EventPtr> PopAll();

    private:
        // Logs the cap-hit breakdown, synchronously flushes log sinks, then
        // std::exit(EXIT_FAILURE).  Called from Push only when the cap is
        // exceeded — does not return.  Lock must NOT be held when this runs
        // (the caller drops the lock before invoking).
        [[noreturn]] void EmergencyExitOnCapExceeded(
            ProducerId triggeringProducer,
            std::array<uint64_t, static_cast<size_t>(ProducerId::NumVariants)> const& breakdown,
            size_t queueDepth);

        mutable std::mutex m_QueueAccessMutex;
        std::queue<EventPtr> m_Queue;
        // Pushes since the last PopAll, per ProducerId.  Reset to zero in
        // PopAll.  Read under m_QueueAccessMutex.  Snapshot is copied out
        // under the lock and passed to EmergencyExitOnCapExceeded.
        std::array<uint64_t, static_cast<size_t>(ProducerId::NumVariants)> m_PushesSinceDrain{};
    };

} // namespace AIAssistant
