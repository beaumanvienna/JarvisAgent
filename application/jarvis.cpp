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
#include "jarvis.h"
namespace AIAssistant
{
    std::unique_ptr<Application> Jarvis::Create() { return std::make_unique<Jarvis>(); }

    void Jarvis::OnStart() { LOG_APP_INFO("starting Jarvis"); }
    void Jarvis::OnUpdate()
    {
        m_Curl.Clear();
        // retrieve prompt data from queue
        CurlWrapper::QueryData queryData = {
            .m_Url = "https://api.openai.com/v1/chat/completions",
            .m_Data = R"({
            "model": "gpt-4.1",
            "messages": [{"role": "user", "content": "Hello from C++!"}]
        })" //
        };
        m_Curl.Query(queryData);

        // check if app should terminate
        CheckIfFinished();
    }

    void Jarvis::OnEvent() {}

    void Jarvis::OnShutdown() { LOG_APP_INFO("leaving Jarvis"); }

    bool Jarvis::IsFinished() { return m_IsFinished; }

    void Jarvis::CheckIfFinished()
    {
        // not used
        // m_IsFinished = false;
        // Ctrl+C is caught by the engine and breaks the run loop
    }
} // namespace AIAssistant
