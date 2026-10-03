#include <Heimdall/Completion.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heimdall
{

namespace
{

constexpr bool IsIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

constexpr bool IsIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

constexpr std::string_view kKeywords[] = {
    "alignas",     "alignof",   "and",      "and_eq",    "asm",       "auto",
    "bitand",      "bitor",     "bool",     "break",     "case",      "catch",
    "char",        "char8_t",   "char16_t", "char32_t",  "class",     "compl",
    "concept",     "const",     "consteval", "constexpr", "constinit", "const_cast",
    "continue",    "co_await",  "co_return", "co_yield",  "decltype",  "default",
    "delete",      "do",        "double",   "dynamic_cast", "else",   "enum",
    "explicit",    "export",    "extern",   "false",     "float",     "for",
    "friend",      "goto",      "if",       "inline",    "int",       "long",
    "mutable",     "namespace", "new",      "noexcept",  "not",       "not_eq",
    "nullptr",     "operator",  "or",       "or_eq",     "private",   "protected",
    "public",      "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof",      "static",    "static_assert", "static_cast", "struct", "switch",
    "template",    "this",      "thread_local", "throw", "true",  "try",
    "typedef",     "typeid",    "typename",  "union",   "unsigned",  "using",
    "virtual",     "void",      "volatile",  "wchar_t", "while",     "xor",
    "xor_eq",      "compl",     "import",    "module",  "co_await",  "co_return",
    "char8_t",     "char16_t",  "consteval", "constinit",
};

constexpr std::string_view kBuiltinTypes[] = {
    "void",     "bool",     "char",     "char8_t", "char16_t", "char32_t",
    "wchar_t",  "short",    "int",      "long",    "signed",   "unsigned",
    "float",    "double",   "auto",     "void",    "typename", "decltype",
};

constexpr std::string_view kDirectives[] = {
    "include", "define", "undef", "if", "ifdef", "ifndef",
    "elif",    "else",   "endif", "pragma", "error", "line",
};

bool IsBuiltinType(std::string_view word)
{
    for (const auto type : kBuiltinTypes)
    {
        if (word == type) return true;
    }
    return false;
}

bool IsKeyword(std::string_view word)
{
    for (const auto keyword : kKeywords)
    {
        if (word == keyword) return true;
    }
    return false;
}

int KindPriority(CompletionKind kind)
{
    switch (kind)
    {
    case CompletionKind::Macro: return 5;
    case CompletionKind::Type: return 4;
    case CompletionKind::Function: return 3;
    case CompletionKind::Variable: return 2;
    case CompletionKind::Directive: return 2;
    case CompletionKind::Keyword: return 1;
    }
    return 0;
}

std::string KindDetail(CompletionKind kind)
{
    switch (kind)
    {
    case CompletionKind::Keyword: return "keyword";
    case CompletionKind::Type: return "type";
    case CompletionKind::Function: return "function";
    case CompletionKind::Variable: return "variable";
    case CompletionKind::Macro: return "macro";
    case CompletionKind::Directive: return "directive";
    }
    return "";
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

enum class CursorContext
{
    Expression,
    MemberAccess,
    ScopeAccess,
    Preprocessor,
    Suppressed,
};

// Finds the token holding offset-1 (the character just typed). Returns
// tokens.size() when offset == 0 or source is empty.
std::size_t TokenBefore(const std::vector<Token>& tokens, std::size_t offset)
{
    if (offset == 0) return tokens.size();
    const std::size_t pos = offset - 1;
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].offset <= pos && pos < tokens[i].offset + tokens[i].length) return i;
    }
    return tokens.size();
}

std::size_t PreviousSignificant(const std::vector<Token>& tokens, std::size_t index,
                                std::string_view source)
{
    std::size_t i = index;
    while (i > 0)
    {
        --i;
        const TokenKind kind = tokens[i].kind;
        if (kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
            kind == TokenKind::BlockComment)
        {
            continue;
        }
        (void)source;
        return i;
    }
    return tokens.size();
}

std::string_view TokenText(std::string_view source, const Token& token)
{
    return source.substr(token.offset, token.length);
}

