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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AIAssistant
{

    // -----------------------------------------------------------------
    // AST node types for the Lucene-style query language
    //
    // Grammar:
    //   query     := orExpr
    //   orExpr    := andExpr ("OR" andExpr)*
    //   andExpr   := unaryExpr ("AND" unaryExpr)*
    //   unaryExpr := "NOT" unaryExpr | "(" query ")" | fieldExpr
    //   fieldExpr := FIELD ":" ( rangeExpr | value )
    //   rangeExpr := "[" value "TO" value "]" | "{" value "TO" value "}"
    //   value     := WORD | QUOTED_STRING
    //
    //   Wildcard: a value ending with '*' is a prefix match.
    //   Negation: "-field:value" is shorthand for "NOT field:value".
    // -----------------------------------------------------------------

    enum class QueryNodeType
    {
        And,
        Or,
        Not,
        FieldMatch, // field:value (exact or wildcard)
        FieldRange  // field:[lo TO hi] or field:{lo TO hi}
    };

    struct QueryNode
    {
        QueryNodeType m_Type{QueryNodeType::FieldMatch};

        // For And / Or: children
        std::vector<std::unique_ptr<QueryNode>> m_Children;

        // For Not: single child
        std::unique_ptr<QueryNode> m_Child;

        // For FieldMatch
        std::string m_Field;
        std::string m_Value;
        bool m_IsWildcard{false}; // true if m_Value ends with '*' (prefix match)

        // For FieldRange
        std::string m_RangeLo;
        std::string m_RangeHi;
        bool m_RangeLoInclusive{true}; // '[' = inclusive, '{' = exclusive
        bool m_RangeHiInclusive{true}; // ']' = inclusive, '}' = exclusive
    };

    // Document represented as a simple key-value map (field → value).
    using QueryDocument = std::unordered_map<std::string, std::string>;

    // -----------------------------------------------------------------
    // QueryParser: recursive descent parser + evaluator
    // -----------------------------------------------------------------

    class QueryParser
    {
    public:
        // Tokenizer types (public so .cpp helpers can reference them)
        enum class TokenType
        {
            Word,     // unquoted token (field name or value)
            Quoted,   // "quoted string"
            LParen,   // (
            RParen,   // )
            LBracket, // [
            RBracket, // ]
            LBrace,   // {
            RBrace,   // }
            Colon,    // :
            And,      // AND
            Or,       // OR
            Not,      // NOT
            To,       // TO
            Minus,    // - (prefix negation)
            Eof
        };

        struct Token
        {
            TokenType m_Type{TokenType::Eof};
            std::string m_Text;
        };

        // Parse a Lucene-style query string into an AST.
        // Returns nullptr and populates errorMessage on failure.
        std::unique_ptr<QueryNode> Parse(std::string const& queryString, std::string& errorMessage) const;

        // Evaluate a parsed query against a document.
        static bool Matches(QueryNode const& node, QueryDocument const& doc);

        // Parser state (public for .cpp access)
        struct ParserState
        {
            std::vector<Token> const* m_Tokens{nullptr};
            size_t m_Pos{0};

            Token const& Current() const;
            Token const& Peek(size_t offset = 0) const;
            void Advance();
            bool AtEnd() const;
        };

    private:
        static std::vector<Token> Tokenize(std::string const& input, std::string& errorMessage);

        // Recursive descent
        static std::unique_ptr<QueryNode> ParseOrExpr(ParserState& state, std::string& errorMessage);
        static std::unique_ptr<QueryNode> ParseAndExpr(ParserState& state, std::string& errorMessage);
        static std::unique_ptr<QueryNode> ParseUnaryExpr(ParserState& state, std::string& errorMessage);
        static std::unique_ptr<QueryNode> ParseFieldExpr(ParserState& state, std::string& errorMessage);
        static std::unique_ptr<QueryNode> ParseRangeExpr(ParserState& state, std::string const& field, bool loInclusive,
                                                         std::string& errorMessage);

        // Evaluation helpers
        static bool MatchField(QueryNode const& node, QueryDocument const& doc);
        static bool MatchRange(QueryNode const& node, QueryDocument const& doc);
        static int CompareValues(std::string const& a, std::string const& b);
    };

} // namespace AIAssistant
