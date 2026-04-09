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

#include "simdjson/simdjson.h"

#include <fstream>
#include <sstream>

#include "engine.h"
#include "keys/keyEncryption.h"
#include "keys/keyManager.h"

namespace AIAssistant
{
    bool KeyManager::Load(std::filesystem::path const& keysFilePath, std::string const& masterPassword)
    {
        // Read encrypted file into memory
        std::ifstream file(keysFilePath, std::ios::binary);
        if (!file)
        {
            LOG_CORE_ERROR("KeyManager::Load: cannot open '{}'", keysFilePath.string());
            return false;
        }

        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        if (blob.empty())
        {
            LOG_CORE_ERROR("KeyManager::Load: '{}' is empty", keysFilePath.string());
            return false;
        }

        std::string json = KeyEncryption::Decrypt(blob, masterPassword);
        if (json.empty())
        {
            LOG_CORE_ERROR("KeyManager::Load: decryption failed for '{}'", keysFilePath.string());
            return false;
        }

        if (!ParseProvidersJson(json))
        {
            LOG_CORE_ERROR("KeyManager::Load: failed to parse decrypted JSON from '{}'", keysFilePath.string());
            return false;
        }

        m_Dirty = false;
        LOG_CORE_INFO("KeyManager::Load: loaded {} provider(s) from '{}'", m_Providers.size(), keysFilePath.string());
        return true;
    }

