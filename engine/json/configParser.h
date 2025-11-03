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
#include "simdjson/simdjson.h"

namespace AIAssistant
{
    class ConfigParser
    {
    public:
        enum State
        {
            Undefined = 0,
            ConfigOk,
            ParseFailure,
            FileNotFound,
            FileFormatFailure
        };

        struct EngineConfig
        {
            enum InterfaceType
            {
                API1 = 0,
                API2,
                NumAPIs,
                InvalidAPI
            };

            struct ApiInterface
            {
                std::string m_Url;
                std::string m_Model;
                InterfaceType m_InterfaceType{InterfaceType::InvalidAPI};
            };

            uint m_MaxThreads{0};
            std::chrono::milliseconds m_SleepDuration{0};
            std::string m_QueueFolderFilepath;
            bool m_Verbose{false};
            size_t m_ApiIndex{0};
            std::vector<ApiInterface> m_ApiInterfaces;
            bool m_ConfigValid{false};

            bool IsValid() const { return m_ConfigValid; }
        };

    private:
        enum ConfigFields
        {
            Format = 0,
            Description,
            Author,
            QueueFolder,
            MaxThreads,
            SleepTime,
            Verbose,
            Url,
            Model,
            InterfaceType,
            ApiIndex,
            NumConfigFields
        };

        using FieldOccurances = std::array<uint32_t, ConfigFields::NumConfigFields>;

        static constexpr std::array<std::string_view, ConfigFields::NumConfigFields> ConfigFieldNames = //
            {
                "Format",        //
                "Description",   //
                "Author",        //
                "QueueFolder",   //
                "MaxThreads",    //
                "SleepTime",     //
                "Verbose",       //
                "Url",           //
                "Model",         //
                "InterfaceType", //
                "IndexAPI"       //
        };

    public:
        ConfigParser(std::string const&);
        ~ConfigParser();

        ConfigParser::State GetState() const;
        ConfigParser::State Parse(EngineConfig&);
        bool ConfigParsed() const;

    private:
        void ParseInterfaces(simdjson::ondemand::array, EngineConfig&, FieldOccurances&);

    private:
        ConfigParser::State m_State;
        std::string m_ConfigFilepathAndFilename;
    };
} // namespace AIAssistant
