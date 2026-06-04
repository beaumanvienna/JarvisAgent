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

#include "json/schemaValidator.h"

#include <cmath>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "engine.h"

// simdjson convention used throughout this file:
//   `auto err = element["key"].get(out)` returns a simdjson error_code where
//   0 == success and non-zero == failure.  The pattern `if (!element[k].get(out))`
//   therefore enters the block ON SUCCESS — the `!` inverts the success-as-zero
//   convention into a boolean truth.  Every such site below follows this
//   convention; do not refactor any of them to `if (element[k].get(out))`
//   without inverting the body, that flip silently skips the validation.

namespace AIAssistant
{
    namespace
    {
        [[nodiscard]] bool IsIntegerValue(simdjson::dom::element const& element)
        {
            if (element.is<int64_t>() || element.is<uint64_t>())
            {
                return true;
            }
            if (element.is<double>())
            {
                double const value = element.get<double>().value_unsafe();
                return std::floor(value) == value && std::isfinite(value);
            }
            return false;
        }

        [[nodiscard]] std::string ElementTypeName(simdjson::dom::element const& element)
        {
            using namespace simdjson;
            // simdjson::dom::element_type is a non-owned third-party enum; the
            // project's no-`default:`-over-closed-enums rule applies only to
            // enums we own.  -Wswitch is on, so adding a new simdjson variant
            // will surface as a compile warning that points here.  The trailing
            // "unknown" return is the safety fallback.
            switch (element.type())
            {
                case dom::element_type::NULL_VALUE: return "null";
                case dom::element_type::BOOL:        return "boolean";
                case dom::element_type::INT64:       return "integer";
                case dom::element_type::UINT64:      return "integer";
                case dom::element_type::DOUBLE:      return "number";
                case dom::element_type::STRING:      return "string";
                case dom::element_type::OBJECT:      return "object";
                case dom::element_type::ARRAY:       return "array";
            }
            return "unknown";
        }

        [[nodiscard]] std::string ElementToString(simdjson::dom::element const& element)
        {
            std::ostringstream stream;
            stream << element;
            return stream.str();
        }
    } // anonymous namespace

    class SchemaValidator::Impl
    {
    public:
        explicit Impl(std::string schemaJson) : m_SchemaJson(std::move(schemaJson))
        {
            using namespace simdjson;
            auto parseResult = m_Parser.parse(m_SchemaJson);
            if (parseResult.error())
            {
                m_LoadError = std::string("schema parse failed: ") + error_message(parseResult.error());
                return;
            }
            m_SchemaRoot = parseResult.value();

            std::string unsupportedError;
            if (!CheckSupportedKeywords(m_SchemaRoot, "", unsupportedError))
            {
                m_LoadError = unsupportedError;
                return;
            }

            dom::element defsElement;
            if (!m_SchemaRoot["$defs"].get(defsElement))
            {
                if (defsElement.is_object())
                {
                    for (auto field : defsElement.get_object())
                    {
                        m_Defs[std::string(field.key)] = field.value;
                    }
                }
            }

            m_IsLoaded = true;
        }

        bool IsLoaded() const { return m_IsLoaded; }
        std::string const& LoadError() const { return m_LoadError; }

        ValidationResult Validate(std::string const& documentJson) const
        {
            using namespace simdjson;
            ValidationResult result;
            result.m_Ok = false;

            dom::parser parser;
            auto parseResult = parser.parse(documentJson);
            if (parseResult.error())
            {
                result.m_Errors.push_back(ValidationError{
                    "", std::string("document parse failed: ") + error_message(parseResult.error())});
                return result;
            }

            std::vector<ValidationError> errors;
            ValidateAgainst(m_SchemaRoot, parseResult.value(), "", errors);
            if (errors.empty())
            {
                result.m_Ok = true;
            }
            else
            {
                result.m_Errors = std::move(errors);
            }
            return result;
        }