CursorContext ClassifyContext(std::string_view source, const std::vector<Token>& tokens,
                              std::size_t offset, std::string_view prefix)
{
    if (offset > source.size()) offset = source.size();
    const std::size_t before = TokenBefore(tokens, offset);
    if (before < tokens.size())
    {
        const TokenKind kind = tokens[before].kind;
        if (kind == TokenKind::LineComment || kind == TokenKind::BlockComment ||
            kind == TokenKind::StringLiteral || kind == TokenKind::CharacterLiteral ||
            kind == TokenKind::RawStringLiteral)
        {
            return CursorContext::Suppressed;
        }
        if (kind == TokenKind::Number) return CursorContext::Suppressed;
    }

    // Preprocessor directive line: `^\s*#\s*\w*$` before the cursor.
    std::size_t line_start = source.rfind('\n', offset > 0 ? offset - 1 : 0);
    line_start = line_start == std::string_view::npos ? 0 : line_start + 1;
    // If offset is at the start of a line, rfind finds the previous newline;
    // recompute when offset points just after '\n'.
    if (offset > 0 && offset <= source.size() && source[offset - 1] == '\n') line_start = offset;
    std::size_t first = line_start;
    while (first < offset && (source[first] == ' ' || source[first] == '\t')) ++first;
    if (first < offset && source[first] == '#') return CursorContext::Preprocessor;

    // Member / scope access: look at the significant token before the prefix.
    // The prefix itself is either the identifier token under the cursor or
    // empty (cursor after punctuation/whitespace).
    std::size_t anchor = before;
    bool prefix_is_identifier = false;
    if (before < tokens.size() && tokens[before].kind == TokenKind::Identifier)
    {
        const std::string_view text = TokenText(source, tokens[before]);
        if (!prefix.empty() && text.size() >= prefix.size() &&
            text.substr(text.size() - prefix.size()) == prefix &&
            tokens[before].offset + tokens[before].length >= offset &&
            tokens[before].offset < offset)
        {
            prefix_is_identifier = true;
        }
    }
    std::size_t prev = tokens.size();
    if (prefix_is_identifier)
    {
        prev = PreviousSignificant(tokens, before, source);
    }
    else if (before < tokens.size())
    {
        // Cursor is right after punctuation/whitespace: the token before the
        // cursor is significant unless it is whitespace.
        if (tokens[before].kind == TokenKind::Whitespace)
        {
            prev = PreviousSignificant(tokens, before, source);
        }
        else
        {
            prev = before;
        }
    }
    else
    {
        // Offset past the end: scan back to the last significant token.
        prev = PreviousSignificant(tokens, tokens.size(), source);
    }

    if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
    {
        const std::string_view text = TokenText(source, tokens[prev]);
        if (text == "." || text == "->" || text == ".*") return CursorContext::MemberAccess;
        if (text == "::") return CursorContext::ScopeAccess;
    }
    return CursorContext::Expression;
}

CompletionKind ClassifyDeclaredName(const ParseTree& tree, std::size_t node_index)
{
    // A DeclaredName directly under a FunctionDefinition/Declaration (through
    // Declarator/FunctionSuffix, without crossing a body statement) is the
    // function's own name. Parameters and locals live under ParameterDeclaration
    // or body statements and must stay Variable.
    std::size_t current = tree.Nodes()[node_index].parent;
    for (std::size_t depth = 0; depth < 8 && current < tree.Nodes().size(); ++depth)
    {
        const GrammarKind kind = tree.Nodes()[current].kind;
        if (kind == GrammarKind::ParameterDeclaration || kind == GrammarKind::CompoundStatement ||
            kind == GrammarKind::DeclarationStatement || kind == GrammarKind::ExpressionStatement ||
            kind == GrammarKind::ReturnStatement || kind == GrammarKind::IfStatement ||
            kind == GrammarKind::LoopStatement || kind == GrammarKind::SwitchStatement ||
            kind == GrammarKind::CaseLabel || kind == GrammarKind::TryStatement ||
            kind == GrammarKind::DoStatement || kind == GrammarKind::LambdaExpression)
        {
            return CompletionKind::Variable;
        }
        if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration)
        {
            return CompletionKind::Function;
        }
        if (kind == GrammarKind::RecordDefinition || kind == GrammarKind::ConceptDefinition ||
            kind == GrammarKind::TemplateDeclaration)
        {
            // `struct Foo` / `concept C`: the name itself is a type. Members
            // declared inside still walk past RecordDefinition, so only treat
            // direct children as types.
            if (tree.Nodes()[node_index].parent == current ||
                tree.Nodes()[tree.Nodes()[node_index].parent].parent == current)
            {
                return CompletionKind::Type;
            }
            return CompletionKind::Variable;
        }
        if (kind == GrammarKind::Enumerator) return CompletionKind::Variable;
        current = tree.Nodes()[current].parent;
    }
    return CompletionKind::Variable;
}

