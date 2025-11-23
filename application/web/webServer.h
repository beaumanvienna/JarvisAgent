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
#include "crow.h"
#include "auxiliary/threadPool.h"
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <unordered_set>

namespace AIAssistant
{
    class WebServer
    {
    public:
        WebServer();
        ~WebServer();

        void Start();
        void Stop();

        void Broadcast(std::string const& jsonMessage);
        void BroadcastJSON(const std::string& jsonString);
        void BroadcastPythonStatus(bool pythonRunning);

    private:
        void RegisterRoutes();
        void RegisterWebSocket();

        // Handlers
        crow::response HandleChatPost(crow::request const& req);
        crow::response HandleStatusGet();

    private:
        crow::SimpleApp m_Server;
        std::atomic<bool> m_Running{false};
        std::future<void> m_ServerTask;
        std::mutex m_Mutex;

        std::unordered_set<crow::websocket::connection*> m_Clients;
    };
} // namespace AIAssistant
