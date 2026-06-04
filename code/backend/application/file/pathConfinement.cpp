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

#include "file/pathConfinement.h"

namespace AIAssistant
{
    namespace fs = std::filesystem;

    fs::path ConfineUnderProjectRoot(fs::path const& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code ec;
        fs::path const root = fs::weakly_canonical(fs::current_path(ec), ec);
        if (ec || root.empty())
        {
            return {};
        }

        fs::path const candidate = path.is_absolute() ? path : (root / path);
        fs::path const resolved = fs::weakly_canonical(candidate, ec);
        if (ec || resolved.empty())
        {
            return {};
        }

        fs::path const rel = resolved.lexically_relative(root);
        std::string const relStr = rel.string();
        if (relStr.empty() || relStr == ".." || relStr.rfind("..", 0) == 0)
        {
            return {};
        }
        return resolved;
    }

    fs::path ConfineUnderProjectRoot(std::string const& path)
    {
        return ConfineUnderProjectRoot(fs::path(path));
    }
} // namespace AIAssistant
