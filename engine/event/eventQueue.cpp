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

#include "event/eventQueue.h"

#include <cstdlib>
#include <sstream>

#include "core.h"
#include "engine.h"

namespace AIAssistant
{

    void EventQueue::Push(EventPtr event, ProducerId producer)
    {
        bool capHit = false;
        std::array<uint64_t, static_cast<size_t>(ProducerId::NumVariants)> snapshot{};
        size_t queueDepthAtCap = 0;

        {
            std::lock_guard<std::mutex> guard(m_QueueAccessMutex);
            if (m_Queue.size() >= kMaxUnprocessedEvents)
            {
                capHit = true;
                snapshot = m_PushesSinceDrain;
                queueDepthAtCap = m_Queue.size();
            }
            else
            {
                m_Queue.push(std::move(event));
                ++m_PushesSinceDrain[static_cast<size_t>(producer)];
            }
        }

        if (capHit)
        {
            // Lock released; safe to log + flush + exit without holding the mutex.
            EmergencyExitOnCapExceeded(producer, snapshot, queueDepthAtCap);
        }
    }

    std::vector<EventQueue::EventPtr> EventQueue::PopAll()
    {
        std::queue<EventPtr> drained;
        {
            std::lock_guard<std::mutex> guard(m_QueueAccessMutex);
            std::swap(drained, m_Queue);
            m_PushesSinceDrain.fill(0);
        }

        std::vector<EventPtr> eventVector;
        eventVector.reserve(drained.size());
        while (!drained.empty())
        {
            eventVector.push_back(std::move(drained.front()));
            drained.pop();
        }
        return eventVector;
    }

    void EventQueue::EmergencyExitOnCapExceeded(
        ProducerId triggeringProducer,
        std::array<uint64_t, static_cast<size_t>(ProducerId::NumVariants)> const& breakdown,
        size_t queueDepth)
    {
        std::ostringstream breakdownStream;
        constexpr size_t numProducers = static_cast<size_t>(ProducerId::NumVariants);
        bool first = true;
        for (size_t i = 0; i < numProducers; ++i)
        {
            if (!first)
            {
                breakdownStream << ", ";
            }
            breakdownStream << ProducerIdToString(static_cast<ProducerId>(i)) << "=" << breakdown[i];
            first = false;
        }

        LOG_CORE_ERROR(
            "EventQueue::Push: hard cap of {} unprocessed events hit — main loop appears wedged. "
            "Triggering producer: {}. Queue depth: {}. Breakdown since last drain: {{ {} }}. "
            "Emergency exit follows: this bypasses keystore re-seal and audit-log flush that "
            "POST /api/shutdown provides — deliberate trade vs. leaking forever in a wedged process.",
            kMaxUnprocessedEvents, ProducerIdToString(triggeringProducer), queueDepth, breakdownStream.str());

        // Synchronous flush so the ERROR line reaches log/log.txt before the
        // process disappears.  Both loggers flush — the CORE logger carries
        // the ERROR above, the APP logger may carry trailing context from
        // other threads still in flight.
        if (Core::g_Logger)
        {
            Core::g_Logger->GetLogger().flush();
            Core::g_Logger->GetAppLogger().flush();
        }

        std::exit(EXIT_FAILURE);
    }

} // namespace AIAssistant