    bool KeyManager::Save(std::filesystem::path const& keysFilePath, std::string const& masterPassword)
    {
        std::string json;
        {
            std::shared_lock lock(m_Mutex);
            json = SerializeToJson();
        }

        std::vector<uint8_t> blob = KeyEncryption::Encrypt(json, masterPassword);
        if (blob.empty())
        {
            LOG_CORE_ERROR("KeyManager::Save: encryption failed");
            return false;
        }

        std::ofstream file(keysFilePath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            LOG_CORE_ERROR("KeyManager::Save: cannot open '{}' for writing", keysFilePath.string());
            return false;
        }

        file.write(reinterpret_cast<char const*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        file.close();

        m_Dirty = false;
        LOG_CORE_INFO("KeyManager::Save: saved {} provider(s) to '{}'", m_Providers.size(), keysFilePath.string());
        return true;
    }

    bool KeyManager::LoadPlaintext(std::filesystem::path const& keysFilePath)
    {
        std::ifstream file(keysFilePath);
        if (!file)
        {
            LOG_CORE_ERROR("KeyManager::LoadPlaintext: cannot open '{}'", keysFilePath.string());
            return false;
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        file.close();

        std::string json = oss.str();
        if (json.empty())
        {
            LOG_CORE_ERROR("KeyManager::LoadPlaintext: '{}' is empty", keysFilePath.string());
            return false;
        }

        if (!ParseProvidersJson(json))
        {
            LOG_CORE_ERROR("KeyManager::LoadPlaintext: failed to parse JSON from '{}'", keysFilePath.string());
            return false;
        }

        m_Dirty = false;
        LOG_CORE_INFO("KeyManager::LoadPlaintext: loaded {} provider(s) from '{}'", m_Providers.size(),
                      keysFilePath.string());
        return true;
    }

    bool KeyManager::SavePlaintext(std::filesystem::path const& keysFilePath)
    {
        std::string json;
        {
            std::shared_lock lock(m_Mutex);
            json = SerializeToJson();
        }

        std::ofstream file(keysFilePath, std::ios::trunc);
        if (!file)
        {
            LOG_CORE_ERROR("KeyManager::SavePlaintext: cannot open '{}' for writing", keysFilePath.string());
            return false;
        }

        file << json;
        file.close();

        m_Dirty = false;
        LOG_CORE_INFO("KeyManager::SavePlaintext: saved {} provider(s) to '{}'", m_Providers.size(), keysFilePath.string());
        return true;
    }

    bool KeyManager::LoadFromEnvironment(std::string const& endpoint, std::string const& model, std::string const& apiType)
    {
        char const* apiKeyEnv = std::getenv("OPENAI_API_KEY");
        if (!apiKeyEnv || std::strlen(apiKeyEnv) < 8)
        {
            LOG_CORE_WARN("KeyManager::LoadFromEnvironment: OPENAI_API_KEY not set or too short");
            return false;
        }

        std::unique_lock lock(m_Mutex);
        m_Providers.clear();

        ProviderConfig config;
        config.m_DisplayName = "OpenAI (from environment)";
        config.m_Endpoint = endpoint;
        config.m_ApiKey = std::string(apiKeyEnv);
        config.m_DefaultModel = model;
        config.m_ApiType = apiType;

        m_Providers["openai"] = std::move(config);
        m_DefaultProviderName = "openai";

        m_Dirty = false;
        LOG_CORE_INFO("KeyManager::LoadFromEnvironment: created 'openai' provider from OPENAI_API_KEY env var");
        return true;
    }

    bool KeyManager::Unlock(std::string const& masterPassword)
    {
        if (m_KeysFilePath.empty())
        {
            LOG_CORE_ERROR("KeyManager::Unlock: no keys file path set");
            return false;
        }

        if (!std::filesystem::exists(m_KeysFilePath))
        {
            LOG_CORE_ERROR("KeyManager::Unlock: keys file '{}' does not exist", m_KeysFilePath.string());
            m_KeyLoadStatus = KeyLoadStatus::NoKeysFile;
            return false;
        }

        if (Load(m_KeysFilePath, masterPassword))
        {
            m_KeyLoadStatus = KeyLoadStatus::Ok;
            LOG_CORE_INFO("KeyManager::Unlock: successfully unlocked with runtime password");
            return true;
        }

        m_KeyLoadStatus = KeyLoadStatus::WrongPassword;
        return false;
    }

    KeyManager::ProviderConfig const* KeyManager::GetProvider(std::string const& name) const
    {
        std::shared_lock lock(m_Mutex);
        auto const it = m_Providers.find(name);
        if (it == m_Providers.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    KeyManager::ProviderConfig const* KeyManager::GetDefaultProvider() const
    {
        std::shared_lock lock(m_Mutex);
        if (m_DefaultProviderName.empty())
        {
            return nullptr;
        }
        auto const it = m_Providers.find(m_DefaultProviderName);
        if (it == m_Providers.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::string KeyManager::GetDefaultProviderName() const
    {
        std::shared_lock lock(m_Mutex);
        return m_DefaultProviderName;
    }

    std::vector<std::string> KeyManager::GetProviderNames() const
    {
        std::shared_lock lock(m_Mutex);
        std::vector<std::string> names;
        names.reserve(m_Providers.size());
        for (auto const& [name, config] : m_Providers)
        {
            names.push_back(name);
        }
        return names;
    }

    bool KeyManager::HasProviders() const
    {
        std::shared_lock lock(m_Mutex);
        return !m_Providers.empty();
    }

    bool KeyManager::AddProvider(std::string const& name, ProviderConfig config)
    {
        std::unique_lock lock(m_Mutex);
        if (m_Providers.count(name))
        {
            LOG_CORE_WARN("KeyManager::AddProvider: provider '{}' already exists", name);
            return false;
        }
        m_Providers[name] = std::move(config);
        m_Dirty = true;

        // If this is the first provider, make it the default
        if (m_Providers.size() == 1)
        {
            m_DefaultProviderName = name;
        }
        return true;
    }

    bool KeyManager::UpdateProvider(std::string const& name, ProviderConfig config)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Providers.find(name);
        if (it == m_Providers.end())
        {
            LOG_CORE_WARN("KeyManager::UpdateProvider: provider '{}' not found", name);
            return false;
        }
        it->second = std::move(config);
        m_Dirty = true;
        return true;
    }

    bool KeyManager::RemoveProvider(std::string const& name)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Providers.find(name);
        if (it == m_Providers.end())
        {
            LOG_CORE_WARN("KeyManager::RemoveProvider: provider '{}' not found", name);
            return false;
        }
        m_Providers.erase(it);
        m_Dirty = true;

        // If we removed the default, pick a new one (or clear it)
        if (m_DefaultProviderName == name)
        {
            if (!m_Providers.empty())
            {
                m_DefaultProviderName = m_Providers.begin()->first;
            }
            else
            {
                m_DefaultProviderName.clear();
            }
        }
        return true;
    }

    void KeyManager::SetDefaultProvider(std::string const& name)
    {
        std::unique_lock lock(m_Mutex);
        if (m_Providers.count(name) || name.empty())
        {
            m_DefaultProviderName = name;
        }
        else
        {
            LOG_CORE_WARN("KeyManager::SetDefaultProvider: provider '{}' not found", name);
        }
    }

    bool KeyManager::ParseProvidersJson(std::string const& json)
    {
        std::unique_lock lock(m_Mutex);
        m_Providers.clear();
        m_DefaultProviderName.clear();

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(json);
        simdjson::ondemand::document doc;

        auto error = parser.iterate(paddedJson).get(doc);
        if (error)
        {
            LOG_CORE_ERROR("KeyManager::ParseProvidersJson: JSON parse error: {}", simdjson::error_message(error));
            return false;
        }

        // Read version
        {
            int64_t version = 0;
            if (doc["version"].get_int64().get(version) == simdjson::SUCCESS)
            {
                if (version != 1)
                {
                    LOG_CORE_ERROR("KeyManager::ParseProvidersJson: unsupported version {}", version);
                    return false;
                }
            }
        }

        // Read default_provider
        {
            std::string_view defaultProvider;
            if (doc["default_provider"].get_string().get(defaultProvider) == simdjson::SUCCESS)
            {
                m_DefaultProviderName = std::string(defaultProvider);
            }
        }

        // Read providers object
        simdjson::ondemand::object providers;
        if (doc["providers"].get_object().get(providers) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("KeyManager::ParseProvidersJson: 'providers' object not found");
            return false;
        }

        for (auto field : providers)
        {
            std::string_view providerName = field.unescaped_key();
            simdjson::ondemand::object providerObj;
            if (field.value().get_object().get(providerObj) != simdjson::SUCCESS)
            {
                LOG_CORE_WARN("KeyManager::ParseProvidersJson: skipping non-object provider '{}'", providerName);
                continue;
            }

            ProviderConfig config;

            std::string_view sv;
            if (providerObj["display_name"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DisplayName = std::string(sv);
            }
            if (providerObj["endpoint"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Endpoint = std::string(sv);
            }
            if (providerObj["api_key"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiKey = std::string(sv);
            }
            if (providerObj["default_model"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_DefaultModel = std::string(sv);
            }
            if (providerObj["api_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_ApiType = std::string(sv);
            }

            // Credential type — backward compatible: absent means "api_key"
            if (providerObj["credential_type"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_CredentialType = std::string(sv);
            }

            // OAuth fields
            if (providerObj["refresh_token"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_RefreshToken = std::string(sv);
            }
            {
                int64_t expiresAt = 0;
                if (providerObj["expires_at"].get_int64().get(expiresAt) == simdjson::SUCCESS)
                {
                    config.m_ExpiresAt = expiresAt;
                }
            }
            if (providerObj["scopes"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Scopes = std::string(sv);
            }

            // Key pair fields
            if (providerObj["private_key_pem"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_PrivateKeyPem = std::string(sv);
            }

            // Basic auth fields
            if (providerObj["username"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Username = std::string(sv);
            }
            if (providerObj["password"].get_string().get(sv) == simdjson::SUCCESS)
            {
                config.m_Password = std::string(sv);
            }

            m_Providers[std::string(providerName)] = std::move(config);
        }

        // If default_provider was not set but we have providers, use the first one
        if (m_DefaultProviderName.empty() && !m_Providers.empty())
        {
            m_DefaultProviderName = m_Providers.begin()->first;
        }

        return true;
    }

    std::string KeyManager::SerializeToJson() const
    {
        // Hand-build JSON to avoid extra dependencies.
        // The JSON helper's SanitizeForJson could be used, but keys/values here
        // are controlled strings that don't need escaping beyond basic safety.
        std::ostringstream oss;
        oss << "{\n";
        oss << "    \"version\": 1,\n";
        oss << "    \"default_provider\": \"" << m_DefaultProviderName << "\",\n";
        oss << "    \"providers\": {\n";

        size_t i = 0;
        for (auto const& [name, config] : m_Providers)
        {
            oss << "        \"" << name << "\": {\n";
            oss << "            \"display_name\": \"" << config.m_DisplayName << "\",\n";
            oss << "            \"endpoint\": \"" << config.m_Endpoint << "\",\n";
            oss << "            \"api_key\": \"" << config.m_ApiKey << "\",\n";
            oss << "            \"default_model\": \"" << config.m_DefaultModel << "\",\n";
            oss << "            \"api_type\": \"" << config.m_ApiType << "\",\n";
            oss << "            \"credential_type\": \"" << config.m_CredentialType << "\"";

            if (config.m_CredentialType == "oauth")
            {
                oss << ",\n";
                oss << "            \"refresh_token\": \"" << config.m_RefreshToken << "\",\n";
                oss << "            \"expires_at\": " << config.m_ExpiresAt << ",\n";
                oss << "            \"scopes\": \"" << config.m_Scopes << "\"";
            }
            else if (config.m_CredentialType == "key_pair")
            {
                oss << ",\n";
                // PEM keys contain newlines — escape them for JSON
                std::string escapedPem;
                for (char c : config.m_PrivateKeyPem)
                {
                    if (c == '\n') escapedPem += "\\n";
                    else if (c == '\r') escapedPem += "\\r";
                    else if (c == '"') escapedPem += "\\\"";
                    else if (c == '\\') escapedPem += "\\\\";
                    else escapedPem += c;
                }
                oss << "            \"private_key_pem\": \"" << escapedPem << "\"";
            }
            else if (config.m_CredentialType == "credentials")
            {
                oss << ",\n";
                oss << "            \"username\": \"" << config.m_Username << "\",\n";
                oss << "            \"password\": \"" << config.m_Password << "\"";
            }

            oss << "\n";
            oss << "        }";
            if (++i < m_Providers.size())
            {
                oss << ",";
            }
            oss << "\n";
        }

        oss << "    }\n";
        oss << "}\n";
        return oss.str();
    }
} // namespace AIAssistant
