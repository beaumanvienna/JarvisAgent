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

#include "curl.h"
#include <curl/curl.h>

namespace AIAssistant
{

    Curl::Curl()
    {
        char* apiKeyEnv = std::getenv("OPENAI_API_KEY");
        if (apiKeyEnv)
        {
            m_ApiKey = std::string(apiKeyEnv);
        }
        if (!IsValidOpenAIKey(m_ApiKey))
        {
            std::cerr << "Missing OPENAI_API_KEY env variable\n";
            return;
        }

        curl_global_init(CURL_GLOBAL_DEFAULT);
        m_Curl = curl_easy_init();

        if (!m_Curl)
        {
            std::cerr << "curl_easy_init()\n";
            return;
        }

        m_Initialized = true;
    }

    Curl::~Curl()
    {
        if (m_Curl)
        {
            curl_easy_cleanup(m_Curl);
        }
        curl_global_cleanup();
    }

    Curl::CurlSlist::~CurlSlist()
    {
        if (m_List)
        {
            curl_slist_free_all(m_List);
        }
    }
    void Curl::CurlSlist::Append(std::string const& str)
    {
        m_List = curl_slist_append(m_List, str.c_str());
        if (!m_List)
        {
            // if curl_slist_append fails it returns nullptr
            std::cerr << "curl_slist_append failed\n";
        }
    }
    struct curl_slist* Curl::CurlSlist::Get() { return m_List; }

    bool Curl::IsInitialized() const { return m_Initialized; }

    std::string& Curl::GetBuffer() { return m_ReadBuffer; }

    void Curl::Clear() { m_ReadBuffer.clear(); }

    bool Curl::IsValidOpenAIKey(std::string const& key)
    {
        return key.size() >= 40 && key.size() <= 60 && key.rfind("sk-", 0) == 0; // starts with "sk-"
    }

    bool Curl::QueryData::IsValid() const
    {
        bool urlEmpty = m_Url.empty();
        bool dataEmpty = m_Data.empty();

        if (urlEmpty)
        {
            std::cerr << "Curl::QueryData::IsValid(): url empty \n";
        }
        if (dataEmpty)
        {
            std::cerr << "Curl::QueryData::IsValid(): data empty \n";
        }

        return !urlEmpty && !dataEmpty;
    }

    bool Curl::Query(QueryData const& queryData)
    {
        if ((!m_Initialized) || (!queryData.IsValid()))
        {
            return false;
        }

        CurlSlist headers;
        headers.Append("Authorization: Bearer " + m_ApiKey);
        headers.Append("Content-Type: application/json");

        auto& url = queryData.m_Url;
        auto& data = queryData.m_Data;

        // lambda for write callback
        auto write_callback = [](void* contents, size_t size, size_t numberOfMembers, void* userPointer) -> size_t
        {
            auto* buffer = reinterpret_cast<std::string*>(userPointer);
            const size_t totalSize = size * numberOfMembers;
            buffer->append(static_cast<char*>(contents), totalSize);
            return totalSize;
        };

        curl_easy_setopt(m_Curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_Curl, CURLOPT_HTTPHEADER, headers.Get());
        curl_easy_setopt(m_Curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(m_Curl, CURLOPT_WRITEFUNCTION, static_cast<CurlWriteCallback>(write_callback));
        curl_easy_setopt(m_Curl, CURLOPT_WRITEDATA, &m_ReadBuffer);
        curl_easy_setopt(m_Curl, CURLOPT_VERBOSE, 1L);

        CURLcode res = curl_easy_perform(m_Curl);

        if (res == CURLE_OK)
        {
            std::cout << "Response:\n" << m_ReadBuffer << std::endl;
        }
        else
        {
            std::cerr << "curl error: " << curl_easy_strerror(res) << std::endl;
        }

        return res == CURLE_OK;
    }
} // namespace AIAssistant