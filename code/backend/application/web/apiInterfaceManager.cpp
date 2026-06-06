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

#include "web/apiInterfaceManager.h"

#include <mutex>
#include <shared_mutex>
#include <sstream>

#include "simdjson/simdjson.h"

#include "engine.h"
#include "file/pathConfinement.h"
#include "json/jsonHelper.h"
#include "network/urlPolicy.h"

namespace AIAssistant
{
    using EngineConfig = ConfigParser::EngineConfig;
    using ApiInterface = EngineConfig::ApiInterface;

    namespace
    {
        // Validate + normalize an interface in place (auto-name, max-context
        // resolution, is_mock/fixture coupling, URL-policy gate).  Single source
        // of truth shared by the JSON loader and the REST upsert path, so a
        // runtime-added interface gets exactly the same treatment a loaded one
        // does.  Returns std::nullopt on success, or an error string on rejection.
        std::optional<std::string> ValidateAndNormalize(ApiInterface& iface)
        {
            // is_mock requires a confined fixture_path.
            if (iface.m_IsMock)
            {
                if (iface.m_FixturePath.empty())
                {
                    return "is_mock=true requires a non-empty fixture_path";
                }
                std::filesystem::path const confined = ConfineUnderProjectRoot(iface.m_FixturePath);
                if (confined.empty())
                {
                    return "fixture_path '" + iface.m_FixturePath +
                           "' rejected by path confinement (outside project root, symlink escape, or unresolvable)";
                }
            }

            // A valid api_type is mandatory (the dispatcher branches on it).
            if (iface.m_InterfaceType == EngineConfig::InterfaceType::InvalidAPI)
            {
                return "missing or unknown api_type (expected API1-API6)";
            }

            // Auto-generate a stable name from url + model + api type when absent.
            if (iface.m_Name.empty())
            {
                std::string_view const apiTypeStr = EngineConfig::InterfaceTypeToString(iface.m_InterfaceType);
                iface.m_Name = EngineConfig::GenerateInterfaceName(iface.m_Url, iface.m_Model,
                                                                   std::string(apiTypeStr));
            }

            // Resolve the context window from the curated model table when unset.
            if (iface.m_MaxContextTokens == 0 && !iface.m_Model.empty())
            {
                iface.m_MaxContextTokens = EngineConfig::ResolveMaxContextTokensFromModel(iface.m_Model);
            }

            // Plain-HTTP / credentialed-plaintext SSRF gate — http:// only for
            // loopback, never with a key_name.  Same gate the config loader used.
            if (auto const result = UrlPolicy::ValidateAiInterfaceUrl(iface.m_Url, iface.m_KeyName);
                !result.has_value())
            {
                if (result.error().m_Code == UrlPolicy::UrlPolicyErrorCode::CredentialedPlaintextHttp)
                {
                    UrlPolicy::RecordCredentialedPlaintextHttpRejection();
                }
                else
                {
                    UrlPolicy::RecordUrlPolicyRejection();
                }
                return std::string(UrlPolicy::Describe(result.error().m_Code)) + ": " +
                       result.error().m_Details;
            }
            return std::nullopt;
        }

