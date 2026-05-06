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

#include "web/mcpKeyManager.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "json/jsonHelper.h"
#include "keys/keyEncryption.h"
#include "log/secretRedactor.h"

namespace AIAssistant
{
    namespace
    {
        constexpr char const* kKeyPrefix = "mcp_";
        constexpr char const* kEnrollPrefix = "enroll_";
        constexpr size_t kKeyRandomBytes = 32;       // 64 hex chars
        constexpr size_t kKeyIdHexChars = 8;         // "mcp_" + 8 hex
        constexpr int kGracePeriodHours = 24;

        std::string RandomHex(size_t numBytes)
        {
            std::vector<uint8_t> buf(numBytes);
            if (RAND_bytes(buf.data(), static_cast<int>(numBytes)) != 1)
            {
                LOG_CORE_ERROR("McpKeyManager: RAND_bytes failed ({} bytes)", numBytes);
                return {};
            }
            static constexpr char const* hex = "0123456789abcdef";
            std::string out(numBytes * 2, '0');
            for (size_t i = 0; i < numBytes; ++i)
            {
                out[2 * i] = hex[(buf[i] >> 4) & 0xF];
                out[2 * i + 1] = hex[buf[i] & 0xF];
            }
            return out;
        }

        std::string Sha256Hex(std::string_view input)
        {
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<unsigned char const*>(input.data()), input.size(), hash);
            static constexpr char const* hex = "0123456789abcdef";
            std::string out(SHA256_DIGEST_LENGTH * 2, '0');
            for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            {
                out[2 * i] = hex[(hash[i] >> 4) & 0xF];
                out[2 * i + 1] = hex[hash[i] & 0xF];
            }
            return out;
        }

        // Constant-time comparison of two equal-length hex strings. Returns true on match.
        bool HashesEqual(std::string const& a, std::string const& b)
        {
            if (a.size() != b.size()) return false;
            return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
        }

        std::string Iso8601FromTimePoint(std::chrono::system_clock::time_point tp)
        {
            std::time_t t = std::chrono::system_clock::to_time_t(tp);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            std::ostringstream oss;
            oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            return oss.str();
        }

        std::string Iso8601NowUtc() { return Iso8601FromTimePoint(std::chrono::system_clock::now()); }

        std::string Iso8601FromNow(std::chrono::seconds delta)
        {
            return Iso8601FromTimePoint(std::chrono::system_clock::now() + delta);
        }

        std::optional<std::chrono::system_clock::time_point> ParseIso8601(std::string const& ts)
        {
            if (ts.empty()) return std::nullopt;
            std::tm tm{};
            std::istringstream iss(ts);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (iss.fail()) return std::nullopt;
#ifdef _WIN32
            std::time_t t = _mkgmtime(&tm);
#else
            std::time_t t = timegm(&tm);
#endif
            if (t == static_cast<std::time_t>(-1)) return std::nullopt;
            return std::chrono::system_clock::from_time_t(t);
        }

        // Returns integer days until `expiresAt`; negative if already past. 0 if parse fails.
        int DaysUntil(std::string const& expiresAt)
        {
            auto tp = ParseIso8601(expiresAt);
            if (!tp) return 0;
            auto delta = *tp - std::chrono::system_clock::now();
            return static_cast<int>(std::chrono::duration_cast<std::chrono::hours>(delta).count() / 24);
        }

        bool IsPast(std::string const& iso)
        {
            auto tp = ParseIso8601(iso);
            if (!tp) return false; // be lenient on unparseable timestamps
            return *tp < std::chrono::system_clock::now();
        }

        // Safe string-field read from a simdjson object. Returns default if missing/non-string.
        std::string ReadString(simdjson::ondemand::object& obj, char const* field, std::string const& def = "")
        {
            std::string_view sv;
            if (obj[field].get_string().get(sv) == simdjson::SUCCESS)
            {
                return std::string(sv);
            }
            return def;
        }

        bool ReadBool(simdjson::ondemand::object& obj, char const* field, bool def)
        {
            bool v = def;
            if (obj[field].get_bool().get(v) == simdjson::SUCCESS) return v;
            return def;
        }

