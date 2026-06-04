/* Copyright (c) 2026 JC Technolabs

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

#include <filesystem>
#include <string>

namespace AIAssistant
{
    // Resolves `path` (absolute or relative) against the project root
    // (fs::current_path()) and verifies, via canonicalization, that it lands
    // inside the root.  Returns the canonical absolute path on success;
    // returns an empty path on rejection (`..` escape, absolute path that
    // lands outside the root, symlink pointing out of tree, or any
    // resolution error).  Fail-closed.
    //
    // Used as the canonical gate for any external string before it becomes
    // a filesystem operation argument: Python sys.path entries, module
    // resolution targets, file deletion paths in CleanWorkflow, etc.
    // Project root is captured from fs::current_path() — j9t launches from
    // the project root so this is the operator-trusted boundary in both
    // Studio and Engine editions.
    std::filesystem::path ConfineUnderProjectRoot(std::string const& path);

    // Convenience overload for callers that already hold an fs::path.
    std::filesystem::path ConfineUnderProjectRoot(std::filesystem::path const& path);
} // namespace AIAssistant
