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

#include "cloud/emailConnector.h"

#include <curl/curl.h>

#include "core.h"
#include "engine.h"
#include "keys/keyManager.h"
#include "curlWrapper/curlWrapper.h"

namespace AIAssistant
{
    std::string EmailConnector::GetType() const
    {
        return "email";
    }

    bool EmailConnector::ResolveCredentials(CloudConnection const& connection, CloudCredentials& credentials,
                                            std::string& errorMessage)
    {
        if (connection.m_KeyName.empty())
        {
            errorMessage = "No credential key specified for Email connection '" + connection.m_Name + "'";
            return false;
        }

        auto const* provider = Core::g_Core->GetKeyManager().GetProvider(connection.m_KeyName);
        if (!provider)
        {
            errorMessage = "Credential '" + connection.m_KeyName + "' not found in KeyManager";
            return false;
        }

        if (provider->m_Username.empty())
        {
            errorMessage = "Credential '" + connection.m_KeyName +
                           "' has no username — Email requires a credentials (username + password) entry";
            return false;
        }

        credentials.m_AuthType = CloudAuthType::BasicAuth;
        credentials.m_Username = provider->m_Username;
        credentials.m_Password = provider->m_Password;
        return true;
    }

    std::string EmailConnector::BuildSmtpUrl(CloudConnection const& connection)
    {
        auto hostIt = connection.m_Params.find("smtp_host");
        std::string host = (hostIt != connection.m_Params.end()) ? hostIt->second : "";

        auto portIt = connection.m_Params.find("smtp_port");
        std::string port = (portIt != connection.m_Params.end() && !portIt->second.empty()) ? portIt->second : "587";

        auto sslIt = connection.m_Params.find("use_ssl");
        bool useSsl = (sslIt == connection.m_Params.end() || sslIt->second != "false");

        std::string scheme = (useSsl && port == "465") ? "smtps" : "smtp";
        return scheme + "://" + host + ":" + port;
    }

    std::string EmailConnector::BuildImapUrl(CloudConnection const& connection)
    {
        auto hostIt = connection.m_Params.find("imap_host");
        std::string host = (hostIt != connection.m_Params.end()) ? hostIt->second : "";

        auto portIt = connection.m_Params.find("imap_port");
        std::string port = (portIt != connection.m_Params.end() && !portIt->second.empty()) ? portIt->second : "993";

        return "imaps://" + host + ":" + port;
    }

    bool EmailConnector::TestConnection(CloudConnection const& connection, std::string& errorMessage)
    {
        auto smtpHostIt = connection.m_Params.find("smtp_host");
        if (smtpHostIt == connection.m_Params.end() || smtpHostIt->second.empty())
        {
            errorMessage = "Email connection requires 'smtp_host' parameter";
            return false;
        }

        CloudCredentials credentials;
        if (!ResolveCredentials(connection, credentials, errorMessage))
        {
            return false;
        }

        // Test SMTP connectivity with EHLO handshake via libcurl
        std::string smtpUrl = BuildSmtpUrl(connection);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            errorMessage = "curl_easy_init() failed";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, smtpUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.m_Username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.m_Password.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        auto const& caBundle = CurlWrapper::GetCaBundlePath();
        if (!caBundle.empty())
        {
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        }

        // Use STARTTLS for port 587
        auto portIt = connection.m_Params.find("smtp_port");
        std::string port = (portIt != connection.m_Params.end() && !portIt->second.empty()) ? portIt->second : "587";
        if (port == "587")
        {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        }

        // CONNECT_ONLY: just establish connection and authenticate, don't send mail
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            errorMessage = std::string("Email SMTP test failed: ") + curl_easy_strerror(res);
            return false;
        }

        return true;
    }
} // namespace AIAssistant
