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

#include <string>
#include <string_view>

namespace AIAssistant
{
    // Typed error returned by workflow JSON parsers (ParseTask, ParseFilter,
    // ParseTaskInputs / ParseTaskOutputs / ParseTaskQueueBinding, plus the
    // RequireObject / RequireArray shape helpers).  Replaces the legacy
    // `bool + std::string& errorMessage` shape, pairing with
    // `std::expected<T, ParserError>` so callers are compiler-forced to
    // handle the rejection path.
    //
    // Subsystem-scoped on purpose: cloud connectors get their own
    // ConnectorError, registry methods get RegistryError, parsers get this.
    // No single mega-enum across the codebase — each subsystem's failure
    // modes are mostly orthogonal and a unified enum would muddy the switch
    // -Wswitch exhaustiveness story.
    enum class ParserErrorCode
    {
        // Expected a JSON object / array at the cursor and got something
        // else (string, number, null).  RequireObject / RequireArray emit
        // this.  Details carries the JSON path context.
        TypeMismatch,

        // Required field absent from the JSON object.  Details carries the
        // field name + surrounding context.
        MissingField,

        // Value is present but out of the documented range (e.g. negative
        // count, enum-style string outside the allowlist).  Details carries
        // the offending value + valid range.
        ValueOutOfRange,

        // Underlying simdjson parse failure (incomplete document, unclosed
        // string, etc.).  Details carries the simdjson error message.
        SimdjsonError,

        // Backstop — see ConnectorErrorCode::UnknownError rationale.
        UnknownError,
    };

    struct ParserError
    {
        ParserErrorCode m_Code{ParserErrorCode::UnknownError};
        std::string m_Details;

        static ParserError Make(ParserErrorCode code, std::string details)
        {
            return ParserError{code, std::move(details)};
        }
    };

    std::string_view Describe(ParserErrorCode code);

} // namespace AIAssistant
