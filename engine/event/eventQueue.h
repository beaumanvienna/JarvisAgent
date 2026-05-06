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
#include <memory>
#include <queue>
#include <mutex>
#include <vector>

#include "event/event.h"

namespace AIAssistant
{

    // Thread-safe queue of events for main-thread consumption.
    //
    // Producers (any thread): keyboardInput, fileWatcher, aiRequestPool, pythonEngine, webServer,
    // and Core::CheckSignalFlags push events via Push().  Consumer (main thread only): Core::Run
    // drains the queue once per loop iteration via PopAll().  Push is fully thread-safe; PopAll
    // assumes a single consumer (the main loop).
    class EventQueue
    {
    public:
        using EventPtr = std::shared_ptr<AIAssistant::Event>;

        // Append an event to the back of the queue.  Takes ownership.  Safe to call from any thread.
        void Push(EventPtr event);

        // Drain every queued event into a vector and return it; queue is empty afterwards.
        // Holds the mutex only long enough to swap out the underlying queue (O(1)) — vector
        // construction and event destruction happen outside the lock so producers aren't blocked
        // by main-thread housekeeping.
        std::vector<EventPtr> PopAll();

    private:
        mutable std::mutex m_QueueAccessMutex;
        std::queue<EventPtr> m_Queue;
    };

} // namespace AIAssistant
