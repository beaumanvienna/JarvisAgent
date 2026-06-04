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
#include <string>

#include "event/event.h"

namespace AIAssistant
{
    // ─── Engine ↔ Application contract ──────────────────────────────────
    //
    // Abstract base for the single per-process application object owned by
    // the engine.  Implementers (currently `JarvisAgent`) provide the four
    // lifecycle hooks below; the engine drives them in a fixed order.
    //
    // Lifecycle (engine-driven, see `code/backend/engine/core.cpp::Run`):
    //
    //     OnStart()                         // once, blocking, may throw
    //     while (!IsFinished())             // engine main loop
    //     {
    //         for each pending event:
    //             OnEvent(event)            // engine handles first; if not
    //                                       // marked handled, forwarded here
    //         OnUpdate()                    // every tick
    //     }
    //     OnShutdown()                      // once, blocking
    //     // If GetFatalStartupMessage() is non-empty after shutdown,
    //     // the engine prints it on stderr before the process exits.
    //
    // Threading:
    //   • All four hooks run on the engine's main thread, never concurrently
    //     with each other.  Implementations are free to spawn worker threads
    //     internally; they own the synchronisation discipline for shared
    //     state with those threads (see `JarvisAgent`'s threading contract
    //     for the canonical pattern).
    //   • `IsFinished()` is also called from the engine's main thread inside
    //     the loop guard — must be cheap and must not block on subsystem
    //     state that takes the same locks as OnUpdate.
    //
    // Ownership & slicing:
    //   • The engine holds the application via `std::unique_ptr<Application>`
    //     — no copies, no slices.  Copy + move ctor / assignment are
    //     `=delete`-d so a future caller cannot accidentally slice a derived
    //     application down to the abstract base.  The virtual destructor
    //     remains `=default` so `unique_ptr<Application>` can destroy a
    //     derived instance polymorphically.
    //
    // Derived-class responsibilities:
    //   • Either complete OnStart() successfully or set `m_FatalStartupMessage`
    //     to a human-readable explanation before throwing / returning early.
    //     The engine does NOT skip OnShutdown on a fatal-start path — the
    //     derived class must arrange its own state so OnShutdown is safe to
    //     call after a partial OnStart (typically: nullptr-guard each
    //     subsystem reset, see `JarvisAgent::OnShutdown`'s pattern).
    //   • OnEvent's `std::shared_ptr<Event>&` parameter is by non-const ref
    //     for legacy reasons — current overriders do not `.reset()` the
    //     pointer.  Treat the parameter as if it were
    //     `std::shared_ptr<Event> const&` for forward compatibility; a
    //     signature tightening to const ref or to `Event&` is a future
    //     refactor that touches every override.
    class Application
    {
    public:
        Application() = default;
        virtual ~Application() = default;

        Application(Application const&) = delete;
        Application& operator=(Application const&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        virtual void OnStart() = 0;
        virtual void OnUpdate() = 0;
        virtual void OnEvent(std::shared_ptr<Event>&) = 0;
        virtual void OnShutdown() = 0;

        [[nodiscard]] virtual bool IsFinished() const = 0;

        // Returned reference is valid until the next OnStart of the same
        // application instance (i.e. the lifetime of the underlying string
        // member).  Returns an empty string in the common success path.
        [[nodiscard]] std::string const& GetFatalStartupMessage() const { return m_FatalStartupMessage; }

    protected:
        // Set by derived classes on a fatal-start path so the engine can
        // surface the message on stderr after OnShutdown.  Protected (not
        // private with a setter) so derived OnStart implementations can
        // assign directly without an extra method-call layer; the value is
        // only read post-OnShutdown via `GetFatalStartupMessage()` and is
        // single-thread by construction (set inside OnStart, read after
        // OnShutdown — never concurrent with any other access).
        std::string m_FatalStartupMessage;
    };
} // namespace AIAssistant
