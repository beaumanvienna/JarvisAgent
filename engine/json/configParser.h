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
                API3,
                API4,
                Test, // No-network fixture-driven backend for integration tests (§8 Phase 7)
                NumAPIs,
                InvalidAPI
            };

            struct ApiInterface
            {
                std::string m_Name;
                std::string m_Description;
                std::string m_Url;
                std::string m_Model;
                std::string m_KeyName;
                InterfaceType m_InterfaceType{InterfaceType::InvalidAPI};
                // Structure-aware chunking budget (§8 Phase 6). Zero = "no limit known,
                // fall back to a conservative default". Chars ÷ 4 is the rough token
                // estimator for English text.
                uint64_t m_MaxContextTokens{0};
            };

            // Generate a unique interface name from URL domain + model
            static std::string GenerateInterfaceName(std::string const& url, std::string const& model,
                                                     std::string const& apiType);

            size_t m_MaxThreads{0};
            size_t m_MaxInflightAiCalls{1000};
            size_t m_MaxAiCallsPerJcwf{0};  // 0 = no cap; per-run safety limit
            size_t m_PythonEngines{4};
            std::chrono::milliseconds m_SleepDuration{0};
            std::string m_QueueFolderFilepath;
            std::string m_WorkflowsFolderFilepath;
            bool m_Verbose{false};
            size_t m_ApiIndex{0};
            std::vector<ApiInterface> m_ApiInterfaces;
            size_t m_MaxFileSizekB{20};
            size_t m_JcwfBatchSize{10};
            int m_JcwfAiInterfaceIndex{-1}; // -1 = use global default (m_ApiIndex)
            std::string m_KeysFilePath{"keys.json.enc"};
            std::string m_McpKeysFilePath{"mcp_keys.json.enc"};
            int m_SessionTimeoutHours{8};
            std::string m_TlsCert;
            std::string m_TlsKey;
            std::string m_TrustedProxyHeader;
            std::string m_TrustedRoleHeader;
            size_t m_MaxRequestBodyMB{10};
            uint16_t m_Port{0}; // 0 = auto (8080 HTTP, 8443 HTTPS)
            bool m_UseBashOnWindows{false};

            // Determinism defaults.  Per-task settings on AiInvocation override these
            // when non-default.
            double m_DeterminismTemperature{0.0};
            bool m_DeterminismSeedSet{false};
            int64_t m_DeterminismSeed{0};
            bool m_DeterminismRecordSystemFingerprint{true};

            bool m_ConfigValid{false};
            bool m_InterfacesDirty{false};

            bool IsValid() const { return m_ConfigValid; }
        };

    private:
        enum ConfigFields
        {
            Format = 0,
            Description,
            Author,
            QueueFolder,
            WorkflowsFolder,
            MaxThreads,
            SleepTime,
            Verbose,
            Url,
            Model,
            InterfaceType,
            ApiIndex,
            MaxFileSizekB,
            KeysFile,
            InterfaceName,
            InterfaceDescription,
            InterfaceKeyName,
            JcwfBatchSize,
            JcwfAiInterface,
            UseBashOnWindows,
            TlsCert,
            TlsKey,
            TrustedProxyHeader,
            TrustedRoleHeader,
            MaxRequestBodyMB,
            MaxInflightAiCalls,
            MaxAiCallsPerJcwf,
            PythonEngines,
            Port,
            McpKeysFile,
            SessionTimeoutHours,
            NumConfigFields
        };

        using FieldOccurances = std::array<uint32_t, ConfigFields::NumConfigFields>;

        static constexpr std::array<std::string_view, ConfigFields::NumConfigFields> ConfigFieldNames = //
            {
                "Format",               //
                "Description",          //
                "Author",               //
                "QueueFolder",          //
                "WorkflowsFolder",      //
                "MaxThreads",           //
                "SleepTime",            //
                "Verbose",              //
                "Url",                  //
                "Model",                //
                "InterfaceType",        //
                "IndexAPI",             //
                "MaxFileSizekB",        //
                "KeysFile",             //
                "InterfaceName",        //
                "InterfaceDescription", //
                "InterfaceKeyName",     //
                "JcwfBatchSize",        //
                "JcwfAiInterface",      //
                "UseBashOnWindows",     //
                "TlsCert",              //
                "TlsKey",               //
                "TrustedProxyHeader",   //
                "TrustedRoleHeader",    //
                "MaxRequestBodyMB",     //
                "MaxInflightAiCalls",   //
                "MaxAiCallsPerJcwf",    //
                "PythonEngines",        //
                "Port",                 //
                "McpKeysFile",          //
                "SessionTimeoutHours"   //
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
