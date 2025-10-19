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

#include "simdjson/simdjson.h"

#include "engine.h"
#include "json/configParser.h"
#include "auxiliary/file.h"

namespace AIAssistant
{
    ConfigParser::ConfigParser(std::string const& filepathAndFilename)
        : m_ConfigFilepathAndFilename(filepathAndFilename), m_State{ConfigParser::State::Undefined}
    {
    }

    ConfigParser::~ConfigParser() {}

    ConfigParser::State ConfigParser::GetState() const { return m_State; }

    ConfigParser::State ConfigParser::Parse(EngineConfig& engineConfig)
    {
        m_State = ConfigParser::State::Undefined;
        engineConfig = {}; // reset all fields of engine config

        if ((!EngineCore::FileExists(m_ConfigFilepathAndFilename)) || (EngineCore::IsDirectory(m_ConfigFilepathAndFilename)))
        {
            LOG_CORE_ERROR("file {} not found", m_ConfigFilepathAndFilename);
            m_State = ConfigParser::State::FileNotFound;
            return m_State;
        }
        using namespace simdjson;
        ondemand::parser parser;
        padded_string json = padded_string::load(m_ConfigFilepathAndFilename);

        ondemand::document doc;
        auto error = parser.iterate(json).get(doc);

        if (error)
        {
            std::cerr << "Parse error: " << error_message(error) << std::endl;
            LOG_CORE_ERROR("An error occurred during parsing: {}", error_message(error));
            m_State = ConfigParser::State::ParseFailure;
            return m_State;
        }

        ondemand::document sceneDocument = parser.iterate(json);
        ondemand::object sceneObjects = sceneDocument.get_object();

        std::array<uint32_t, ConfigFields::NumConfigFields> fieldOccurances{};
        for (auto sceneObject : sceneObjects)
        {
            std::string_view sceneObjectKey = sceneObject.unescaped_key();

            if (sceneObjectKey == "file format identifier")
            {
                CORE_ASSERT((sceneObject.value().type() == ondemand::json_type::number), "type must be number");
                ++fieldOccurances[ConfigFields::Format];
            }
            else if (sceneObjectKey == "description")
            {
                CORE_ASSERT((sceneObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view description = sceneObject.value().get_string();
                LOG_CORE_INFO("description: {}", description);
                ++fieldOccurances[ConfigFields::Description];
            }
            else if (sceneObjectKey == "author")
            {
                CORE_ASSERT((sceneObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view author = sceneObject.value().get_string();
                LOG_CORE_INFO("author: {}", author);
                ++fieldOccurances[ConfigFields::Author];
            }
            else if (sceneObjectKey == "queue folder")
            {
                CORE_ASSERT((sceneObject.value().type() == ondemand::json_type::string), "type must be string");
                std::string_view queueFolderFilepath = sceneObject.value().get_string();
                LOG_CORE_INFO("queue folder: {}", queueFolderFilepath);
                engineConfig.m_QueueFolderFilepath = queueFolderFilepath;
                ++fieldOccurances[ConfigFields::QueueFolder];
            }
            else if (sceneObjectKey == "max threads")
            {
                CORE_ASSERT((sceneObject.value().type() == ondemand::json_type::number), "type must be number");
                auto maxThreads = static_cast<int64_t>(sceneObject.value().get_int64());
                LOG_CORE_INFO("max threads: {}", maxThreads);
                engineConfig.m_MaxThreads = maxThreads > 0 ? static_cast<uint32_t>(maxThreads) : 0;
                ++fieldOccurances[ConfigFields::MaxThreads];
            }
        }

        // declare it ok if queue folder filepath was found
        if (fieldOccurances[ConfigFields::QueueFolder] > 0)
        {
            m_State = ConfigParser::State::ConfigOk;
        }
        else
        {
            m_State = ConfigParser::State::FileFormatFailure;
        }

        {
            LOG_CORE_INFO("format info:");
            for (uint32_t index{0}; auto& fieldOccurance : fieldOccurances)
            {
                LOG_CORE_INFO("field: {}, field occurance: {}", ConfigFieldNames[index], fieldOccurance);
                ++index;
            }
        }
        return m_State;
    }

    bool ConfigParser::ConfigParsed() const { return m_State == State::ConfigOk; }
} // namespace AIAssistant