void InsertCandidate(std::unordered_map<std::string, CompletionKind>& best, std::string_view label,
                     CompletionKind kind)
{
    if (label.empty() || !IsIdentStart(label.front())) return;
    const auto found = best.find(std::string(label));
    if (found == best.end())
    {
        best.emplace(std::string(label), kind);
    }
    else if (KindPriority(kind) > KindPriority(found->second))
    {
        found->second = kind;
    }
}

void CollectDefines(std::string_view source, const std::vector<Token>& tokens,
                    std::unordered_map<std::string, CompletionKind>& best, std::string_view prefix)
{
    for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
    {
        if (tokens[i].kind != TokenKind::Punctuation) continue;
        if (TokenText(source, tokens[i]) != "#") continue;
        std::size_t j = i + 1;
        while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace) ++j;
        if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier) continue;
        if (TokenText(source, tokens[j]) != "define") continue;
        ++j;
        while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace) ++j;
        if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier) continue;
        const std::string_view name = TokenText(source, tokens[j]);
        if (prefix.empty() || StartsWith(name, prefix)) InsertCandidate(best, name, CompletionKind::Macro);
    }
}

// The grammar does not always materialize the tag name itself as a
// DeclaredName (e.g. `struct Widget { ... };`), so collect tag names
// lexically as a fallback: `class/struct/union/enum [class/struct] Name`
// plus `using Name`. Mirrors SemanticAnalyzer::CollectTypeNames.
void CollectTagNames(std::string_view source, const std::vector<Token>& tokens,
                     std::unordered_map<std::string, CompletionKind>& best, std::string_view prefix)
{
    std::vector<std::string_view> words;
    words.reserve(tokens.size());
    for (const auto& token : tokens)
    {
        if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
            token.kind == TokenKind::BlockComment)
        {
            continue;
        }
        words.push_back(TokenText(source, token));
    }
    auto is_word = [](std::string_view value) {
        return !value.empty() && IsIdentStart(value.front());
    };
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        std::string_view candidate {};
        if (words[i] == "using" && i + 1 < words.size() && is_word(words[i + 1]))
        {
            candidate = words[i + 1];
        }
        else if ((words[i] == "class" || words[i] == "struct" || words[i] == "union" ||
                  words[i] == "enum") &&
                 i + 1 < words.size())
        {
            std::size_t name = i + 1;
            if ((words[i] == "enum") && name < words.size() &&
                (words[name] == "class" || words[name] == "struct"))
            {
                ++name;
            }
            if (name < words.size() && is_word(words[name]) && !IsKeyword(words[name]))
            {
                candidate = words[name];
            }
        }
        if (!candidate.empty() && (prefix.empty() || StartsWith(candidate, prefix)))
        {
            InsertCandidate(best, candidate, CompletionKind::Type);
        }
    }
}

} // namespace

std::string CompletionEngine::PrefixAt(std::string_view source, std::size_t offset)
{
    if (offset > source.size()) offset = source.size();
    std::size_t start = offset;
    while (start > 0 && IsIdentChar(source[start - 1])) --start;
    return std::string(source.substr(start, offset - start));
}

std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source, std::size_t offset)
{
    ParserOptions options;
    return Complete(source, options, offset);
}

