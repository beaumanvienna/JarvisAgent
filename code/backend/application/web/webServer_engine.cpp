/* Copyright (c) 2026 JC Technolabs

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

// Engine-edition stubs for edition-specific WebServer methods.  The Studio
// equivalents live in `webServer_studio.cpp` and do the real work
// (RegisterAssistantWebSocket + assistant broadcast wiring; ai-* WS dispatch).
// Engine has no assistant surface, so both methods are no-ops.
//
// Compile-excluded from the Studio binary by `removefiles` in premake5.lua;
// the `#ifndef J9T_STUDIO` wrapper is a defence-in-depth backstop in case the
// premake gating is ever bypassed.

#ifndef J9T_STUDIO

#include "web/webServer.h"

namespace AIAssistant
{
    void WebServer::InitEditionSpecific() {}

    bool WebServer::HandleAssistantWebSocketMessage(crow::websocket::connection& /*conn*/,
                                                    simdjson::ondemand::document& /*doc*/,
                                                    std::string_view /*type*/)
    {
        return false;
    }
} // namespace AIAssistant

#endif // !J9T_STUDIO