        int64_t ReadInt(simdjson::ondemand::object& obj, char const* field, int64_t def)
        {
            int64_t v = def;
            if (obj[field].get_int64().get(v) == simdjson::SUCCESS) return v;
            return def;
        }
    } // namespace

    bool McpKeyManager::IsLoaded() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Loaded;
    }

    bool McpKeyManager::Load(std::filesystem::path const& path, std::string_view masterPassword)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            LOG_CORE_ERROR("McpKeyManager::Load: cannot open '{}'", path.string());
            return false;
        }

        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        if (blob.empty())
        {
            LOG_CORE_ERROR("McpKeyManager::Load: '{}' is empty", path.string());
            return false;
        }

        std::string json = KeyEncryption::Decrypt(blob, masterPassword);
        if (json.empty())
        {
            LOG_CORE_ERROR("McpKeyManager::Load: decryption failed for '{}'", path.string());
            return false;
        }

        std::lock_guard lock(m_Mutex);
        if (!ParseFromJson(json))
        {
            LOG_CORE_ERROR("McpKeyManager::Load: failed to parse decrypted JSON from '{}'", path.string());
            return false;
        }
        m_Loaded = true;
        LOG_CORE_INFO("McpKeyManager::Load: loaded {} key(s), {} enrollment(s) from '{}'",
                      m_Keys.size(), m_Enrollments.size(), path.string());
        return true;
    }

    bool McpKeyManager::Save(std::filesystem::path const& path, std::string_view masterPassword)
    {
        std::string json;
        {
            std::lock_guard lock(m_Mutex);
            json = SerializeToJson();
        }

        std::vector<uint8_t> blob = KeyEncryption::Encrypt(json, masterPassword);
        if (blob.empty())
        {
            LOG_CORE_ERROR("McpKeyManager::Save: encryption failed");
            return false;
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            LOG_CORE_ERROR("McpKeyManager::Save: cannot open '{}' for writing", path.string());
            return false;
        }
        file.write(reinterpret_cast<char const*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        file.close();

        {
            std::lock_guard lock(m_Mutex);
            m_Loaded = true;
        }
        LOG_CORE_INFO("McpKeyManager::Save: saved to '{}'", path.string());
        return true;
    }

    std::string McpKeyManager::CreateEnrollment(EnrollmentRequest const& req)
    {
        if (req.m_User.empty() || req.m_Role.empty())
        {
            LOG_CORE_WARN("McpKeyManager::CreateEnrollment: user and role are required");
            return {};
        }

        std::string const randomHex = RandomHex(kKeyRandomBytes);
        if (randomHex.empty()) return {};

        std::string rawEnrollmentToken = std::string(kEnrollPrefix) + randomHex;

        EnrollmentRecord rec;
        rec.m_TokenHash = Sha256Hex(rawEnrollmentToken);
        rec.m_User = req.m_User;
        rec.m_Role = req.m_Role;
        rec.m_AdhocEnabled = req.m_AdhocEnabled;
        rec.m_DiskQuotaMb = req.m_DiskQuotaMb;
        rec.m_DefaultCleanupPolicy = req.m_DefaultCleanupPolicy;
        rec.m_Description = req.m_Description;
        rec.m_KeyExpiryDays = req.m_KeyExpiryDays > 0 ? req.m_KeyExpiryDays : 90;
        rec.m_ExpiresAt = Iso8601FromNow(std::chrono::minutes(
            req.m_EnrollmentTtlMinutes > 0 ? req.m_EnrollmentTtlMinutes : 30));
        rec.m_CreatedBy = req.m_CreatedBy;
        rec.m_CreatedAt = Iso8601NowUtc();

        {
            std::lock_guard lock(m_Mutex);
            m_Enrollments.push_back(std::move(rec));
        }

        // Register the raw token with the redactor.  Once issued, the token may pass
        // through any number of downstream logs (web handlers, audit log, the admin's
        // own diagnostic trace) — the redactor scrubs every appearance.  Registration
        // outlives the enrollment's TTL on purpose: a leaked stale token is still a
        // leaked credential pattern that should not appear in logs.
        SecretRedactor::Get().AddSecret(rawEnrollmentToken);

        return rawEnrollmentToken;
    }

    std::optional<McpKeyManager::ActivateResult> McpKeyManager::ActivateEnrollment(
        std::string const& rawEnrollmentToken)
    {
        if (rawEnrollmentToken.rfind(kEnrollPrefix, 0) != 0)
        {
            return std::nullopt;
        }

        std::string const candidateHash = Sha256Hex(rawEnrollmentToken);

        std::lock_guard lock(m_Mutex);

        // Drop expired enrollments inline so a stale token cannot be activated.
        m_Enrollments.erase(
            std::remove_if(m_Enrollments.begin(), m_Enrollments.end(),
                           [](EnrollmentRecord const& r) { return IsPast(r.m_ExpiresAt); }),
            m_Enrollments.end());

        auto it = std::find_if(m_Enrollments.begin(), m_Enrollments.end(),
                               [&](EnrollmentRecord const& r)
                               { return HashesEqual(r.m_TokenHash, candidateHash); });
        if (it == m_Enrollments.end())
        {
            return std::nullopt;
        }

        // Generate the real key.
        std::string const randomHex = RandomHex(kKeyRandomBytes);
        if (randomHex.empty()) return std::nullopt;

        std::string rawKey = std::string(kKeyPrefix) + randomHex;
        std::string keyId = std::string(kKeyPrefix) + randomHex.substr(0, kKeyIdHexChars);

        Record rec;
        rec.m_KeyId = keyId;
        rec.m_KeyHash = Sha256Hex(rawKey);
        rec.m_User = it->m_User;
        rec.m_Role = it->m_Role;
        rec.m_AdhocEnabled = it->m_AdhocEnabled;
        rec.m_DiskQuotaMb = it->m_DiskQuotaMb;
        rec.m_DefaultCleanupPolicy = it->m_DefaultCleanupPolicy;
        rec.m_CreatedAt = Iso8601NowUtc();
        rec.m_ExpiresAt = Iso8601FromNow(std::chrono::hours(24 * it->m_KeyExpiryDays));
        rec.m_Enabled = true;
        rec.m_Description = it->m_Description;

        m_Keys.push_back(rec);
        m_Enrollments.erase(it);

        // Register the raw key before the std::move-out so any subsequent log line
        // containing it (response logging, audit log, etc.) gets scrubbed.  See
        // CreateEnrollment for the lifetime rationale.
        SecretRedactor::Get().AddSecret(rawKey);

        ActivateResult out;
        out.m_KeyId = keyId;
        out.m_RawKey = std::move(rawKey);
        out.m_Record = rec;
        return out;
    }

    std::optional<McpKeyManager::AuthResult> McpKeyManager::Authenticate(std::string const& rawKey)
    {
        if (rawKey.rfind(kKeyPrefix, 0) != 0) return std::nullopt;
        if (rawKey.size() < std::string(kKeyPrefix).size() + kKeyIdHexChars) return std::nullopt;

        std::string const keyId = rawKey.substr(0, std::string(kKeyPrefix).size() + kKeyIdHexChars);
        std::string const candidateHash = Sha256Hex(rawKey);

        std::lock_guard lock(m_Mutex);
        Record* rec = FindByKeyIdUnlocked(keyId);
        if (!rec) return std::nullopt;
        if (!HashesEqual(rec->m_KeyHash, candidateHash)) return std::nullopt;

        AuthResult out;
        out.m_Record = *rec;
        out.m_DaysUntilExpiry = DaysUntil(rec->m_ExpiresAt);

        // Update last-used opportunistically — this is best-effort and not persisted
        // until the next Save() call from a mutating endpoint.
        rec->m_LastUsedAt = Iso8601NowUtc();

        // Register the raw key only on successful auth: failed attempts (wrong hash,
        // unknown keyId) take the early-return paths above without registering, so an
        // attacker spamming guesses cannot pollute the redactor's value pool with
        // attacker-chosen strings.
        SecretRedactor::Get().AddSecret(rawKey);

        return out;
    }

    std::optional<McpKeyManager::ActivateResult> McpKeyManager::CreateBootstrapAdminKey()
    {
        std::lock_guard lock(m_Mutex);
        // One-shot: only runs on an empty store. This invariant is what makes
        // the unauthenticated `/api/settings/keys/unlock` handler safe to call
        // it — an attacker can't later bypass enrollment by hitting this path.
        if (!m_Keys.empty())
        {
            return std::nullopt;
        }

        std::string const randomHex = RandomHex(kKeyRandomBytes);
        if (randomHex.empty()) return std::nullopt;

        std::string rawKey = std::string(kKeyPrefix) + randomHex;
        std::string keyId = std::string(kKeyPrefix) + randomHex.substr(0, kKeyIdHexChars);

        Record rec;
        rec.m_KeyId = keyId;
        rec.m_KeyHash = Sha256Hex(rawKey);
        rec.m_User = "boss";
        rec.m_Role = "admin";
        rec.m_AdhocEnabled = true;
        rec.m_DiskQuotaMb = 1024;
        rec.m_DefaultCleanupPolicy = "ttl_72h";
        rec.m_CreatedAt = Iso8601NowUtc();
        rec.m_ExpiresAt = Iso8601FromNow(std::chrono::hours(24 * 90));
        rec.m_Enabled = true;
        rec.m_Description = "Bootstrap admin key created during first-run master-password setup";

        m_Keys.push_back(rec);

        // Register the raw key before std::move-out — see CreateEnrollment.
        SecretRedactor::Get().AddSecret(rawKey);

        ActivateResult out;
        out.m_KeyId = keyId;
        out.m_RawKey = std::move(rawKey);
        out.m_Record = rec;
        return out;
    }

    std::optional<McpKeyManager::RenewResult> McpKeyManager::SelfRenew(std::string const& currentRawKey)
    {
        // Authenticate() already handles validation and takes the lock; call it without holding the lock.
        auto auth = Authenticate(currentRawKey);
        if (!auth) return std::nullopt;
        if (!auth->m_Record.m_Enabled) return std::nullopt;
        if (auth->m_DaysUntilExpiry < 0) return std::nullopt;

        std::string const randomHex = RandomHex(kKeyRandomBytes);
        if (randomHex.empty()) return std::nullopt;

        std::string rawKey = std::string(kKeyPrefix) + randomHex;
        std::string keyId = std::string(kKeyPrefix) + randomHex.substr(0, kKeyIdHexChars);

        Record renewed = auth->m_Record;
        renewed.m_KeyId = keyId;
        renewed.m_KeyHash = Sha256Hex(rawKey);
        renewed.m_CreatedAt = Iso8601NowUtc();
        renewed.m_ExpiresAt = Iso8601FromNow(std::chrono::hours(24 * 90));
        renewed.m_LastUsedAt.clear();

        std::lock_guard lock(m_Mutex);
        // Put the old key into a 24h grace period (only shortens; never extends).
        if (Record* oldRec = FindByKeyIdUnlocked(auth->m_Record.m_KeyId))
        {
            std::string const graceExpiry = Iso8601FromNow(std::chrono::hours(kGracePeriodHours));
            auto currentExpiry = ParseIso8601(oldRec->m_ExpiresAt);
            auto graceTp = ParseIso8601(graceExpiry);
            if (!currentExpiry || !graceTp || *graceTp < *currentExpiry)
            {
                oldRec->m_ExpiresAt = graceExpiry;
            }
        }
        m_Keys.push_back(renewed);

        // Register the raw key before std::move-out — see CreateEnrollment.
        SecretRedactor::Get().AddSecret(rawKey);

        RenewResult out;
        out.m_KeyId = keyId;
        out.m_RawKey = std::move(rawKey);
        out.m_ExpiresAt = renewed.m_ExpiresAt;
        return out;
    }

    std::vector<McpKeyManager::Record> McpKeyManager::ListKeys() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Keys;
    }

    bool McpKeyManager::UpdateKey(std::string const& keyId, UpdateFields const& fields)
    {
        std::lock_guard lock(m_Mutex);
        Record* rec = FindByKeyIdUnlocked(keyId);
        if (!rec) return false;
        if (fields.m_Role) rec->m_Role = *fields.m_Role;
        if (fields.m_AdhocEnabled) rec->m_AdhocEnabled = *fields.m_AdhocEnabled;
        if (fields.m_DiskQuotaMb) rec->m_DiskQuotaMb = *fields.m_DiskQuotaMb;
        if (fields.m_DefaultCleanupPolicy) rec->m_DefaultCleanupPolicy = *fields.m_DefaultCleanupPolicy;
        if (fields.m_Enabled) rec->m_Enabled = *fields.m_Enabled;
        if (fields.m_Description) rec->m_Description = *fields.m_Description;
        if (fields.m_ExpiresAt) rec->m_ExpiresAt = *fields.m_ExpiresAt;
        return true;
    }

    bool McpKeyManager::RevokeKey(std::string const& keyId)
    {
        std::lock_guard lock(m_Mutex);
        auto it = std::find_if(m_Keys.begin(), m_Keys.end(),
                               [&](Record const& r) { return r.m_KeyId == keyId; });
        if (it == m_Keys.end()) return false;
        m_Keys.erase(it);
        return true;
    }

    void McpKeyManager::RecordDiskUsage(std::string const& user, size_t bytes)
    {
        std::lock_guard lock(m_Mutex);
        m_DiskUsageByUser[user] = bytes;
    }

    size_t McpKeyManager::GetDiskUsage(std::string const& user) const
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_DiskUsageByUser.find(user);
        return it == m_DiskUsageByUser.end() ? 0 : it->second;
    }

    void McpKeyManager::PurgeExpiredEnrollments()
    {
        std::lock_guard lock(m_Mutex);
        m_Enrollments.erase(
            std::remove_if(m_Enrollments.begin(), m_Enrollments.end(),
                           [](EnrollmentRecord const& r) { return IsPast(r.m_ExpiresAt); }),
            m_Enrollments.end());
    }

    McpKeyManager::Record* McpKeyManager::FindByKeyIdUnlocked(std::string const& keyId)
    {
        for (auto& rec : m_Keys)
        {
            if (rec.m_KeyId == keyId) return &rec;
        }
        return nullptr;
    }

    McpKeyManager::Record const* McpKeyManager::FindByKeyIdUnlocked(std::string const& keyId) const
    {
        for (auto const& rec : m_Keys)
        {
            if (rec.m_KeyId == keyId) return &rec;
        }
        return nullptr;
    }

    std::string McpKeyManager::SerializeToJson() const
    {
        std::ostringstream oss;
        oss << "{\n";
        oss << "    \"version\": 1,\n";
        oss << "    \"keys\": [";
        for (size_t i = 0; i < m_Keys.size(); ++i)
        {
            Record const& r = m_Keys[i];
            oss << (i == 0 ? "\n" : ",\n");
            oss << "        {\n";
            oss << "            \"key_id\": \""                 << JsonHelper::EscapeJsonString(r.m_KeyId) << "\",\n";
            oss << "            \"key_hash\": \""               << JsonHelper::EscapeJsonString(r.m_KeyHash) << "\",\n";
            oss << "            \"user\": \""                   << JsonHelper::EscapeJsonString(r.m_User) << "\",\n";
            oss << "            \"role\": \""                   << JsonHelper::EscapeJsonString(r.m_Role) << "\",\n";
            oss << "            \"adhoc_enabled\": "            << (r.m_AdhocEnabled ? "true" : "false") << ",\n";
            oss << "            \"disk_quota_mb\": "            << r.m_DiskQuotaMb << ",\n";
            oss << "            \"default_cleanup_policy\": \"" << JsonHelper::EscapeJsonString(r.m_DefaultCleanupPolicy) << "\",\n";
            oss << "            \"created_at\": \""             << JsonHelper::EscapeJsonString(r.m_CreatedAt) << "\",\n";
            oss << "            \"expires_at\": \""             << JsonHelper::EscapeJsonString(r.m_ExpiresAt) << "\",\n";
            oss << "            \"last_used_at\": \""           << JsonHelper::EscapeJsonString(r.m_LastUsedAt) << "\",\n";
            oss << "            \"enabled\": "                  << (r.m_Enabled ? "true" : "false") << ",\n";
            oss << "            \"description\": \""            << JsonHelper::EscapeJsonString(r.m_Description) << "\"\n";
            oss << "        }";
        }
        oss << (m_Keys.empty() ? "" : "\n    ") << "],\n";

        oss << "    \"enrollments\": [";
        for (size_t i = 0; i < m_Enrollments.size(); ++i)
        {
            EnrollmentRecord const& r = m_Enrollments[i];
            oss << (i == 0 ? "\n" : ",\n");
            oss << "        {\n";
            oss << "            \"token_hash\": \""             << JsonHelper::EscapeJsonString(r.m_TokenHash) << "\",\n";
            oss << "            \"user\": \""                   << JsonHelper::EscapeJsonString(r.m_User) << "\",\n";
            oss << "            \"role\": \""                   << JsonHelper::EscapeJsonString(r.m_Role) << "\",\n";
            oss << "            \"adhoc_enabled\": "            << (r.m_AdhocEnabled ? "true" : "false") << ",\n";
            oss << "            \"disk_quota_mb\": "            << r.m_DiskQuotaMb << ",\n";
            oss << "            \"default_cleanup_policy\": \"" << JsonHelper::EscapeJsonString(r.m_DefaultCleanupPolicy) << "\",\n";
            oss << "            \"description\": \""            << JsonHelper::EscapeJsonString(r.m_Description) << "\",\n";
            oss << "            \"key_expiry_days\": "          << r.m_KeyExpiryDays << ",\n";
            oss << "            \"expires_at\": \""             << JsonHelper::EscapeJsonString(r.m_ExpiresAt) << "\",\n";
            oss << "            \"created_by\": \""             << JsonHelper::EscapeJsonString(r.m_CreatedBy) << "\",\n";
            oss << "            \"created_at\": \""             << JsonHelper::EscapeJsonString(r.m_CreatedAt) << "\"\n";
            oss << "        }";
        }
        oss << (m_Enrollments.empty() ? "" : "\n    ") << "]\n";
        oss << "}\n";
        return oss.str();
    }

    bool McpKeyManager::ParseFromJson(std::string const& json)
    {
        m_Keys.clear();
        m_Enrollments.clear();

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(json);
        simdjson::ondemand::document doc;
        if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
        {
            LOG_CORE_ERROR("McpKeyManager::ParseFromJson: iterate failed");
            return false;
        }

        int64_t version = 1;
        if (auto err = doc["version"].get_int64().get(version); err != simdjson::SUCCESS)
        {
            version = 1; // default if absent or not an int
        }
        if (version != 1)
        {
            LOG_CORE_ERROR("McpKeyManager::ParseFromJson: unsupported version {}", version);
            return false;
        }

        // keys[]
        simdjson::ondemand::array keysArr;
        if (doc["keys"].get_array().get(keysArr) == simdjson::SUCCESS)
        {
            for (auto elem : keysArr)
            {
                simdjson::ondemand::object obj;
                if (elem.get_object().get(obj) != simdjson::SUCCESS) continue;
                Record r;
                r.m_KeyId                = ReadString(obj, "key_id");
                r.m_KeyHash              = ReadString(obj, "key_hash");
                r.m_User                 = ReadString(obj, "user");
                r.m_Role                 = ReadString(obj, "role", "viewer");
                r.m_AdhocEnabled         = ReadBool(obj, "adhoc_enabled", false);
                r.m_DiskQuotaMb          = static_cast<int>(ReadInt(obj, "disk_quota_mb", 1024));
                r.m_DefaultCleanupPolicy = ReadString(obj, "default_cleanup_policy", "ttl_72h");
                r.m_CreatedAt            = ReadString(obj, "created_at");
                r.m_ExpiresAt            = ReadString(obj, "expires_at");
                r.m_LastUsedAt           = ReadString(obj, "last_used_at");
                r.m_Enabled              = ReadBool(obj, "enabled", true);
                r.m_Description          = ReadString(obj, "description");
                if (!r.m_KeyId.empty() && !r.m_KeyHash.empty())
                {
                    m_Keys.push_back(std::move(r));
                }
            }
        }

        // enrollments[]
        simdjson::ondemand::array enrArr;
        if (doc["enrollments"].get_array().get(enrArr) == simdjson::SUCCESS)
        {
            for (auto elem : enrArr)
            {
                simdjson::ondemand::object obj;
                if (elem.get_object().get(obj) != simdjson::SUCCESS) continue;
                EnrollmentRecord r;
                r.m_TokenHash            = ReadString(obj, "token_hash");
                r.m_User                 = ReadString(obj, "user");
                r.m_Role                 = ReadString(obj, "role", "viewer");
                r.m_AdhocEnabled         = ReadBool(obj, "adhoc_enabled", false);
                r.m_DiskQuotaMb          = static_cast<int>(ReadInt(obj, "disk_quota_mb", 1024));
                r.m_DefaultCleanupPolicy = ReadString(obj, "default_cleanup_policy", "ttl_72h");
                r.m_Description          = ReadString(obj, "description");
                r.m_KeyExpiryDays        = static_cast<int>(ReadInt(obj, "key_expiry_days", 90));
                r.m_ExpiresAt            = ReadString(obj, "expires_at");
                r.m_CreatedBy            = ReadString(obj, "created_by");
                r.m_CreatedAt            = ReadString(obj, "created_at");
                if (!r.m_TokenHash.empty())
                {
                    m_Enrollments.push_back(std::move(r));
                }
            }
        }

        return true;
    }
} // namespace AIAssistant
