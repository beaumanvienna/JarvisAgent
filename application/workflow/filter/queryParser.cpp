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

#include "workflow/filter/queryParser.h"

#include <cctype>

namespace AIAssistant
{

    // =================================================================
    // Tokenizer
    // =================================================================

    static QueryParser::Token const& EofToken()
    {
        static QueryParser::Token const eof{QueryParser::TokenType::Eof, ""};
        return eof;
    }

    QueryParser::Token const& QueryParser::ParserState::Current() const
    {
        if (m_Pos < m_Tokens->size())
        {
            return (*m_Tokens)[m_Pos];
        }
        return EofToken();
    }

    QueryParser::Token const& QueryParser::ParserState::Peek(size_t offset) const
    {
        size_t idx = m_Pos + offset;
        if (idx < m_Tokens->size())
        {
            return (*m_Tokens)[idx];
        }
        return EofToken();
    }

    void QueryParser::ParserState::Advance()
    {
        if (m_Pos < m_Tokens->size())
        {
            ++m_Pos;
        }
    }

    bool QueryParser::ParserState::AtEnd() const { return m_Pos >= m_Tokens->size(); }

    std::vector<QueryParser::Token> QueryParser::Tokenize(std::string const& input, std::string& errorMessage)
    {
        std::vector<Token> tokens;
        size_t i = 0;

        while (i < input.size())
        {
            char c = input[i];

            // Skip whitespace
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                ++i;
                continue;
            }

            // Single-character tokens
            if (c == '(')
            {
                tokens.push_back({TokenType::LParen, "("});
                ++i;
                continue;
            }
            if (c == ')')
            {
                tokens.push_back({TokenType::RParen, ")"});
                ++i;
                continue;
            }
            if (c == '[')
            {
                tokens.push_back({TokenType::LBracket, "["});
                ++i;
                continue;
            }
            if (c == ']')
            {
                tokens.push_back({TokenType::RBracket, "]"});
                ++i;
                continue;
            }
            if (c == '{')
            {
                tokens.push_back({TokenType::LBrace, "{"});
                ++i;
                continue;
            }
            if (c == '}')
            {
                tokens.push_back({TokenType::RBrace, "}"});
                ++i;
                continue;
            }
            if (c == ':')
            {
                tokens.push_back({TokenType::Colon, ":"});
                ++i;
                continue;
            }

            // Minus (prefix negation): only if followed by a word character (not standalone)
            if (c == '-' && i + 1 < input.size() && !std::isspace(static_cast<unsigned char>(input[i + 1])))
            {
                tokens.push_back({TokenType::Minus, "-"});
                ++i;
                continue;
            }

            // Quoted string
            if (c == '"')
            {
                ++i;
                std::string text;
                while (i < input.size() && input[i] != '"')
                {
                    if (input[i] == '\\' && i + 1 < input.size())
                    {
                        text += input[i + 1];
                        i += 2;
                    }
                    else
                    {
                        text += input[i];
                        ++i;
                    }
                }

                if (i >= input.size())
                {
                    errorMessage = "unterminated quoted string in query";
                    return {};
                }

                ++i; // skip closing quote
                tokens.push_back({TokenType::Quoted, text});
                continue;
            }

            // Word token (field names, values, keywords)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '*' || c == '.')
            {
                std::string text;
                while (i < input.size())
                {
                    char ch = input[i];
                    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '*' || ch == '.' || ch == '-')
                    {
                        text += ch;
                        ++i;
                    }
                    else
                    {
                        break;
                    }
                }

                // Classify keywords
                if (text == "AND")
                {
                    tokens.push_back({TokenType::And, text});
                }
                else if (text == "OR")
                {
                    tokens.push_back({TokenType::Or, text});
                }
                else if (text == "NOT")
                {
                    tokens.push_back({TokenType::Not, text});
                }
                else if (text == "TO")
                {
                    tokens.push_back({TokenType::To, text});
                }
                else
                {
                    tokens.push_back({TokenType::Word, text});
                }
                continue;
            }

