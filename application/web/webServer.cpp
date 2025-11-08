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

#include "core.h"
#include "web/webServer.h"
#include "simdjson/simdjson.h"

using namespace AIAssistant;

WebServer::WebServer() { RegisterRoutes(); }

WebServer::~WebServer() { Stop(); }

void WebServer::RegisterRoutes()
{
    // POST /api/chat
    m_Server.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) { HandleChatPost(req, res); });

    // GET /api/status
    m_Server.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) { HandleStatusGet(req, res); });
}

void WebServer::HandleChatPost(const httplib::Request& req, httplib::Response& res)
{
    simdjson::ondemand::parser parser;
    try
    {
        simdjson::padded_string json(req.body);
        auto doc = parser.iterate(json);

        std::string subsystem = std::string(doc["subsystem"].get_string().value());
        std::string message = std::string(doc["message"].get_string().value());

        fs::path queuePath = Core::g_Core->GetConfig().m_QueueFolderFilepath;

        auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        fs::path filename = queuePath / ("PROB_" + std::to_string(timestamp) + ".txt");

        std::ofstream out(filename);
        out << message;
        out.close();

        std::string jsonResponse = R"({"status":"queued","file":")" + filename.string() + R"("})";
        res.set_content(jsonResponse, "application/json");
    }
    catch (std::exception const& e)
    {
        std::string error = R"({"error":")" + std::string(e.what()) + R"("})";
        res.status = 400;
        res.set_content(error, "application/json");
    }
}

void WebServer::HandleStatusGet(const httplib::Request&, httplib::Response& res)
{
    // minimal stub for now — later this will fetch state from SessionManager
    std::string jsonResponse = R"({
        "state": "AllResponsesReceived",
        "outputs": 4,
        "inflight": 0,
        "completed": 18
    })";
    res.set_content(jsonResponse, "application/json");
}

void WebServer::Start()
{
    if (m_Running)
    {
        return;
    }

    m_Running = true;
    m_ServerTask = Core::g_Core->GetThreadPool().SubmitTask(
        [this]()
        {
            LOG_APP_INFO("Web server started at http://localhost:8080");
            m_Server.listen("0.0.0.0", 8080);
        });
}

void WebServer::Stop()
{
    if (!m_Running)
    {
        return;
    }

    m_Server.stop();
    m_Running = false;
    if (m_ServerTask.valid())
    {
        m_ServerTask.wait(); // wait for graceful exit
        LOG_APP_INFO("Web server stopped");
    }
}