    private:
        // Walks the schema recursively, validating supported-keyword usage
        // AND pre-compiling every `pattern` regex into m_RegexCache.  Doing
        // both here means: (a) load-time rejects schemas using rejected
        // keywords (early failure with `outError` populated), (b) a malformed
        // regex breaks the load, never a per-validate call, and (c) Validate
        // does an O(1) cache lookup instead of an O(N) regex compile per
        // string-constraint check.  Drops `const` because m_RegexCache is
        // populated here.
        bool CheckSupportedKeywords(simdjson::dom::element schema, std::string const& pointer,
                                     std::string& outError)
        {
            using namespace simdjson;
            static std::unordered_set<std::string> const kSupported = {
                "type", "properties", "required", "additionalProperties",
                "items", "enum", "minimum", "maximum", "minLength", "maxLength",
                "pattern", "oneOf", "anyOf", "$ref", "$defs", "description",
                "title", "examples", "default", "$schema", "$id", "$comment"};
            static std::unordered_set<std::string> const kRejected = {
                "allOf", "not", "format", "if", "then", "else", "dependencies",
                "patternProperties", "contains", "minContains", "maxContains",
                "uniqueItems", "minItems", "maxItems", "multipleOf", "const",
                "propertyNames", "readOnly", "writeOnly"};

            if (!schema.is_object())
            {
                return true;
            }
            for (auto field : schema.get_object())
            {
                std::string const key(field.key);
                if (kRejected.count(key) > 0)
                {
                    outError = "schema uses unsupported keyword '" + key + "' at " + pointer +
                                " (Draft 2020-12 subset supports only: type, properties, required, items, enum, "
                                "minimum/maximum, minLength/maxLength, pattern, oneOf, anyOf, $ref, $defs)";
                    return false;
                }
                if (kSupported.count(key) == 0)
                {
                    LOG_APP_INFO("SchemaValidator: unknown schema keyword '{}' at {} — ignored", key, pointer);
                }
                if (key == "pattern" && field.value.is_string())
                {
                    std::string_view patternView;
                    if (!field.value.get(patternView))
                    {
                        std::string const patternStr(patternView);
                        if (m_RegexCache.find(patternStr) == m_RegexCache.end())
                        {
                            try
                            {
                                m_RegexCache.emplace(patternStr, std::regex(patternStr));
                            }
                            catch (std::regex_error const& e)
                            {
                                outError = "invalid regex at " + pointer + "/pattern: " + e.what();
                                return false;
                            }
                            catch (std::bad_alloc const&)
                            {
                                outError = "regex compile out-of-memory at " + pointer + "/pattern";
                                return false;
                            }
                        }
                    }
                }
                if (key == "properties" && field.value.is_object())
                {
                    for (auto prop : field.value.get_object())
                    {
                        std::string const childPointer = pointer + "/properties/" + std::string(prop.key);
                        if (!CheckSupportedKeywords(prop.value, childPointer, outError))
                        {
                            return false;
                        }
                    }
                }
                else if (key == "items")
                {
                    if (!CheckSupportedKeywords(field.value, pointer + "/items", outError))
                    {
                        return false;
                    }
                }
                else if ((key == "oneOf" || key == "anyOf") && field.value.is_array())
                {
                    size_t index = 0;
                    for (auto const sub : field.value.get_array())
                    {
                        std::string const childPointer = pointer + "/" + key + "/" + std::to_string(index++);
                        if (!CheckSupportedKeywords(sub, childPointer, outError))
                        {
                            return false;
                        }
                    }
                }
                else if (key == "$defs" && field.value.is_object())
                {
                    for (auto def : field.value.get_object())
                    {
                        std::string const childPointer = pointer + "/$defs/" + std::string(def.key);
                        if (!CheckSupportedKeywords(def.value, childPointer, outError))
                        {
                            return false;
                        }
                    }
                }
            }
            return true;
        }

        // Resolves a `#/$defs/<key>` reference.  Returns std::nullopt when the
        // ref is malformed or the key is absent — emulates Rust's Option<T>
        // and forces the caller to handle the absent case explicitly, instead
        // of relying on a default-constructed simdjson element whose `.type()`
        // is implementation-defined.
        [[nodiscard]] std::optional<simdjson::dom::element> ResolveRef(std::string const& ref) const
        {
            std::string const prefix = "#/$defs/";
            if (ref.rfind(prefix, 0) != 0)
            {
                return std::nullopt;
            }
            std::string const key = ref.substr(prefix.size());
            auto const iterator = m_Defs.find(key);
            if (iterator == m_Defs.end())
            {
                return std::nullopt;
            }
            return iterator->second;
        }

