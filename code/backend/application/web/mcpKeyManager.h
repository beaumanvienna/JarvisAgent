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

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "keys/encryptedJsonStore.h"

namespace AIAssistant
{
    // Persistent store of MCP API keys and pending enrollment tokens, encrypted
    // at rest with the master password (AES-256-GCM + PBKDF2 via KeyEncryption,
    // same format as keys.json.enc).
    //
    // Keys carry per-user identity, role, and adhoc/quota metadata. Only the
    // SHA-256 hash of each raw key is stored — the raw key is shown to the user
    // exactly once, at activation or self-renewal time.
    //
    // Raw key format: "mcp_" + 64 hex chars (32 random bytes).
    // Key ID:         "mcp_" + first 8 hex chars (prefix of the raw key — safe to log).
    //
    // Raw enrollment token format: "enroll_" + 64 hex chars.
    class McpKeyManager : public EncryptedJsonStore
    {
    public:
        struct Record
        {
            std::string m_KeyId;                  // "mcp_a1b2c3d4"
            std::string m_KeyHash;                // hex SHA-256 of raw key
            std::string m_User;                   // identity (free-text, e.g. "alice@company.com")
            std::string m_Role;                   // "admin" | "operator" | "viewer"
            bool m_AdhocEnabled{false};
            int m_DiskQuotaMb{1024};
            std::string m_DefaultCleanupPolicy{"ttl_72h"};
            std::string m_CreatedAt;              // ISO-8601 UTC
            std::string m_ExpiresAt;              // ISO-8601 UTC
            std::string m_LastUsedAt;             // ISO-8601 UTC, empty if never
            bool m_Enabled{true};
            std::string m_Description;
        };

        struct EnrollmentRequest
        {
            std::string m_User;
            std::string m_Role{"operator"};
            bool m_AdhocEnabled{false};
            int m_DiskQuotaMb{1024};
            std::string m_DefaultCleanupPolicy{"ttl_72h"};
            std::string m_Description;
            int m_KeyExpiryDays{90};              // lifetime of the eventual key, not the enrollment
            int m_EnrollmentTtlMinutes{30};
            std::string m_CreatedBy;              // admin identity, for audit trail
        };

        struct EnrollmentRecord
        {
            std::string m_TokenHash;              // hex SHA-256 of raw enrollment token
            std::string m_User;
            std::string m_Role;
            bool m_AdhocEnabled{false};
            int m_DiskQuotaMb{1024};
            std::string m_DefaultCleanupPolicy;
            std::string m_Description;
            int m_KeyExpiryDays{90};
            std::string m_ExpiresAt;              // when this enrollment stops being usable
            std::string m_CreatedBy;
            std::string m_CreatedAt;
        };

        // Storage lifecycle — thin wrappers over EncryptedJsonStore that log at
        // CORE level (preserving callers' bool contract) and expose key/enrollment
        // counts in the success line.  IsLoaded() is inherited from the base.
        bool Load(std::filesystem::path const& path, std::string_view masterPassword);
        bool Save(std::filesystem::path const& path, std::string_view masterPassword);

        // Enrollment flow — admin creates, user exchanges for the real key.
        // Returns the raw enrollment token (shown to the admin once) or empty string on failure.
        std::string CreateEnrollment(EnrollmentRequest const& req);

        struct ActivateResult
        {
            std::string m_KeyId;
            std::string m_RawKey;
            Record m_Record;
        };
        std::optional<ActivateResult> ActivateEnrollment(std::string const& rawEnrollmentToken);

        // Authenticate a raw key (including the "mcp_" prefix).
        struct AuthResult
        {
            Record m_Record;
            int m_DaysUntilExpiry{0};             // negative if already expired
        };
        std::optional<AuthResult> Authenticate(std::string const& rawKey);

        // Bootstrap: create the **first** admin MCP key directly, bypassing the
        // enrollment-token dance. Only succeeds when the key store has no
        // existing keys, so this is a one-shot "you just set the master password,
        // here's your identity" helper. Invoked from the dashboard's first-run
        // flow so users don't have to scrape the log for the enrollment banner.
        std::optional<ActivateResult> CreateBootstrapAdminKey();

        // Self-service rotation — caller proves possession of the current (still-valid) key.
        // Old key is kept for 24h with its expires_at set to now+24h as a grace period.
        struct RenewResult
        {
            std::string m_KeyId;
            std::string m_RawKey;
            std::string m_ExpiresAt;
        };
        std::optional<RenewResult> SelfRenew(std::string const& currentRawKey);

        // Admin operations. Redacted listing — no raw keys, hashes are metadata only.
        std::vector<Record> ListKeys() const;

        struct UpdateFields
        {
            std::optional<std::string> m_Role;
            std::optional<bool> m_AdhocEnabled;
            std::optional<int> m_DiskQuotaMb;
            std::optional<std::string> m_DefaultCleanupPolicy;
            std::optional<bool> m_Enabled;
            std::optional<std::string> m_Description;
            std::optional<std::string> m_ExpiresAt;
        };
        bool UpdateKey(std::string const& keyId, UpdateFields const& fields);
        bool RevokeKey(std::string const& keyId);

        // Adhoc disk quota bookkeeping (bytes, keyed by user identity).
        void RecordDiskUsage(std::string const& user, size_t bytes);
        size_t GetDiskUsage(std::string const& user) const;

        // Drop enrollment tokens whose TTL has elapsed. Called opportunistically
        // from activate/authenticate paths — no background thread required.
        void PurgeExpiredEnrollments();

    private:
        std::string SerializeToJson() const override;
        std::expected<void, StoreError> ParseFromJson(std::string_view json) override;

        // Mutex-free helpers — caller already holds m_Mutex.
        Record* FindByKeyIdUnlocked(std::string const& keyId);
        Record const* FindByKeyIdUnlocked(std::string const& keyId) const;

        // m_Mutex + m_Loaded live in the EncryptedJsonStore base.
        std::vector<Record> m_Keys;
        std::vector<EnrollmentRecord> m_Enrollments;
        std::unordered_map<std::string, size_t> m_DiskUsageByUser;
    };
} // namespace AIAssistant
