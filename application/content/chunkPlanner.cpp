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

#include "content/chunkPlanner.h"

#include <algorithm>
#include <sstream>

#include "content/markdownSectionSplitter.h"

namespace AIAssistant
{
    namespace
    {
        std::vector<std::string> SplitByParagraphs(std::string const& text)
        {
            std::vector<std::string> paragraphs;
            std::string current;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line))
            {
                current += line + "\n";
                if (line.empty())
                {
                    if (!current.empty())
                    {
                        paragraphs.push_back(std::move(current));
                        current.clear();
                    }
                }
            }
            if (!current.empty())
            {
                paragraphs.push_back(std::move(current));
            }
            if (paragraphs.empty())
            {
                paragraphs.push_back(text);
            }
            return paragraphs;
        }

        std::vector<std::string> SplitBySentences(std::string const& text)
        {
            std::vector<std::string> sentences;
            std::string current;
            for (char const character : text)
            {
                current += character;
                if (character == '.' || character == '!' || character == '?' || character == '\n')
                {
                    if (!current.empty())
                    {
                        sentences.push_back(std::move(current));
                        current.clear();
                    }
                }
            }
            if (!current.empty())
            {
                sentences.push_back(std::move(current));
            }
            if (sentences.empty())
            {
                sentences.push_back(text);
            }
            return sentences;
        }

        void AppendPacked(std::vector<std::string>& chunks, std::vector<std::string> const& pieces, uint64_t budget)
        {
            std::string accumulator;
            for (auto const& piece : pieces)
            {
                uint64_t const pieceTokens = ChunkPlanner::EstimateTokens(piece);
                uint64_t const accumulatorTokens = ChunkPlanner::EstimateTokens(accumulator);
                if (accumulatorTokens > 0 && accumulatorTokens + pieceTokens > budget)
                {
                    chunks.push_back(std::move(accumulator));
                    accumulator.clear();
                }
                if (pieceTokens > budget)
                {
                    if (!accumulator.empty())
                    {
                        chunks.push_back(std::move(accumulator));
                        accumulator.clear();
                    }
                    std::vector<std::string> const paragraphs = SplitByParagraphs(piece);
                    if (paragraphs.size() > 1)
                    {
                        AppendPacked(chunks, paragraphs, budget);
                    }
                    else
                    {
                        std::vector<std::string> const sentences = SplitBySentences(piece);
                        if (sentences.size() > 1)
                        {
                            AppendPacked(chunks, sentences, budget);
                        }
                        else
                        {
                            chunks.push_back(piece);
                        }
                    }
                    continue;
                }
                accumulator += piece;
            }
            if (!accumulator.empty())
            {
                chunks.push_back(std::move(accumulator));
            }
        }
    } // anonymous namespace

    uint64_t ChunkPlanner::EstimateTokens(std::string const& text)
    {
        return (text.size() + 3) / 4;
    }

    std::vector<std::string> ChunkPlanner::Plan(std::string const& body, uint64_t maxContextTokens,
                                                  uint64_t promptOverheadTokens)
    {
        if (body.empty())
        {
            return {};
        }
        if (maxContextTokens == 0)
        {
            return {body};
        }

        uint64_t budget = (maxContextTokens > promptOverheadTokens) ? (maxContextTokens - promptOverheadTokens)
                                                                     : (maxContextTokens / 4);
        if (budget < 128)
        {
            budget = 128;
        }

        uint64_t const totalTokens = EstimateTokens(body);
        if (totalTokens <= budget)
        {
            return {body};
        }

        std::vector<MarkdownSection> const sections = MarkdownSectionSplitter::Split(body, 3);
        std::vector<std::string> sectionBodies;
        sectionBodies.reserve(sections.size());
        for (auto const& section : sections)
        {
            sectionBodies.push_back(section.m_Body);
        }

        std::vector<std::string> chunks;
        AppendPacked(chunks, sectionBodies, budget);
        if (chunks.empty())
        {
            chunks.push_back(body);
        }
        return chunks;
    }
} // namespace AIAssistant