        void ValidateAgainst(simdjson::dom::element schema, simdjson::dom::element value, std::string const& pointer,
                              std::vector<ValidationError>& errors) const
        {
            using namespace simdjson;
            if (!schema.is_object())
            {
                return;
            }

            dom::element refElement;
            if (!schema["$ref"].get(refElement))
            {
                std::string_view refView;
                if (!refElement.get(refView))
                {
                    if (auto const resolved = ResolveRef(std::string(refView)); resolved.has_value())
                    {
                        ValidateAgainst(*resolved, value, pointer, errors);
                        return;
                    }
                    errors.push_back({pointer, "unresolved $ref: " + std::string(refView)});
                    return;
                }
            }

            dom::element typeElement;
            if (!schema["type"].get(typeElement))
            {
                bool typeMatched = false;
                std::string expectedTypes;
                auto checkType = [&](std::string_view expectedType)
                {
                    if (!expectedTypes.empty()) expectedTypes += "|";
                    expectedTypes += expectedType;
                    std::string const actual = ElementTypeName(value);
                    if (expectedType == "integer")
                    {
                        if (IsIntegerValue(value))
                        {
                            typeMatched = true;
                        }
                    }
                    else if (actual == expectedType)
                    {
                        typeMatched = true;
                    }
                    else if (expectedType == "number" && (actual == "integer" || actual == "number"))
                    {
                        typeMatched = true;
                    }
                };
                std::string_view typeView;
                if (!typeElement.get(typeView))
                {
                    checkType(typeView);
                }
                else if (typeElement.is_array())
                {
                    for (auto elem : typeElement.get_array())
                    {
                        std::string_view subView;
                        if (!elem.get(subView))
                        {
                            checkType(subView);
                            if (typeMatched) break;
                        }
                    }
                }
                if (!typeMatched)
                {
                    errors.push_back({pointer, "type mismatch: expected " + expectedTypes +
                                               ", got " + ElementTypeName(value)});
                    return;
                }
            }

            dom::element enumElement;
            if (!schema["enum"].get(enumElement) && enumElement.is_array())
            {
                bool enumMatched = false;
                std::string valueStr = ElementToString(value);
                for (auto const candidate : enumElement.get_array())
                {
                    if (ElementToString(candidate) == valueStr)
                    {
                        enumMatched = true;
                        break;
                    }
                }
                if (!enumMatched)
                {
                    errors.push_back({pointer, "value not in enum: " + valueStr});
                }
            }

            ValidateStringConstraints(schema, value, pointer, errors);
            ValidateNumberConstraints(schema, value, pointer, errors);
            ValidateObjectConstraints(schema, value, pointer, errors);
            ValidateArrayConstraints(schema, value, pointer, errors);

            dom::element anyOfElement;
            if (!schema["anyOf"].get(anyOfElement) && anyOfElement.is_array())
            {
                bool anyMatched = false;
                std::vector<ValidationError> lastSubErrors;
                for (auto subSchema : anyOfElement.get_array())
                {
                    std::vector<ValidationError> subErrors;
                    ValidateAgainst(subSchema, value, pointer, subErrors);
                    if (subErrors.empty())
                    {
                        anyMatched = true;
                        break;
                    }
                    lastSubErrors = std::move(subErrors);
                }
                if (!anyMatched)
                {
                    errors.push_back({pointer, "anyOf: no branch matched"});
                    errors.insert(errors.end(),
                                  std::make_move_iterator(lastSubErrors.begin()),
                                  std::make_move_iterator(lastSubErrors.end()));
                }
            }

            dom::element oneOfElement;
            if (!schema["oneOf"].get(oneOfElement) && oneOfElement.is_array())
            {
                int matchCount = 0;
                for (auto subSchema : oneOfElement.get_array())
                {
                    std::vector<ValidationError> subErrors;
                    ValidateAgainst(subSchema, value, pointer, subErrors);
                    if (subErrors.empty())
                    {
                        ++matchCount;
                    }
                }
                if (matchCount == 0)
                {
                    errors.push_back({pointer, "oneOf: no branch matched"});
                }
                else if (matchCount > 1)
                {
                    errors.push_back({pointer, "oneOf: more than one branch matched (" +
                                               std::to_string(matchCount) + ")"});
                }
            }
        }

