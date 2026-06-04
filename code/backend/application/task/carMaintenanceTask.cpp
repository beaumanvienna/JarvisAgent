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

#include "task/carMaintenanceTask.h"

#include <fstream>
#include <system_error>

#include "auxiliary/file.h"
#include <simdjson/simdjson.h>

namespace fs = std::filesystem;

namespace AIAssistant
{
    bool CarMaintenanceTask::Execute(std::vector<fs::path> const& inputFilePaths,
                                     std::vector<fs::path> const& outputFilePaths, std::string& errorMessageOut)
    {
        if (inputFilePaths.empty())
        {
            errorMessageOut = "CarMaintenanceTask: no input files provided.";
            return false;
        }

        if (outputFilePaths.empty())
        {
            errorMessageOut = "CarMaintenanceTask: no output files provided.";
            return false;
        }

        fs::path const& inputFilePath = inputFilePaths[0];
        fs::path const& outputFilePath = outputFilePaths[0];

        std::string inputText;
        if (!TryReadAllText(inputFilePath, inputText, errorMessageOut))
        {
            return false;
        }

        std::string category;
        if (!ExtractCategoryFromStructuredJson(inputText, category, errorMessageOut))
        {
            return false;
        }

        std::string outputText;
        if (category == "engine")
        {
            outputText = MakeEngineManualText();
        }
        else if (category == "tires")
        {
            outputText = MakeTireMaintenanceText();
        }
        else if (category == "rephrase")
        {
            outputText = MakeRephraseRequestText();
        }
        else
        {
            errorMessageOut = "CarMaintenanceTask: unexpected category value '" + category +
                              "' (expected engine | tires | rephrase).";
            return false;
        }

        if (!TryWriteAllText(outputFilePath, outputText, errorMessageOut))
        {
            return false;
        }

        return true;
    }

    bool CarMaintenanceTask::TryReadAllText(fs::path const& filePath, std::string& fileContentsOut,
                                            std::string& errorMessageOut)
    {
        std::error_code fileSizeErrorCode;
        std::uintmax_t const fileSizeBytes = fs::file_size(filePath, fileSizeErrorCode);

        std::ifstream inputStream(filePath, std::ios::binary);
        if (!inputStream.is_open())
        {
            errorMessageOut = "CarMaintenanceTask: failed to open input file: " + filePath.string();
            return false;
        }

        fileContentsOut.clear();
        if (!fileSizeErrorCode)
        {
            if (fileSizeBytes > 0U)
            {
                std::size_t const reserveSize = static_cast<std::size_t>(fileSizeBytes) + 1U;
                fileContentsOut.reserve(reserveSize);
            }
        }

        fileContentsOut.assign(std::istreambuf_iterator<char>(inputStream), std::istreambuf_iterator<char>());

        if (inputStream.bad())
        {
            errorMessageOut = "CarMaintenanceTask: failed while reading input file: " + filePath.string();
            return false;
        }

        return true;
    }

    bool CarMaintenanceTask::TryWriteAllText(fs::path const& filePath, std::string const& fileContents,
                                             std::string& errorMessageOut)
    {
        if (!EngineCore::AtomicWriteFile(filePath, fileContents, errorMessageOut))
        {
            errorMessageOut = "CarMaintenanceTask: " + errorMessageOut;
            return false;
        }
        return true;
    }

    bool CarMaintenanceTask::ExtractCategoryFromStructuredJson(std::string const& fileContents,
                                                               std::string& categoryOut,
                                                               std::string& errorMessageOut)
    {
        categoryOut.clear();

        if (fileContents.empty())
        {
            errorMessageOut = "CarMaintenanceTask: classification file is empty.";
            return false;
        }

        // The classifier task declares output_schema so the runtime writes a validated
        // JSON object of the form {"category": "engine" | "tires" | "rephrase"}.
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(fileContents);

        simdjson::ondemand::document doc;
        if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
        {
            errorMessageOut = "CarMaintenanceTask: failed to parse structured classification JSON.";
            return false;
        }

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS)
        {
            errorMessageOut = "CarMaintenanceTask: classification JSON is not an object.";
            return false;
        }

        simdjson::ondemand::value categoryValue;
        if (obj["category"].get(categoryValue) != simdjson::SUCCESS)
        {
            errorMessageOut = "CarMaintenanceTask: classification JSON missing required field 'category'.";
            return false;
        }

        std::string_view categoryView;
        if (categoryValue.get_string().get(categoryView) != simdjson::SUCCESS)
        {
            errorMessageOut = "CarMaintenanceTask: classification JSON field 'category' is not a string.";
            return false;
        }

        categoryOut.assign(categoryView);
        return true;
    }

    std::string CarMaintenanceTask::MakeEngineManualText()
    {
        return "Engine manual (quick notes):\n"
               "- Check the oil level on a level surface and top up only with the correct oil grade.\n"
               "- If you see an engine warning light, avoid heavy acceleration and schedule a diagnostic scan.\n"
               "- Replace the air filter on schedule; a clogged filter can reduce power and fuel efficiency.\n"
               "- Keep an eye on coolant level and look for leaks around hoses and the radiator.\n";
    }

    std::string CarMaintenanceTask::MakeTireMaintenanceText()
    {
        return "Tire maintenance (quick notes):\n"
               "- Check tire pressure monthly and before long trips; adjust when the tires are cold.\n"
               "- Inspect tread depth and wear patterns; uneven wear often points to alignment issues.\n"
               "- Rotate tires at regular intervals to extend life and keep handling predictable.\n"
               "- Visually inspect sidewalls for cracks, bulges, or embedded debris.\n";
    }

    std::string CarMaintenanceTask::MakeRephraseRequestText()
    {
        return "I could not detect whether your question is about the engine or tires.\n"
               "Please rephrase your car maintenance question and include one of these keywords:\n"
               "- \"engine\" for engine-related help\n"
               "- \"tire\" for tire-related help\n";
    }

} // namespace AIAssistant
