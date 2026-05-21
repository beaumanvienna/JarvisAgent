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
    // Typed error returned by WorkflowRegistry mutation methods
    // (RemoveWorkflow today; subsequent registry mutators will join as they
    // shed the legacy `bool + std::string& errorMessage` shape).  Pairs with
    // `std::expected<T, RegistryError>` so callers are compiler-forced to
    // handle the rejection path.
    enum class RegistryErrorCode
    {
        // workflowId does not name a registered workflow.
        NotFound,

        // File-deletion gate refused to remove a workflow file because its
        // path is outside the project root (defense-in-depth — the file
        // path was canonicalised at insert time, but a future bug that
        // smuggles a non-confined path into the registry can't trigger an
        // arbitrary-file delete via this path).
        PathRefused,

        // std::filesystem operation failed (permission denied, disk full,
        // etc.).  Details carries the system_error message.
        IoError,

        // Backstop.
        UnknownError,
    };

    struct RegistryError
    {
        RegistryErrorCode m_Code{RegistryErrorCode::UnknownError};
        std::string m_Details;

        static RegistryError Make(RegistryErrorCode code, std::string details)
        {
            return RegistryError{code, std::move(details)};
        }
    };

    std::string_view Describe(RegistryErrorCode code);

} // namespace AIAssistant
