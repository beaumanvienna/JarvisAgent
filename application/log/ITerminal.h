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

#include <string>
#include <string_view>
#include <cstddef>
#include <memory>

namespace AIAssistant
{
    class ITerminal
    {
    public:
        virtual ~ITerminal() = default;

        // --------------------------------------------------------------------
        // Lifecycle
        // --------------------------------------------------------------------
        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;

        // --------------------------------------------------------------------
        // Rendering
        // --------------------------------------------------------------------
        virtual void Render() = 0;
        virtual void RenderPaused(int counter) = 0;

        // --------------------------------------------------------------------
        // Logging
        // --------------------------------------------------------------------
        virtual void EnqueueLogLine(std::string const& line) = 0;

        // --------------------------------------------------------------------
        // Status Updates (session tracker)
        // --------------------------------------------------------------------
        virtual void UpdateSession(std::string const& name, std::string_view state, size_t outputs, size_t inflight,
                                   size_t completed) = 0;

        // --------------------------------------------------------------------
        // Factory
        // --------------------------------------------------------------------
        static std::unique_ptr<ITerminal> Create();
    };
} // namespace AIAssistant