        void ValidateStringConstraints(simdjson::dom::element schema, simdjson::dom::element value,
                                        std::string const& pointer, std::vector<ValidationError>& errors) const
        {
            using namespace simdjson;
            std::string_view stringView;
            if (value.get(stringView))
            {
                return;
            }
            uint64_t minLength = 0;
            if (!schema["minLength"].get(minLength))
            {
                if (stringView.size() < minLength)
                {
                    errors.push_back({pointer, "string shorter than minLength " + std::to_string(minLength)});
                }
            }
            uint64_t maxLength = 0;
            if (!schema["maxLength"].get(maxLength))
            {
                if (stringView.size() > maxLength)
                {
                    errors.push_back({pointer, "string longer than maxLength " + std::to_string(maxLength)});
                }
            }
            std::string_view patternView;
            if (!schema["pattern"].get(patternView))
            {
                std::string const patternStr(patternView);
                auto const it = m_RegexCache.find(patternStr);
                if (it == m_RegexCache.end())
                {
                    // Pre-compilation in CheckSupportedKeywords should have
                    // populated every pattern at load time.  A miss here means
                    // the schema was mutated after load or load skipped a
                    // branch — either is a load-side bug, surface it.
                    errors.push_back({pointer, "internal: pattern '" + patternStr + "' not pre-compiled"});
                    return;
                }
                std::string const stringCopy(stringView);
                if (!std::regex_search(stringCopy, it->second))
                {
                    errors.push_back({pointer, "string does not match pattern '" + patternStr + "'"});
                }
            }
        }

        void ValidateNumberConstraints(simdjson::dom::element schema, simdjson::dom::element value,
                                        std::string const& pointer, std::vector<ValidationError>& errors) const
        {
            using namespace simdjson;
            // Explicit switch over the three numeric variants — replaces a
            // ternary chain whose exhaustiveness was not obvious at a glance.
            // The value_unsafe() calls are safe because the type is confirmed
            // by the case arm.
            double numberValue = 0.0;
            switch (value.type())
            {
                case dom::element_type::DOUBLE:
                    numberValue = value.get<double>().value_unsafe();
                    break;
                case dom::element_type::INT64:
                    numberValue = static_cast<double>(value.get<int64_t>().value_unsafe());
                    break;
                case dom::element_type::UINT64:
                    numberValue = static_cast<double>(value.get<uint64_t>().value_unsafe());
                    break;
                default:
                    return;
            }
            double minimum = 0;
            if (!schema["minimum"].get(minimum))
            {
                if (numberValue < minimum)
                {
                    errors.push_back({pointer, "value " + std::to_string(numberValue) +
                                               " below minimum " + std::to_string(minimum)});
                }
            }
            double maximum = 0;
            if (!schema["maximum"].get(maximum))
            {
                if (numberValue > maximum)
                {
                    errors.push_back({pointer, "value " + std::to_string(numberValue) +
                                               " above maximum " + std::to_string(maximum)});
                }
            }
        }