std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source,
                                                      const ParserOptions& options, std::size_t offset)
{
    if (offset > source.size()) offset = source.size();
    const std::string prefix = PrefixAt(source, offset);
    const std::vector<Token> tokens = Lexer(source).Lex();
    const CursorContext context = ClassifyContext(source, tokens, offset, prefix);

    if (context == CursorContext::Suppressed) return {};
    if (context == CursorContext::MemberAccess) return {};

    std::unordered_map<std::string, CompletionKind> best;

    if (context == CursorContext::Preprocessor)
    {
        for (const auto directive : kDirectives)
        {
            if (prefix.empty() || StartsWith(directive, prefix))
            {
                InsertCandidate(best, directive, CompletionKind::Directive);
            }
        }
        CollectDefines(source, tokens, best, prefix);
        for (const auto& [name, value] : options.predefined_macros)
        {
            if (prefix.empty() || StartsWith(name, prefix))
            {
                InsertCandidate(best, name, CompletionKind::Macro);
            }
        }
    }
    else
    {
        const bool scope_only_types = context == CursorContext::ScopeAccess;
        // Keywords (skipped for `::` scope access: `std::ret` is not a keyword).
        if (!scope_only_types)
        {
            for (const auto keyword : kKeywords)
            {
                if (!prefix.empty() && !StartsWith(keyword, prefix)) continue;
                // `compl`/`co_await` appear twice in the table; dedupe keeps one.
                if (IsBuiltinType(keyword))
                {
                    InsertCandidate(best, keyword, CompletionKind::Type);
                }
                else
                {
                    InsertCandidate(best, keyword, CompletionKind::Keyword);
                }
            }
        }
        else
        {
            for (const auto keyword : kKeywords)
            {
                if (!IsBuiltinType(keyword)) continue;
                if (!prefix.empty() && !StartsWith(keyword, prefix)) continue;
                InsertCandidate(best, keyword, CompletionKind::Type);
            }
        }

        // Identifiers seen anywhere in the file (cheap lexical fallback that
        // also works when the grammar is mid-recovery on incomplete code).
        if (!scope_only_types)
        {
            for (const auto& token : tokens)
            {
                if (token.kind != TokenKind::Identifier) continue;
                const std::string_view text = TokenText(source, token);
                if (text == prefix) continue; // don't echo the word being typed
                if (!prefix.empty() && !StartsWith(text, prefix)) continue;
                if (IsKeyword(text)) continue; // keyword entry already added
                InsertCandidate(best, text, CompletionKind::Variable);
            }
        }

        // Declared names upgrade the kind (function/type vs plain variable).
        // Tag names (`struct Widget`) are not always DeclaredName nodes, so
        // collect them lexically as well.
        CollectTagNames(source, tokens, best, prefix);
        const ParseTree tree = ParseTree::Parse(source, options);
        const auto& tree_tokens = tree.Nodes().empty() ? tokens : tree.Tokens();
        (void)tree_tokens;
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind != GrammarKind::DeclaredName) continue;
            const std::size_t token_index = tree.Nodes()[n].first_token;
            if (token_index >= tree.Tokens().size()) continue;
            const std::string_view name = tree.Text(tree.Tokens()[token_index]);
            if (name.empty()) continue;
            if (!prefix.empty() && !StartsWith(name, prefix)) continue;
            if (scope_only_types)
            {
                // After `::` keep only type-like names; member variables and
                // functions belong to an instance, not a scope qualifier.
                const CompletionKind kind = ClassifyDeclaredName(tree, n);
                if (kind != CompletionKind::Type && kind != CompletionKind::Function) continue;
            }
            const CompletionKind kind = ClassifyDeclaredName(tree, n);
            if (IsKeyword(name) && kind == CompletionKind::Variable) continue;
            InsertCandidate(best, name, kind);
        }

        // Macros from `#define` and predefined compile-command defines.
        CollectDefines(source, tokens, best, prefix);
        for (const auto& [name, value] : options.predefined_macros)
        {
            if (!prefix.empty() && !StartsWith(name, prefix)) continue;
            InsertCandidate(best, name, CompletionKind::Macro);
        }
    }

    std::vector<CompletionItem> items;
    items.reserve(best.size());
    for (const auto& [label, kind] : best)
    {
        items.push_back({ label, kind, KindDetail(kind) });
    }
    std::sort(items.begin(), items.end(), [](const CompletionItem& left, const CompletionItem& right) {
        if (left.label != right.label) return left.label < right.label;
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    });
    return items;
}

} // namespace heimdall
