/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "simdjson/simdjson.h"

namespace AIAssistant
{
    // JSON Schema Draft 2020-12 subset validator, built on simdjson.
    //
    // Supported keywords:
    //   type (string | number | integer | boolean | object | array | null)
    //   properties, required, additionalProperties
    //   items
    //   enum
    //   minimum, maximum, minLength, maxLength, pattern
    //   oneOf, anyOf
    //   $ref, $defs
    //
    // Unsupported keywords (`allOf`, `not`, `format`, ...) are rejected at schema-load time
    // with an explicit error — authors see this before a run starts, never during one.
    struct ValidationError
    {
        std::string m_Path;    // JSON pointer to the failing node
        std::string m_Message;
    };

    struct ValidationResult
    {
        bool m_Ok = false;
        std::vector<ValidationError> m_Errors;
    };

    class SchemaValidator final
    {
    public:
        explicit SchemaValidator(std::string schemaJson);
        ~SchemaValidator();

        SchemaValidator(SchemaValidator const&) = delete;
        SchemaValidator& operator=(SchemaValidator const&) = delete;

        [[nodiscard]] bool IsLoaded() const;
        [[nodiscard]] std::string const& LoadError() const;

        [[nodiscard]] ValidationResult Validate(std::string const& documentJson) const;

        [[nodiscard]] static std::string FormatErrorsForModel(std::vector<ValidationError> const& errors);

    private:
        class Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace AIAssistant