        // Parse one interface object from the decrypted document.  Returns the
        // validated+normalized interface, or nullopt (with an ERROR log) when the
        // entry is malformed or rejected — a bad row is skipped, not fatal.
        std::optional<ApiInterface> ParseInterfaceObject(simdjson::ondemand::object& obj)
        {
            using namespace simdjson;
            ApiInterface iface;

            for (auto field : obj)
            {
                std::string_view key = field.unescaped_key();
                if (key == "name")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_Name = v;
                }
                else if (key == "description")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_Description = v;
                }
                else if (key == "url")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_Url = v;
                }
                else if (key == "model")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_Model = v;
                }
                else if (key == "key_name")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_KeyName = v;
                }
                else if (key == "API")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS)
                    {
                        iface.m_InterfaceType = EngineConfig::InterfaceTypeFromString(v);
                    }
                }
                else if (key == "max_context_tokens")
                {
                    uint64_t v = 0;
                    if (field.value().get_uint64().get(v) == SUCCESS) iface.m_MaxContextTokens = v;
                }
                else if (key == "default_output_tokens")
                {
                    int64_t v = 0;
                    if (field.value().get_int64().get(v) == SUCCESS && v > 0)
                        iface.m_DefaultOutputTokens = static_cast<int32_t>(v);
                }
                else if (key == "is_mock")
                {
                    bool v = false;
                    if (field.value().get_bool().get(v) == SUCCESS) iface.m_IsMock = v;
                }
                else if (key == "fixture_path")
                {
                    std::string_view v;
                    if (field.value().get_string().get(v) == SUCCESS) iface.m_FixturePath = v;
                }
                else if (key == "rate_limit")
                {
                    ondemand::object rl;
                    if (field.value().get_object().get(rl) != SUCCESS) continue;
                    auto& rateLimit = iface.m_RateLimit;
                    auto& budget = rateLimit.m_RequestBudget;
                    for (auto rlField : rl)
                    {
                        std::string_view rlKey = rlField.unescaped_key();
                        if (rlKey == "initial_concurrency_probe")
                        {
                            int64_t v = 0;
                            if (rlField.value().get_int64().get(v) == SUCCESS)
                                rateLimit.m_InitialConcurrencyProbe = static_cast<int>(v);
                        }
                        else if (rlKey == "max_concurrency")
                        {
                            int64_t v = 0;
                            if (rlField.value().get_int64().get(v) == SUCCESS && v > 0)
                                rateLimit.m_MaxConcurrency = static_cast<int>(v);
                        }
                        else if (rlKey == "max_retries_429")
                        {
                            int64_t v = 0;
                            if (rlField.value().get_int64().get(v) == SUCCESS && v >= 0)
                                rateLimit.m_MaxRetries429 = static_cast<int>(v);
                        }
                        else if (rlKey == "max_retries_transient")
                        {
                            int64_t v = 0;
                            if (rlField.value().get_int64().get(v) == SUCCESS && v >= 0)
                                rateLimit.m_MaxRetriesTransient = static_cast<int>(v);
                        }
                        else if (rlKey == "base_retry_ms")
                        {
                            int64_t v = 0;
                            if (rlField.value().get_int64().get(v) == SUCCESS && v > 0)
                                rateLimit.m_BaseRetryMs = static_cast<int>(v);
                        }
                        else if (rlKey == "request_budget")
                        {
                            ondemand::object bo;
                            if (rlField.value().get_object().get(bo) != SUCCESS) continue;
                            for (auto bField : bo)
                            {
                                std::string_view bKey = bField.unescaped_key();
                                double v = 0.0;
                                if (bField.value().get_double().get(v) != SUCCESS) continue;
                                if (bKey == "per_1k_input_token_seconds") budget.m_Per1kInputTokenSeconds = v;
                                else if (bKey == "per_1k_output_token_seconds") budget.m_Per1kOutputTokenSeconds = v;
                                else if (bKey == "fixed_overhead_seconds") budget.m_FixedOverheadSeconds = v;
                                else if (bKey == "safety_margin_factor") budget.m_SafetyMarginFactor = v;
                                else if (bKey == "min_seconds") budget.m_MinSeconds = v;
                                else if (bKey == "max_seconds") budget.m_MaxSeconds = v;
                            }
                        }
                    }
                }
            }

            if (auto const err = ValidateAndNormalize(iface); err.has_value())
            {
                LOG_CORE_ERROR("ApiInterfaceManager: rejected interface '{}' url='{}': {}", iface.m_Name,
                               iface.m_Url, *err);
                return std::nullopt;
            }
            return iface;
        }

        // Serialize one interface to a JSON object (lossless — every field, full
        // rate_limit + request_budget — so the encrypted round-trip is exact).
        std::string SerializeInterfaceObject(ApiInterface const& iface)
        {
            auto& budget = iface.m_RateLimit.m_RequestBudget;
            std::ostringstream o;
            o << "{";
            o << "\"name\":\"" << JsonHelper::EscapeJsonString(iface.m_Name) << "\",";
            o << "\"description\":\"" << JsonHelper::EscapeJsonString(iface.m_Description) << "\",";
            o << "\"url\":\"" << JsonHelper::EscapeJsonString(iface.m_Url) << "\",";
            o << "\"model\":\"" << JsonHelper::EscapeJsonString(iface.m_Model) << "\",";
            o << "\"key_name\":\"" << JsonHelper::EscapeJsonString(iface.m_KeyName) << "\",";
            o << "\"API\":\"" << EngineConfig::InterfaceTypeToString(iface.m_InterfaceType) << "\",";
            o << "\"max_context_tokens\":" << iface.m_MaxContextTokens << ",";
            o << "\"default_output_tokens\":" << iface.m_DefaultOutputTokens << ",";
            o << "\"is_mock\":" << (iface.m_IsMock ? "true" : "false") << ",";
            o << "\"fixture_path\":\"" << JsonHelper::EscapeJsonString(iface.m_FixturePath) << "\",";
            o << "\"rate_limit\":{";
            o << "\"initial_concurrency_probe\":" << iface.m_RateLimit.m_InitialConcurrencyProbe << ",";
            o << "\"max_concurrency\":" << iface.m_RateLimit.m_MaxConcurrency << ",";
            o << "\"max_retries_429\":" << iface.m_RateLimit.m_MaxRetries429 << ",";
            o << "\"max_retries_transient\":" << iface.m_RateLimit.m_MaxRetriesTransient << ",";
            o << "\"base_retry_ms\":" << iface.m_RateLimit.m_BaseRetryMs << ",";
            o << "\"request_budget\":{";
            o << "\"per_1k_input_token_seconds\":" << budget.m_Per1kInputTokenSeconds << ",";
            o << "\"per_1k_output_token_seconds\":" << budget.m_Per1kOutputTokenSeconds << ",";
            o << "\"fixed_overhead_seconds\":" << budget.m_FixedOverheadSeconds << ",";
            o << "\"safety_margin_factor\":" << budget.m_SafetyMarginFactor << ",";
            o << "\"min_seconds\":" << budget.m_MinSeconds << ",";
            o << "\"max_seconds\":" << budget.m_MaxSeconds;
            o << "}}}";
            return o.str();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // EncryptedJsonStore overrides
    // -------------------------------------------------------------------------

    std::string ApiInterfaceManager::SerializeToJson() const
    {
        // Caller (base Save) holds m_Mutex shared.
        std::ostringstream o;
        o << "{\"version\":1,";
        o << "\"default_interface\":\"" << JsonHelper::EscapeJsonString(m_DefaultInterface) << "\",";
        o << "\"jcwf_interface\":\"" << JsonHelper::EscapeJsonString(m_JcwfInterface) << "\",";
        o << "\"interfaces\":[";
        for (size_t i = 0; i < m_Interfaces.size(); ++i)
        {
            if (i) o << ",";
            o << SerializeInterfaceObject(m_Interfaces[i]);
        }
        o << "]}";
        return o.str();
    }

    std::expected<void, StoreError> ApiInterfaceManager::ParseFromJson(std::string_view json)
    {
        // Caller (base Load) holds m_Mutex unique.
        using namespace simdjson;
        std::vector<ApiInterface> parsed;
        std::string defaultName, jcwfName;

        ondemand::parser parser;
        padded_string padded(json);
        ondemand::document doc;
        if (parser.iterate(padded).get(doc) != SUCCESS)
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::ParseFailed, "iterate failed"));
        }

        int64_t version = 1;
        if (doc["version"].get_int64().get(version) == SUCCESS && version != 1)
        {
            return std::unexpected(StoreError::Make(StoreErrorCode::ParseFailed,
                                                    "unsupported version " + std::to_string(version)));
        }

        std::string_view sv;
        if (doc["default_interface"].get_string().get(sv) == SUCCESS) defaultName = sv;
        if (doc["jcwf_interface"].get_string().get(sv) == SUCCESS) jcwfName = sv;

        ondemand::array interfaces;
        if (doc["interfaces"].get_array().get(interfaces) == SUCCESS)
        {
            // Hard cap on entry count.  The file is size-capped (4 MiB) but a document
            // packed with minimal objects could still push tens of thousands of
            // ApiInterface onto the heap inside the unique lock, stalling the dispatch
            // readers.  Mirrors KeyManager's kMaxProviders posture.  Checked at the top
            // of the loop so parse work is bounded too, not just the vector.
            constexpr std::size_t kMaxInterfaces = 1024;
            for (auto element : interfaces)
            {
                if (parsed.size() >= kMaxInterfaces)
                {
                    LOG_CORE_ERROR("ApiInterfaceManager::ParseFromJson: interface count hit the {} cap; "
                                   "ignoring remaining entries",
                                   kMaxInterfaces);
                    break;
                }
                ondemand::object obj;
                if (element.get_object().get(obj) != SUCCESS) continue;
                if (auto iface = ParseInterfaceObject(obj); iface.has_value())
                {
                    parsed.push_back(std::move(*iface));
                }
            }
        }

        m_Interfaces = std::move(parsed);
        m_DefaultInterface = std::move(defaultName);
        m_JcwfInterface = std::move(jcwfName);
        return {};
    }

    // -------------------------------------------------------------------------
    // Load / Save wrappers
    // -------------------------------------------------------------------------

    bool ApiInterfaceManager::Load(std::filesystem::path const& path, std::string_view masterPassword)
    {
        if (auto const result = EncryptedJsonStore::Load(path, masterPassword); !result.has_value())
        {
            LOG_CORE_ERROR("ApiInterfaceManager::Load: {} — {}", Describe(result.error().m_Code),
                           result.error().m_Details);
            return false;
        }
        size_t count = 0;
        {
            std::shared_lock lock(m_Mutex);
            count = m_Interfaces.size();
        }
        LOG_CORE_INFO("ApiInterfaceManager::Load: loaded {} interface(s) from '{}'", count, path.string());
        return true;
    }

    bool ApiInterfaceManager::Save(std::filesystem::path const& path, std::string_view masterPassword)
    {
        if (auto const result = EncryptedJsonStore::Save(path, masterPassword); !result.has_value())
        {
            LOG_CORE_ERROR("ApiInterfaceManager::Save: {} — {}", Describe(result.error().m_Code),
                           result.error().m_Details);
            return false;
        }
        LOG_CORE_INFO("ApiInterfaceManager::Save: saved to '{}'", path.string());
        return true;
    }

    // -------------------------------------------------------------------------
    // Snapshots
    // -------------------------------------------------------------------------

    std::vector<ApiInterface> ApiInterfaceManager::GetInterfaces() const
    {
        std::shared_lock lock(m_Mutex);
        return m_Interfaces;
    }

    std::string ApiInterfaceManager::GetDefaultInterfaceName() const
    {
        std::shared_lock lock(m_Mutex);
        return m_DefaultInterface;
    }

    std::string ApiInterfaceManager::GetJcwfInterfaceName() const
    {
        std::shared_lock lock(m_Mutex);
        return m_JcwfInterface;
    }

    bool ApiInterfaceManager::HasInterface(std::string const& name) const
    {
        std::shared_lock lock(m_Mutex);
        return FindInterfaceIndexByName(m_Interfaces, name).has_value();
    }

    // -------------------------------------------------------------------------
    // Mutations
    // -------------------------------------------------------------------------

    bool ApiInterfaceManager::UpsertInterface(ApiInterface iface, std::string& error)
    {
        if (auto const err = ValidateAndNormalize(iface); err.has_value())
        {
            error = *err;
            return false;
        }
        std::unique_lock lock(m_Mutex);
        if (auto const idx = FindInterfaceIndexByName(m_Interfaces, iface.m_Name); idx.has_value())
        {
            m_Interfaces[*idx] = std::move(iface);
        }
        else
        {
            m_Interfaces.push_back(std::move(iface));
        }
        return true;
    }

    bool ApiInterfaceManager::RemoveInterface(std::string const& name, std::string& error)
    {
        std::unique_lock lock(m_Mutex);
        auto const idx = FindInterfaceIndexByName(m_Interfaces, name);
        if (!idx.has_value())
        {
            error = "no interface named '" + name + "'";
            return false;
        }
        m_Interfaces.erase(m_Interfaces.begin() + static_cast<std::ptrdiff_t>(*idx));
        // A dangling default/jcwf selector silently falls back to "first/none";
        // clear it so the stored state stays consistent.
        if (m_DefaultInterface == name) m_DefaultInterface.clear();
        if (m_JcwfInterface == name) m_JcwfInterface.clear();
        return true;
    }

    bool ApiInterfaceManager::SetDefaultInterface(std::string const& name, std::string& error)
    {
        std::unique_lock lock(m_Mutex);
        if (!FindInterfaceIndexByName(m_Interfaces, name).has_value())
        {
            error = "no interface named '" + name + "'";
            return false;
        }
        m_DefaultInterface = name;
        return true;
    }

    bool ApiInterfaceManager::SetJcwfInterface(std::string const& name, std::string& error)
    {
        std::unique_lock lock(m_Mutex);
        if (!name.empty() && !FindInterfaceIndexByName(m_Interfaces, name).has_value())
        {
            error = "no interface named '" + name + "'";
            return false;
        }
        m_JcwfInterface = name; // empty clears (use default)
        return true;
    }

    std::optional<size_t> ApiInterfaceManager::FindInterfaceIndexByName(
        std::vector<ApiInterface> const& interfaces, std::string_view name)
    {
        for (size_t i = 0; i < interfaces.size(); ++i)
        {
            if (interfaces[i].m_Name == name) return i;
        }
        return std::nullopt;
    }
} // namespace AIAssistant
