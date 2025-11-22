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

#include <fstream>
#include <filesystem>
#include "simdjson/simdjson.h"

#include "core.h"
#include "engine.h"
#include "jarvisAgent.h"
#include "web/webServer.h"
#include "web/chatMessages.h"

#include "event/events.h"

namespace fs = std::filesystem;
namespace AIAssistant

{
    WebServer::WebServer()
    {
        m_Server.loglevel(crow::LogLevel::Warning);
        RegisterRoutes();
        RegisterWebSocket();
    }

    WebServer::~WebServer() { Stop(); }

    void WebServer::RegisterRoutes()
    {
        // ---- Serve static index page ----
        CROW_ROUTE(m_Server, "/")(
            []()
            {
                std::ifstream file("web/index.html");
                if (!file)
                {
                    return crow::response(404, "index.html not found");
                }

                std::stringstream buffer;
                buffer << file.rdbuf();
                return crow::response(200, buffer.str());
            });

        // ---- POST /api/chat ----
        CROW_ROUTE(m_Server, "/api/chat")
            .methods("POST"_method)([this](const crow::request& req) { return HandleChatPost(req); });

        // ---- GET /api/status ----
        CROW_ROUTE(m_Server, "/api/status")([this]() { return HandleStatusGet(); });
    }

    crow::response WebServer::HandleChatPost(const crow::request& req)
    {
        simdjson::ondemand::parser parser;
        try
        {
            simdjson::padded_string json(req.body);
            auto doc = parser.iterate(json);

            std::string subsystem = std::string(doc["subsystem"].get_string().value());
            std::string message = std::string(doc["message"].get_string().value());

            fs::path queuePath = fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath) / subsystem;

            fs::create_directories(queuePath);

            uint64_t id = App::g_App->GetChatMessagePool()->AddMessage(subsystem, message);
            fs::path filename = queuePath / ("ISSUE_" + std::to_string(id) + ".txt");

            std::ofstream out(filename);
            out << message;
            out.close();

            crow::json::wvalue response;
            response["status"] = "queued";
            response["id"] = id;
            response["file"] = filename.string();
            return crow::response(200, response.dump());
        }
        catch (const std::exception& e)
        {
            crow::json::wvalue error;
            error["error"] = e.what();
            return crow::response(400, error.dump());
        }
    }

    crow::response WebServer::HandleStatusGet()
    {
        crow::json::wvalue status;

        status["type"] = "status";
        status["name"] = "../queue/ICE";
        status["state"] = "SendingQueries";
        status["outputs"] = 4;
        status["inflight"] = 1;
        status["completed"] = 7;
        return crow::response(200, status.dump());
    }

    void WebServer::RegisterWebSocket()
    {
        CROW_WEBSOCKET_ROUTE(m_Server, "/ws")
            .onopen(
                [this](crow::websocket::connection& conn)
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Clients.insert(&conn);
                    LOG_APP_INFO("WebSocket client connected");
                })
            .onclose(
                [this](crow::websocket::connection& conn, const std::string& reason, uint16_t code)
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Clients.erase(&conn);
                    LOG_APP_INFO("WebSocket client disconnected ({}, code {})", reason, code);
                })
            .onmessage(
                [this](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/)
                {
                    try
                    {
                        simdjson::ondemand::parser parser;
                        simdjson::padded_string json(data);
                        auto doc = parser.iterate(json);

                        std::string type = std::string(doc["type"].get_string().value());

                        if (type == "chat")
                        {
                            std::string subsystem = std::string(doc["subsystem"].get_string().value());
                            std::string text = std::string(doc["message"].get_string().value());

                            // add to chat message pool
                            uint64_t id = App::g_App->GetChatMessagePool()->AddMessage(subsystem, text);
                            auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();

                            // determine queue directory
                            fs::path queuePath = fs::path(Core::g_Core->GetConfig().m_QueueFolderFilepath) / subsystem;
                            fs::create_directories(queuePath);

                            // write the PROB_<id>_<timestamp>.txt file
                            fs::path filename =
                                queuePath / ("PROB_" + std::to_string(id) + "_" + std::to_string(timestamp) + ".txt");

                            std::ofstream out(filename);
                            out << text;

                            // respond to browser
                            crow::json::wvalue response;
                            response["type"] = "queued";
                            response["id"] = id; // <-- RETURN UNIQUE ID
                            response["file"] = filename.string();
                            conn.send_text(response.dump());
                        }
                        else if (type == "quit")
                        {
                            auto event = std::make_shared<EngineEvent>(EngineEvent::EngineEventShutdown);
                            Core::g_Core->PushEvent(event);

                            crow::json::wvalue response;
                            response["type"] = "quit-ack";
                            response["message"] = "Shutdown initiated.";
                            conn.send_text(response.dump());

                            return;
                        }

                        else
                        {
                            conn.send_text(R"({"error":"unknown type"})");
                        }
                    }
                    catch (const std::exception& e)
                    {
                        crow::json::wvalue error;
                        error["error"] = e.what();
                        conn.send_text(error.dump());
                    }
                });
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
                LOG_APP_INFO("Crow web server started at http://localhost:8080");
                m_Server.port(8080).multithreaded().signal_clear().run();
            });
    }

    void WebServer::Stop()
    {
        if (!m_Running)
        {
            return;
        }

        m_Running = false;
        m_Server.stop();
        if (m_ServerTask.valid())
        {
            m_ServerTask.wait();
            LOG_APP_INFO("Crow web server stopped");
        }
    }

    void WebServer::Broadcast(const std::string& jsonMessage)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto* client : m_Clients)
        {
            client->send_text(jsonMessage);
        }
    }

    void WebServer::BroadcastJSON(std::string const& jsonString)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto* client : m_Clients)
        {
            client->send_text(jsonString);
        }
    }

} // namespace AIAssistant