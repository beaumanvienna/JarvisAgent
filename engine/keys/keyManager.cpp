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
#include "json/jsonHelper.h"
#include "keys/keyEncryption.h"
#include "keys/keyManager.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    namespace
    {
        // Blast-radius caps on keystore parsing.  Either limit being hit
        // means the keystore was tampered with (a legitimately-curated key
        // file is < 100 KB and has < 50 providers in any realistic
        // deployment) — fail closed with ERROR.
        constexpr size_t kMaxKeysFileBytes = 4ull * 1024 * 1024;  // 4 MB
        constexpr size_t kMaxProviders     = 1024;
    } // namespace

    bool KeyManager::Load(std::filesystem::path const& keysFilePath, std::string_view masterPassword)
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
        if (blob.size() > kMaxKeysFileBytes)
        {
            LOG_CORE_ERROR("KeyManager::Load: '{}' is {} bytes, exceeds kMaxKeysFileBytes={} — refusing to parse",
                           keysFilePath.string(), blob.size(), kMaxKeysFileBytes);
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

        {
            std::unique_lock pwdLock(m_MasterPasswordMutex);
            m_MasterPassword.Set(masterPassword);
        }
        m_Dirty.store(false);

        size_t credentialCount;
        {
            std::shared_lock lock(m_Mutex);
            credentialCount = m_Credentials.size();
        }
        LOG_CORE_INFO("KeyManager::Load: loaded {} provider(s) from '{}'", credentialCount, keysFilePath.string());
        return true;
    }

    bool KeyManager::Save(std::filesystem::path const& keysFilePath, std::string_view masterPassword)
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

        {
            std::unique_lock pwdLock(m_MasterPasswordMutex);
            m_MasterPassword.Set(masterPassword);
        }
        m_Dirty.store(false);

        size_t credentialCount;
        {
            std::shared_lock lock(m_Mutex);
            credentialCount = m_Credentials.size();
        }
        LOG_CORE_INFO("KeyManager::Save: saved {} provider(s) to '{}'", credentialCount, keysFilePath.string());
        return true;
    }

    bool KeyManager::Unlock(std::string_view masterPassword)
    {
        // Capture the path once under the path mutex so a concurrent
        // SetKeysFilePath can't swap m_KeysFilePath between the existence
        // check and the Load() call.
        std::filesystem::path capturedPath;
        {
            std::shared_lock pathLock(m_KeysFilePathMutex);
            capturedPath = m_KeysFilePath;
        }

        if (capturedPath.empty())
        {
            LOG_CORE_ERROR("KeyManager::Unlock: no keys file path set");
            return false;
        }

        if (!std::filesystem::exists(capturedPath))
        {
            LOG_CORE_ERROR("KeyManager::Unlock: keys file '{}' does not exist", capturedPath.string());
            m_KeyLoadStatus = KeyLoadStatus::NoKeysFile;
            return false;
        }

        if (Load(capturedPath, masterPassword))
        {
            m_KeyLoadStatus = KeyLoadStatus::Ok;
            LOG_CORE_INFO("KeyManager::Unlock: successfully unlocked with runtime password");
            return true;
        }

        m_KeyLoadStatus = KeyLoadStatus::WrongPassword;
        return false;
    }

    void KeyManager::SetKeysFilePath(std::filesystem::path const& path)
    {
        std::unique_lock pathLock(m_KeysFilePathMutex);
        m_KeysFilePath = path;
    }

    std::filesystem::path KeyManager::GetKeysFilePath() const
    {
        std::shared_lock pathLock(m_KeysFilePathMutex);
        return m_KeysFilePath;
    }

    bool KeyManager::HasCredential(std::string const& name) const
    {
        std::shared_lock lock(m_Mutex);
        return m_Credentials.find(name) != m_Credentials.end();
    }

    bool KeyManager::HasDefaultCredential() const
    {
        std::shared_lock lock(m_Mutex);
        if (m_DefaultProviderName.empty())
        {
            return false;
        }
        return m_Credentials.find(m_DefaultProviderName) != m_Credentials.end();
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
        names.reserve(m_Credentials.size());
        for (auto const& [name, cred] : m_Credentials)
        {
            names.push_back(name);
        }
        return names;
    }

    bool KeyManager::HasProviders() const
    {
        std::shared_lock lock(m_Mutex);
        return !m_Credentials.empty();
    }

    bool KeyManager::AddCredential(std::string const& name, std::unique_ptr<ICredential> cred)
    {
        if (!cred)
        {
            LOG_CORE_ERROR("KeyManager::AddCredential: '{}' rejected — null credential", name);
            return false;
        }
        std::unique_lock lock(m_Mutex);
        if (m_Credentials.count(name))
        {
            LOG_CORE_WARN("KeyManager::AddCredential: provider '{}' already exists", name);
            return false;
        }

        cred->m_Name = name;
        cred->RegisterSecrets();

        m_Credentials[name] = std::move(cred);
        m_Dirty.store(true);

        if (m_Credentials.size() == 1)
        {
            m_DefaultProviderName = name;
        }
        return true;
    }

    bool KeyManager::RemoveProvider(std::string const& name)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Credentials.find(name);
        if (it == m_Credentials.end())
        {
            LOG_CORE_WARN("KeyManager::RemoveProvider: provider '{}' not found", name);
            return false;
        }
        m_Credentials.erase(it);
        m_Dirty.store(true);

        // If we removed the default, pick a new one (or clear it)
        if (m_DefaultProviderName == name)
        {
            if (!m_Credentials.empty())
            {
                m_DefaultProviderName = m_Credentials.begin()->first;
            }
            else
            {
                m_DefaultProviderName.clear();
            }
        }
        return true;
    }

    bool KeyManager::SetDefaultProvider(std::string const& name)
    {
        if (name.empty())
        {
            LOG_CORE_WARN("KeyManager::SetDefaultProvider: refusing to set default to empty name; "
                          "call ClearDefaultProvider() explicitly to clear");
            return false;
        }
        std::unique_lock lock(m_Mutex);
        if (m_Credentials.count(name) == 0)
        {
            LOG_CORE_WARN("KeyManager::SetDefaultProvider: provider '{}' not found", name);
            return false;
        }
        m_DefaultProviderName = name;
        return true;
    }

    void KeyManager::ClearDefaultProvider()
    {
        std::unique_lock lock(m_Mutex);
        m_DefaultProviderName.clear();
    }

    bool KeyManager::ParseProvidersJson(std::string const& json)
    {
        std::unique_lock lock(m_Mutex);
        m_Credentials.clear();
        m_DefaultProviderName.clear();

        simdjson::ondemand::parser parser;
        simdjson::padded_string paddedJson(json);
        simdjson::ondemand::document doc;

        if (auto err = parser.iterate(paddedJson).get(doc); err != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("KeyManager::ParseProvidersJson: JSON parse error: {}", simdjson::error_message(err));
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

        size_t providerCount = 0;
        for (auto field : providers)
        {
            if (++providerCount > kMaxProviders)
            {
                LOG_CORE_ERROR("KeyManager::ParseProvidersJson: provider count exceeds kMaxProviders={} — "
                               "refusing further entries; aborting parse",
                               kMaxProviders);
                m_Credentials.clear();
                m_DefaultProviderName.clear();
                return false;
            }
            std::string_view providerName = field.unescaped_key();
            simdjson::ondemand::object providerObj;
            if (field.value().get_object().get(providerObj) != simdjson::SUCCESS)
            {
                LOG_CORE_WARN("KeyManager::ParseProvidersJson: skipping non-object provider '{}'", providerName);
                continue;
            }

            // Dispatch through CredentialFactory — returns nullptr on unknown credential_type
            // (factory already logs in that path).
            std::unique_ptr<ICredential> cred = CredentialFactory::CreateFromJson(providerObj);
            if (!cred)
            {
                LOG_CORE_WARN("KeyManager::ParseProvidersJson: skipping provider '{}' (factory returned null)",
                              providerName);
                continue;
            }
            cred->m_Name = std::string(providerName);
            cred->RegisterSecrets();

            m_Credentials[std::string(providerName)] = std::move(cred);
        }

        // If default_provider was not set but we have providers, use the first one
        if (m_DefaultProviderName.empty() && !m_Credentials.empty())
        {
            m_DefaultProviderName = m_Credentials.begin()->first;
        }

        return true;
    }

    std::string KeyManager::SerializeToJson() const
    {
        // Iterate `m_Credentials` (the source of truth, with SecureString-protected
        // secret fields) and route each entry through `CredentialFactory::SerializeToJson`.
        std::ostringstream oss;
        oss << "{\n";
        oss << "    \"version\": 1,\n";
        oss << "    \"default_provider\": \"" << JsonHelper::EscapeJsonString(m_DefaultProviderName) << "\",\n";
        oss << "    \"providers\": {\n";

        size_t i = 0;
        for (auto const& [name, cred] : m_Credentials)
        {
            oss << "        \"" << JsonHelper::EscapeJsonString(name) << "\": {\n";
            oss << CredentialFactory::SerializeToJson(*cred);
            oss << "        }";
            if (++i < m_Credentials.size())
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