        void ValidateObjectConstraints(simdjson::dom::element schema, simdjson::dom::element value,
                                        std::string const& pointer, std::vector<ValidationError>& errors) const
        {
            using namespace simdjson;
            if (!value.is_object())
            {
                return;
            }

            dom::element requiredElement;
            if (!schema["required"].get(requiredElement) && requiredElement.is_array())
            {
                for (auto required : requiredElement.get_array())
                {
                    std::string_view requiredName;
                    if (required.get(requiredName))
                    {
                        continue;
                    }
                    dom::element found;
                    auto const error = value[requiredName].get(found);
                    if (error)
                    {
                        errors.push_back({pointer, "missing required property '" + std::string(requiredName) + "'"});
                    }
                }
            }

            dom::element propertiesElement;
            bool const hasProperties = !schema["properties"].get(propertiesElement) && propertiesElement.is_object();

            dom::element additionalPropertiesElement;
            bool additionalAllowed = true;
            dom::element additionalSchema;
            if (!schema["additionalProperties"].get(additionalPropertiesElement))
            {
                if (additionalPropertiesElement.is_bool())
                {
                    additionalAllowed = additionalPropertiesElement.get<bool>().value();
                }
                else if (additionalPropertiesElement.is_object())
                {
                    additionalSchema = additionalPropertiesElement;
                }
            }

            for (auto field : value.get_object())
            {
                std::string const fieldName(field.key);
                std::string const childPointer = pointer + "/" + fieldName;
                dom::element propertySchema;
                bool matchedProperty = false;
                if (hasProperties)
                {
                    if (!propertiesElement[fieldName].get(propertySchema))
                    {
                        matchedProperty = true;
                        ValidateAgainst(propertySchema, field.value, childPointer, errors);
                    }
                }
                if (!matchedProperty)
                {
                    if (!additionalAllowed)
                    {
                        errors.push_back({childPointer, "additional property '" + fieldName + "' not allowed"});
                    }
                    else if (additionalSchema.type() != dom::element_type::NULL_VALUE &&
                             additionalSchema.is_object())
                    {
                        ValidateAgainst(additionalSchema, field.value, childPointer, errors);
                    }
                }
            }
        }

        void ValidateArrayConstraints(simdjson::dom::element schema, simdjson::dom::element value,
                                       std::string const& pointer, std::vector<ValidationError>& errors) const
        {
            using namespace simdjson;
            if (!value.is_array())
            {
                return;
            }
            dom::element itemsSchema;
            if (schema["items"].get(itemsSchema))
            {
                return;
            }
            size_t index = 0;
            for (auto const elem : value.get_array())
            {
                std::string const childPointer = pointer + "/" + std::to_string(index++);
                ValidateAgainst(itemsSchema, elem, childPointer, errors);
            }
        }

    private:
        // Lifetime invariant — m_Parser owns the simdjson arena into which
        // m_SchemaRoot and every value in m_Defs hold pointers.  Members are
        // destroyed in reverse declaration order, so m_Parser MUST be declared
        // before m_SchemaRoot and m_Defs (it is below) — that way m_Parser is
        // destroyed LAST and the borrowing elements are torn down first.
        // Adding new simdjson::dom::element members below this line is fine;
        // do not move m_Parser later in the declaration list.
        std::string m_SchemaJson;
        simdjson::dom::parser m_Parser;
        simdjson::dom::element m_SchemaRoot;
        std::unordered_map<std::string, simdjson::dom::element> m_Defs;
        // Pre-compiled patterns from CheckSupportedKeywords.  Read-only after
        // load — no synchronisation needed for concurrent Validate() calls.
        std::unordered_map<std::string, std::regex> m_RegexCache;
        bool m_IsLoaded = false;
        std::string m_LoadError;
    };

    SchemaValidator::SchemaValidator(std::string schemaJson)
        : m_Impl(std::make_unique<Impl>(std::move(schemaJson)))
    {
    }

    SchemaValidator::~SchemaValidator() = default;

    bool SchemaValidator::IsLoaded() const { return m_Impl->IsLoaded(); }

    std::string const& SchemaValidator::LoadError() const { return m_Impl->LoadError(); }

    ValidationResult SchemaValidator::Validate(std::string const& documentJson) const
    {
        if (!m_Impl->IsLoaded())
        {
            ValidationResult result;
            result.m_Ok = false;
            result.m_Errors.push_back({"", "schema not loaded: " + m_Impl->LoadError()});
            return result;
        }
        return m_Impl->Validate(documentJson);
    }

    std::string SchemaValidator::FormatErrorsForModel(std::vector<ValidationError> const& errors)
    {
        std::string formatted;
        for (auto const& error : errors)
        {
            if (!formatted.empty()) formatted += "\n";
            formatted += "  - at '" + (error.m_Path.empty() ? std::string("/") : error.m_Path) + "': " + error.m_Message;
        }
        return formatted;
    }
} // namespace AIAssistant
