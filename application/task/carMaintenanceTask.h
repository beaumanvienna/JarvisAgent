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

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/

#pragma once

#include <string>
#include <filesystem>

#include "task/taskBase.h"

namespace AIAssistant
{
    class CarMaintenanceTask final : public ITask
    {
    public:
        CarMaintenanceTask() = default;
        virtual ~CarMaintenanceTask() = default;

        bool Execute(std::vector<std::filesystem::path> const& inputFilePaths,
                     std::vector<std::filesystem::path> const& outputFilePaths, std::string& errorMessageOut) override;

    private:
        static bool TryReadAllText(std::filesystem::path const& filePath, std::string& fileContentsOut,
                                   std::string& errorMessageOut);
        static bool TryWriteAllText(std::filesystem::path const& filePath, std::string const& fileContents,
                                    std::string& errorMessageOut);

        static bool ExtractCategoryFromStructuredJson(std::string const& fileContents, std::string& categoryOut,
                                                      std::string& errorMessageOut);
        static std::string MakeEngineManualText();
        static std::string MakeTireMaintenanceText();
        static std::string MakeRephraseRequestText();
    };

} // namespace AIAssistant