            // Unknown character — skip with warning
            ++i;
        }

        return tokens;
    }

    // =================================================================
    // Parser — public API
    // =================================================================

    std::unique_ptr<QueryNode> QueryParser::Parse(std::string const& queryString, std::string& errorMessage) const
    {
        if (queryString.empty())
        {
            errorMessage = "empty query string";
            return nullptr;
        }

        auto tokens = Tokenize(queryString, errorMessage);
        if (!errorMessage.empty())
        {
            return nullptr;
        }

        if (tokens.empty())
        {
            errorMessage = "query produced no tokens";
            return nullptr;
        }

        ParserState state;
        state.m_Tokens = &tokens;
        state.m_Pos = 0;

        auto node = ParseOrExpr(state, errorMessage);
        if (!node)
        {
            return nullptr;
        }

        if (!state.AtEnd())
        {
            errorMessage = "unexpected token after query: '" + state.Current().m_Text + "'";
            return nullptr;
        }

        return node;
    }

    // =================================================================
    // Recursive descent
    // =================================================================

    // orExpr := andExpr ("OR" andExpr)*
    std::unique_ptr<QueryNode> QueryParser::ParseOrExpr(ParserState& state, std::string& errorMessage)
    {
        auto left = ParseAndExpr(state, errorMessage);
        if (!left)
        {
            return nullptr;
        }

        if (state.Current().m_Type == TokenType::Or)
        {
            auto orNode = std::make_unique<QueryNode>();
            orNode->m_Type = QueryNodeType::Or;
            orNode->m_Children.push_back(std::move(left));

            while (state.Current().m_Type == TokenType::Or)
            {
                state.Advance(); // consume OR
                auto right = ParseAndExpr(state, errorMessage);
                if (!right)
                {
                    return nullptr;
                }
                orNode->m_Children.push_back(std::move(right));
            }

            return orNode;
        }

        return left;
    }

    // andExpr := unaryExpr ("AND" unaryExpr)*
    std::unique_ptr<QueryNode> QueryParser::ParseAndExpr(ParserState& state, std::string& errorMessage)
    {
        auto left = ParseUnaryExpr(state, errorMessage);
        if (!left)
        {
            return nullptr;
        }

        if (state.Current().m_Type == TokenType::And)
        {
            auto andNode = std::make_unique<QueryNode>();
            andNode->m_Type = QueryNodeType::And;
            andNode->m_Children.push_back(std::move(left));

            while (state.Current().m_Type == TokenType::And)
            {
                state.Advance(); // consume AND
                auto right = ParseUnaryExpr(state, errorMessage);
                if (!right)
                {
                    return nullptr;
                }
                andNode->m_Children.push_back(std::move(right));
            }

            return andNode;
        }

        return left;
    }

    // unaryExpr := "NOT" unaryExpr | "-" fieldExpr | "(" query ")" | fieldExpr
    std::unique_ptr<QueryNode> QueryParser::ParseUnaryExpr(ParserState& state, std::string& errorMessage)
    {
        // NOT prefix
        if (state.Current().m_Type == TokenType::Not)
        {
            state.Advance(); // consume NOT
            auto child = ParseUnaryExpr(state, errorMessage);
            if (!child)
            {
                return nullptr;
            }

            auto notNode = std::make_unique<QueryNode>();
            notNode->m_Type = QueryNodeType::Not;
            notNode->m_Child = std::move(child);
            return notNode;
        }

        // - prefix (shorthand for NOT)
        if (state.Current().m_Type == TokenType::Minus)
        {
            state.Advance(); // consume -
            auto child = ParseFieldExpr(state, errorMessage);
            if (!child)
            {
                return nullptr;
            }

            auto notNode = std::make_unique<QueryNode>();
            notNode->m_Type = QueryNodeType::Not;
            notNode->m_Child = std::move(child);
            return notNode;
        }

        // Grouped expression
        if (state.Current().m_Type == TokenType::LParen)
        {
            state.Advance(); // consume (
            auto inner = ParseOrExpr(state, errorMessage);
            if (!inner)
            {
                return nullptr;
            }

            if (state.Current().m_Type != TokenType::RParen)
            {
                errorMessage = "expected ')' after grouped expression";
                return nullptr;
            }

            state.Advance(); // consume )
            return inner;
        }

        return ParseFieldExpr(state, errorMessage);
    }

    // fieldExpr := FIELD ":" ( rangeExpr | value )
    std::unique_ptr<QueryNode> QueryParser::ParseFieldExpr(ParserState& state, std::string& errorMessage)
    {
        if (state.Current().m_Type != TokenType::Word && state.Current().m_Type != TokenType::Quoted)
        {
            errorMessage = "expected field name, got '" + state.Current().m_Text + "'";
            return nullptr;
        }

        std::string field = state.Current().m_Text;
        state.Advance(); // consume field name

        if (state.Current().m_Type != TokenType::Colon)
        {
            errorMessage = "expected ':' after field '" + field + "'";
            return nullptr;
        }

        state.Advance(); // consume :

        // Check for range expression: [ or {
        if (state.Current().m_Type == TokenType::LBracket)
        {
            state.Advance(); // consume [
            return ParseRangeExpr(state, field, true, errorMessage);
        }

        if (state.Current().m_Type == TokenType::LBrace)
        {
            state.Advance(); // consume {
            return ParseRangeExpr(state, field, false, errorMessage);
        }

        // Simple value (word or quoted)
        if (state.Current().m_Type != TokenType::Word && state.Current().m_Type != TokenType::Quoted)
        {
            errorMessage = "expected value after '" + field + ":'";
            return nullptr;
        }

        auto node = std::make_unique<QueryNode>();
        node->m_Type = QueryNodeType::FieldMatch;
        node->m_Field = field;
        node->m_Value = state.Current().m_Text;

        // Check wildcard: value ending with *
        if (!node->m_Value.empty() && node->m_Value.back() == '*')
        {
            node->m_IsWildcard = true;
            node->m_Value.pop_back(); // store prefix only
        }

        state.Advance(); // consume value
        return node;
    }

    // rangeExpr := value "TO" value ( "]" | "}" )
    std::unique_ptr<QueryNode> QueryParser::ParseRangeExpr(ParserState& state, std::string const& field, bool loInclusive,
                                                           std::string& errorMessage)
    {
        // Low value
        if (state.Current().m_Type != TokenType::Word && state.Current().m_Type != TokenType::Quoted)
        {
            errorMessage = "expected range low value for field '" + field + "'";
            return nullptr;
        }

        std::string lo = state.Current().m_Text;
        state.Advance();

        // TO
        if (state.Current().m_Type != TokenType::To)
        {
            errorMessage = "expected 'TO' in range expression for field '" + field + "'";
            return nullptr;
        }
        state.Advance();

        // High value
        if (state.Current().m_Type != TokenType::Word && state.Current().m_Type != TokenType::Quoted)
        {
            errorMessage = "expected range high value for field '" + field + "'";
            return nullptr;
        }

        std::string hi = state.Current().m_Text;
        state.Advance();

        // Closing bracket
        bool hiInclusive = true;
        if (state.Current().m_Type == TokenType::RBracket)
        {
            hiInclusive = true;
            state.Advance();
        }
        else if (state.Current().m_Type == TokenType::RBrace)
        {
            hiInclusive = false;
            state.Advance();
        }
        else
        {
            errorMessage = "expected ']' or '}' to close range for field '" + field + "'";
            return nullptr;
        }

        auto node = std::make_unique<QueryNode>();
        node->m_Type = QueryNodeType::FieldRange;
        node->m_Field = field;
        node->m_RangeLo = lo;
        node->m_RangeHi = hi;
        node->m_RangeLoInclusive = loInclusive;
        node->m_RangeHiInclusive = hiInclusive;
        return node;
    }

    // =================================================================
    // Evaluation
    // =================================================================

    bool QueryParser::Matches(QueryNode const& node, QueryDocument const& doc)
    {
        switch (node.m_Type)
        {
            case QueryNodeType::And:
            {
                for (auto const& child : node.m_Children)
                {
                    if (!Matches(*child, doc))
                    {
                        return false;
                    }
                }
                return true;
            }

            case QueryNodeType::Or:
            {
                for (auto const& child : node.m_Children)
                {
                    if (Matches(*child, doc))
                    {
                        return true;
                    }
                }
                return false;
            }

            case QueryNodeType::Not:
            {
                return node.m_Child && !Matches(*node.m_Child, doc);
            }

            case QueryNodeType::FieldMatch:
                return MatchField(node, doc);

            case QueryNodeType::FieldRange:
                return MatchRange(node, doc);
        }

        return false;
    }

    bool QueryParser::MatchField(QueryNode const& node, QueryDocument const& doc)
    {
        auto it = doc.find(node.m_Field);
        if (it == doc.end())
        {
            return false;
        }

        std::string const& docValue = it->second;

        if (node.m_IsWildcard)
        {
            // Prefix match
            return docValue.size() >= node.m_Value.size() && docValue.compare(0, node.m_Value.size(), node.m_Value) == 0;
        }

        // Exact match
        return docValue == node.m_Value;
    }

    bool QueryParser::MatchRange(QueryNode const& node, QueryDocument const& doc)
    {
        auto it = doc.find(node.m_Field);
        if (it == doc.end())
        {
            return false;
        }

        std::string const& docValue = it->second;
        int const cmpLo = CompareValues(docValue, node.m_RangeLo);
        int const cmpHi = CompareValues(docValue, node.m_RangeHi);

        bool loOk = node.m_RangeLoInclusive ? (cmpLo >= 0) : (cmpLo > 0);
        bool hiOk = node.m_RangeHiInclusive ? (cmpHi <= 0) : (cmpHi < 0);

        return loOk && hiOk;
    }

    // Compare two values: tries numeric first, falls back to lexicographic.
    int QueryParser::CompareValues(std::string const& a, std::string const& b)
    {
        // Try numeric comparison
        try
        {
            double da = std::stod(a);
            double db = std::stod(b);
            if (da < db)
            {
                return -1;
            }
            if (da > db)
            {
                return 1;
            }
            return 0;
        }
        catch (...)
        {
            // Fall through to lexicographic
        }

        return a.compare(b);
    }

} // namespace AIAssistant
