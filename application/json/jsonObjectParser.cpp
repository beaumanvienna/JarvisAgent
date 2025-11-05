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

#include "json/jsonObjectParser.h"
#include "engine.h"

namespace AIAssistant
{
    JsonObjectParser::JsonObjectParser(std::string_view key, simdjson::ondemand::value value, std::string const& warningText,
                                       uint32_t indentLevel)
        : m_Key{key}, m_Value{value}, m_WarningText{warningText}, m_IndentLevel{indentLevel}
    {
        Parse();
    }

    std::string JsonObjectParser::PrintIndented() const
    {
        std::string str;
        for (uint i = 0; i < m_IndentLevel; ++i)
        {
            str += "  ";
        }
        return str;
    }

    bool JsonObjectParser::HasError() const { return m_HasError; }

    void JsonObjectParser::Parse()
    {
        using namespace simdjson;

        try
        {
            std::string valueString;

            switch (m_Value.type())
            {
                case ondemand::json_type::string:
                {
                    valueString = std::string(m_Value.get_string().value());
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    break;
                }
                case ondemand::json_type::number:
                {
                    valueString = std::to_string(m_Value.get_double().value());
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    break;
                }
                case ondemand::json_type::boolean:
                {
                    valueString = m_Value.get_bool().value() ? "true" : "false";
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    break;
                }
                case ondemand::json_type::null:
                {
                    valueString = "null";
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    break;
                }
                case ondemand::json_type::array:
                {
                    valueString = "[array]";
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    ParseArray(m_Value.get_array().value());
                    break;
                }
                case ondemand::json_type::object:
                {
                    valueString = "{object}";
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    ParseObject(m_Value.get_object().value());
                    break;
                }
                default:
                {
                    valueString = "[unknown type]";
                    LOG_APP_INFO("{}{}: {}", PrintIndented(), m_Key, valueString);
                    break;
                }
            }
        }

        catch (const simdjson::simdjson_error& e)
        {
            LOG_APP_WARN("{}{}: \"{}\" (failed to stringify, error: {})", PrintIndented(), m_WarningText, m_Key, e.what());
            m_HasError = true;
        }
    }

    void JsonObjectParser::ParseObject(simdjson::ondemand::object obj)
    {
        for (auto field : obj)
        {
            auto fieldError = field.error();
            if (fieldError)
            {
                LOG_APP_WARN("{}{}: error while parsing field: {}", PrintIndented(), m_Key,
                             simdjson::error_message(fieldError));
                continue;
            }

            std::string_view key = field.unescaped_key();
            simdjson::ondemand::value val = field.value();

            JsonObjectParser nested(key, val, m_WarningText, m_IndentLevel + 1);
        }
    }

    void JsonObjectParser::ParseArray(simdjson::ondemand::array arr)
    {
        size_t index = 0;
        for (auto elementResult : arr)
        {
            // Unwrap the simdjson_result<ondemand::value> safely
            auto elementError = elementResult.error();
            if (elementError)
            {
                LOG_APP_WARN("{}{}: [index {}] error while parsing array element: {}", PrintIndented(), m_Key, index,
                             simdjson::error_message(elementError));
                ++index;
                continue;
            }

            simdjson::ondemand::value element = elementResult.value();
            std::string key = "[" + std::to_string(index++) + "]";
            JsonObjectParser nested(key, element, m_WarningText, m_IndentLevel + 1);
        }
    }
} // namespace AIAssistant
