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

#include <iostream>
#include <string>

typedef void CURL;
struct curl_slist;

namespace AIAssistant
{
    class Curl
    {
    public:
        struct QueryData
        {
            std::string m_Url;
            std::string m_Data;
            bool IsValid() const;
        };

        // type alias for curl write callback
        using CurlWriteCallback = size_t (*)(void*, size_t, size_t, void*);

        // RAII for curl_slist
        class CurlSlist
        {
        public:
            CurlSlist() = default;
            ~CurlSlist();

            void Append(std::string const& str);
            struct curl_slist* Get();

        private:
            struct curl_slist* m_List{nullptr};
        };

    public:
        Curl();
        ~Curl();

        bool IsInitialized() const;
        bool Query(QueryData const& queryData);
        std::string& GetBuffer();
        void Clear();

    private:
        bool IsValidOpenAIKey(std::string const& key);

    private:
        bool m_Initialized{false};
        CURL* m_Curl{nullptr};
        std::string m_ApiKey;
        std::string m_ReadBuffer;
    };
} // namespace AIAssistant
