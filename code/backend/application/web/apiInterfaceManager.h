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

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json/configParser.h"
#include "keys/encryptedJsonStore.h"

namespace AIAssistant
{
    // Persistent, master-password-encrypted store for the AI-routing table —
    // the interface list plus the default + JCWF interface selectors (held by
    // stable name).  Replaces the plaintext "API interfaces" / "API index" /
    // "jcwf AI interface" keys that used to live in config.json, closing the
    // credential-exfiltration / reroute tamper vector (an attacker editing
    // config.json could repoint a key_name's url or flip the default).
    //
    // On-disk document (decrypted): { "version":1, "default_interface":"<name>",
    // "jcwf_interface":"<name>", "interfaces":[ {<interface>}, ... ] }.
    //
    // The interface (de)serialization + validation (UrlPolicy gate, fixture-path
    // confinement, auto-name, max-context resolution) lives here as the single
    // source of truth — the old ConfigParser::ParseInterfaces and the config.json
    // save-splice are removed.  WebServer hydrates EngineConfig::m_ApiInterfaces
    // (+ resolved default/jcwf indices) from a snapshot of this store after unlock,
    // so the dispatch hot path keeps reading the lock-free in-memory cache.
    class ApiInterfaceManager : public EncryptedJsonStore
    {
    public:
        using ApiInterface = ConfigParser::EngineConfig::ApiInterface;

        // Thin bool wrappers over EncryptedJsonStore that log at CORE level
        // (preserving a simple caller contract).  IsLoaded() is inherited.
        bool Load(std::filesystem::path const& path, std::string_view masterPassword);
        bool Save(std::filesystem::path const& path, std::string_view masterPassword);

        // ---- Snapshots for post-unlock hydration (thread-safe copies) ----
        std::vector<ApiInterface> GetInterfaces() const;
        std::string GetDefaultInterfaceName() const;
        std::string GetJcwfInterfaceName() const; // empty => use the default

        // ---- Mutations (REST handlers call these AFTER CheckMasterPasswordReauth) ----
        // Each validates + normalizes (auto-name, max-context, UrlPolicy, fixture
        // confinement).  On success the in-memory table is updated; the caller
        // then Save()s and re-hydrates EngineConfig.  Return false + populate
        // `error` (a machine-ish reason for the REST body) on rejection.
        bool UpsertInterface(ApiInterface iface, std::string& error);          // add or replace by name
        bool RemoveInterface(std::string const& name, std::string& error);
        bool SetDefaultInterface(std::string const& name, std::string& error); // name must exist
        bool SetJcwfInterface(std::string const& name, std::string& error);    // "" clears (use default)

        bool HasInterface(std::string const& name) const;

        // Resolve a stable interface name to its index within a hydrated table.
        // Shared by the dispatch read-sites that used to index by m_ApiIndex.
        static std::optional<size_t> FindInterfaceIndexByName(std::vector<ApiInterface> const& interfaces,
                                                              std::string_view name);

    private:
        std::string SerializeToJson() const override;
        std::expected<void, StoreError> ParseFromJson(std::string_view json) override;

        std::vector<ApiInterface> m_Interfaces;
        std::string m_DefaultInterface; // by name
        std::string m_JcwfInterface;    // by name; empty = use default
    };
} // namespace AIAssistant
