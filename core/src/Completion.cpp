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
    case CompletionKind::Type:
    case CompletionKind::Namespace: return 4;
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
    case CompletionKind::Namespace: return "namespace";
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

// Index of the `.`, `->`, `.*` or `::` punctuation token directly preceding
// the completion prefix (whitespace allowed, e.g. `ns :: name`), or
// tokens.size() when there is none. Shared by context classification and
// qualifier extraction so the two can never disagree.
std::size_t AccessOperatorBefore(std::string_view source, const std::vector<Token>& tokens,
                                 std::size_t offset, std::string_view prefix)
{
    const std::size_t before = TokenBefore(tokens, offset);
    // The prefix itself is either the identifier token under the cursor or
    // empty (cursor after punctuation/whitespace).
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
    if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation) return prev;
    return tokens.size();
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
    const std::size_t access = AccessOperatorBefore(source, tokens, offset, prefix);
    if (access < tokens.size())
    {
        const std::string_view text = TokenText(source, tokens[access]);
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
        if (kind == GrammarKind::UsingDeclaration) return CompletionKind::Type;
        if (kind == GrammarKind::Declaration)
        {
            // Old-style `typedef Rep name;` introduces a type alias. Trivia
            // (comments, blank lines) and leading macros shift the keyword, so
            // scan the first significant tokens instead of raw positions.
            const GrammarNode& declaration = tree.Nodes()[current];
            const std::size_t last =
                std::min(declaration.first_token + declaration.token_count, tree.Tokens().size());
            std::size_t seen = 0;
            for (std::size_t t = declaration.first_token; t < last && seen < 4; ++t)
            {
                const TokenKind token_kind = tree.Tokens()[t].kind;
                if (token_kind == TokenKind::Whitespace || token_kind == TokenKind::LineComment ||
                    token_kind == TokenKind::BlockComment)
                {
                    continue;
                }
                if (tree.Text(tree.Tokens()[t]) == "typedef") return CompletionKind::Type;
                ++seen;
            }
        }
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

// --- Scope-aware name resolution -------------------------------------------
// Unqualified lookup walks the scope chain (block, function, namespace,
// global) honoring the point of declaration; qualified lookup (`ns::name`)
// resolves the qualifier to a scope node and lists its direct members.

constexpr std::size_t NoIndex = static_cast<std::size_t>(-1);

bool IsScopeKind(GrammarKind kind)
{
    switch (kind)
    {
    case GrammarKind::TranslationUnit:
    case GrammarKind::NamespaceDefinition:
    case GrammarKind::RecordDefinition:
    case GrammarKind::FunctionDefinition:
    case GrammarKind::CompoundStatement:
    case GrammarKind::LambdaExpression: return true;
    default: return false;
    }
}

std::pair<std::size_t, std::size_t> NodeRange(const ParseTree& tree, std::size_t node)
{
    if (node >= tree.Nodes().size()) return { 0, 0 };
    const GrammarNode& grammar = tree.Nodes()[node];
    if (grammar.token_count == 0 || grammar.first_token >= tree.Tokens().size()) return { 0, 0 };
    const std::size_t last = grammar.first_token + grammar.token_count <= tree.Tokens().size()
                                 ? grammar.first_token + grammar.token_count - 1
                                 : tree.Tokens().size() - 1;
    const Token& first = tree.Tokens()[grammar.first_token];
    const Token& last_token = tree.Tokens()[last];
    return { first.offset, last_token.offset + last_token.length };
}

bool IsLocalBoundary(GrammarKind kind)
{
    switch (kind)
    {
    case GrammarKind::ParameterDeclaration:
    case GrammarKind::CompoundStatement:
    case GrammarKind::DeclarationStatement:
    case GrammarKind::ExpressionStatement:
    case GrammarKind::ReturnStatement:
    case GrammarKind::IfStatement:
    case GrammarKind::LoopStatement:
    case GrammarKind::SwitchStatement:
    case GrammarKind::CaseLabel:
    case GrammarKind::TryStatement:
    case GrammarKind::DoStatement:
    case GrammarKind::LambdaExpression: return true;
    default: return false;
    }
}

struct LocalInfo
{
    bool is_local = false;
    // Enclosing function/lambda for parameters and body locals; NoIndex when
    // the name belongs to an outer scope (or to no function at all, such as
    // prototype parameters, which are never usable).
    std::size_t owner = NoIndex;
    // Innermost block owning a body local; the function itself for parameters.
    std::size_t boundary = NoIndex;
};

// Splits DeclaredName nodes into function parameters/body locals versus names
// owned by an outer scope (the function's own name, globals, namespace and
// record members). Lambda bodies count as function boundaries so their
// parameters are modeled as locals of the lambda.
LocalInfo AnalyzeLocal(const ParseTree& tree, std::size_t node)
{
    LocalInfo info;
    if (node >= tree.Nodes().size()) return info;
    std::size_t current = tree.Nodes()[node].parent;
    for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
    {
        const GrammarKind kind = tree.Nodes()[current].kind;
        if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::LambdaExpression)
        {
            if (!info.is_local) return info; // the function's own name
            info.owner = current;
            if (info.boundary == NoIndex) info.boundary = current; // parameters: whole body
            return info;
        }
        if (IsLocalBoundary(kind))
        {
            info.is_local = true;
            // The first non-parameter boundary is the owning block (`for`
            // initializers and `if` initializers own their loop/statement).
            if (kind != GrammarKind::ParameterDeclaration && info.boundary == NoIndex)
            {
                info.boundary = current;
            }
        }
        if (kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
            kind == GrammarKind::RecordDefinition)
        {
            return info;
        }
        current = tree.Nodes()[current].parent;
    }
    return info;
}

bool IsTransparentForMembership(GrammarKind kind)
{
    // Declarator plumbing between a name and the scope that owns it: the name
    // of a function defined in a namespace belongs to the namespace, while a
    // parameter or body local stops at ParameterDeclaration/CompoundStatement.
    switch (kind)
    {
    case GrammarKind::Declarator:
    case GrammarKind::InitDeclarator:
    case GrammarKind::Declaration:
    case GrammarKind::TypeSpecifier:
    case GrammarKind::FunctionSuffix:
    case GrammarKind::PointerOperator:
    case GrammarKind::NestedNameSpecifier:
    case GrammarKind::ArraySuffix:
    case GrammarKind::TrailingReturnType:
    case GrammarKind::NoexceptSpecifier:
    case GrammarKind::AttributeSpecifier:
    case GrammarKind::BitfieldSuffix:
    case GrammarKind::TemplateDeclaration:
    case GrammarKind::TemplateArgument:
    case GrammarKind::FunctionDefinition:
    case GrammarKind::FunctionDeclaration:
    case GrammarKind::Enumerator:
    case GrammarKind::RequiresClause:
    case GrammarKind::DeclaredName: return true;
    default: return false;
    }
}

// Scope node a name belongs to for qualified lookup: the namespace/record
// that directly owns it (a function defined in a namespace belongs to the
// namespace; its locals belong to body blocks instead).
std::size_t MemberScope(const ParseTree& tree, std::size_t node)
{
    if (node >= tree.Nodes().size()) return NoIndex;
    std::size_t current = tree.Nodes()[node].parent;
    for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
    {
        if (IsTransparentForMembership(tree.Nodes()[current].kind))
        {
            current = tree.Nodes()[current].parent;
            continue;
        }
        return current;
    }
    return NoIndex;
}

// Name elements introducing a namespace/record scope node. Compound
// definitions (`namespace a::b`) contribute several elements; anonymous scopes
// contribute one empty element so they can never match a typed qualifier.
std::vector<std::string> ScopeNameElements(const ParseTree& tree, std::size_t node)
{
    const auto& nodes = tree.Nodes();
    if (node >= nodes.size()) return { {} };
    const GrammarKind kind = nodes[node].kind;
    if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
    {
        return { {} };
    }
    const GrammarNode& grammar = nodes[node];
    std::vector<std::size_t> significant;
    const std::size_t end =
        std::min(grammar.first_token + grammar.token_count, tree.Tokens().size());
    for (std::size_t i = grammar.first_token; i < end; ++i)
    {
        const TokenKind token_kind = tree.Tokens()[i].kind;
        if (token_kind == TokenKind::Whitespace || token_kind == TokenKind::LineComment ||
            token_kind == TokenKind::BlockComment)
        {
            continue;
        }
        significant.push_back(i);
    }
    auto text = [&](std::size_t token) -> std::string_view { return tree.Text(tree.Tokens()[token]); };
    auto is_identifier = [&](std::size_t token) {
        return tree.Tokens()[token].kind == TokenKind::Identifier;
    };
    std::size_t pos = 0;
    if (kind == GrammarKind::NamespaceDefinition)
    {
        while (pos < significant.size() &&
               (text(significant[pos]) == "export" || text(significant[pos]) == "inline"))
        {
            ++pos;
        }
        if (pos >= significant.size() || text(significant[pos]) != "namespace") return { {} };
        ++pos;
        // Anonymous (`{`), alias (`name = ...`), or a dotted name.
        std::vector<std::string> names;
        while (pos < significant.size() && is_identifier(significant[pos]))
        {
            names.emplace_back(text(significant[pos]));
            ++pos;
            if (pos + 1 < significant.size() && text(significant[pos]) == "::" &&
                is_identifier(significant[pos + 1]))
            {
                ++pos;
                continue;
            }
            break;
        }
        return names.empty() ? std::vector<std::string> { {} } : names;
    }
    for (; pos < significant.size(); ++pos)
    {
        const std::string_view word = text(significant[pos]);
        if (word != "struct" && word != "class" && word != "union" && word != "enum") continue;
        ++pos;
        if (word == "enum" && pos < significant.size() &&
            (text(significant[pos]) == "class" || text(significant[pos]) == "struct"))
        {
            ++pos;
        }
        if (pos < significant.size() && is_identifier(significant[pos]))
        {
            return { std::string(text(significant[pos])) };
        }
        return { {} };
    }
    return { {} };
}

// Whether a namespace definition is `inline namespace` (members visible as
// direct members of the enclosing scope).
bool IsInlineNamespace(const ParseTree& tree, std::size_t node)
{
    const auto& nodes = tree.Nodes();
    if (node >= nodes.size() || nodes[node].kind != GrammarKind::NamespaceDefinition) return false;
    const GrammarNode& grammar = nodes[node];
    const std::size_t end =
        std::min(grammar.first_token + grammar.token_count, tree.Tokens().size());
    bool seen_inline = false;
    for (std::size_t i = grammar.first_token; i < end; ++i)
    {
        const Token& token = tree.Tokens()[i];
        if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
            token.kind == TokenKind::BlockComment)
        {
            continue;
        }
        const std::string_view word = tree.Text(token);
        if (word == "inline")
        {
            seen_inline = true;
            continue;
        }
        if (word == "export") continue;
        return seen_inline && word == "namespace";
    }
    return false;
}

// Qualified path of a scope node from the translation unit down, e.g.
// `outer::inner`. Function-like levels cannot be named from the outside and
// are skipped, so a function-local struct still resolves by its tag. Inline
// namespaces are transparent (their members are visible as direct members of
// the enclosing scope), which is what makes e.g. libc++ `std::__1::vector`
// resolve as `std::vector`.
std::vector<std::string> ScopePath(const ParseTree& tree, std::size_t node)
{
    std::vector<std::string> path;
    std::size_t current = node;
    for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
    {
        const GrammarKind kind = tree.Nodes()[current].kind;
        if (kind == GrammarKind::TranslationUnit) break;
        if (kind == GrammarKind::NamespaceDefinition || kind == GrammarKind::RecordDefinition)
        {
            if (!IsInlineNamespace(tree, current))
            {
                const auto elements = ScopeNameElements(tree, current);
                path.insert(path.begin(), elements.begin(), elements.end());
            }
        }
        current = tree.Nodes()[current].parent;
    }
    return path;
}

struct Qualifier
{
    std::vector<std::string> path;
    bool global = false;     // leading `::` with no names (`::name`)
    bool unresolved = false; // modeled as nothing (`A<int>::x`, `call()::y`)
};

// Names in `a::b::` before the final `::` at scope_op.
Qualifier QualifierBefore(std::string_view source, const std::vector<Token>& tokens,
                          std::size_t scope_op)
{
    Qualifier result;
    std::size_t current = scope_op;
    while (true)
    {
        const std::size_t name = PreviousSignificant(tokens, current, source);
        if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier) break;
        result.path.insert(result.path.begin(), std::string(TokenText(source, tokens[name])));
        const std::size_t separator = PreviousSignificant(tokens, name, source);
        if (separator >= tokens.size() || tokens[separator].kind != TokenKind::Punctuation ||
            TokenText(source, tokens[separator]) != "::")
        {
            break;
        }
        current = separator;
    }
    if (!result.path.empty()) return result;
    // No identifiers: either a global qualifier (`::name`) or something this
    // engine cannot model. A closing bracket hints at the latter
    // (`A<int>::x`); anything else is the global scope.
    const std::size_t prev = PreviousSignificant(tokens, scope_op, source);
    if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
    {
        const std::string_view text = TokenText(source, tokens[prev]);
        if (text == ">" || text == ">>" || text == ")" || text == "]") result.unresolved = true;
        else result.global = true;
    }
    else
    {
        result.global = true;
    }
    return result;
}

// Scope nodes whose qualified path equals the qualifier. Namespaces reopened
// in several blocks naturally yield several targets whose members unite.
std::vector<std::size_t> ResolveScope(const ParseTree& tree, const std::vector<std::string>& path)
{
    std::vector<std::size_t> targets;
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
        {
            continue;
        }
        if (ScopePath(tree, n) == path) targets.push_back(n);
    }
    return targets;
}

bool InNamedScope(const ParseTree& tree, std::size_t offset)
{
    for (std::size_t n = 1; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
        {
            continue;
        }
        const auto [start, end] = NodeRange(tree, n);
        if (start <= offset && offset < end) return true;
    }
    return false;
}

// Innermost function/lambda range containing pos, for occurrence visibility.
std::size_t InnermostCallable(const ParseTree& tree, std::size_t pos)
{
    std::size_t found = NoIndex;
    std::size_t span = static_cast<std::size_t>(-1);
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::FunctionDefinition && kind != GrammarKind::LambdaExpression)
        {
            continue;
        }
        const auto [start, end] = NodeRange(tree, n);
        if (start <= pos && pos <= end && end - start < span)
        {
            found = n;
            span = end - start;
        }
    }
    return found;
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
// plus `using Name` and `concept Name`. Mirrors SemanticAnalyzer::
// CollectTypeNames. The range restriction serves qualified lookup, which
// only wants tags nested directly in the resolved scope.
struct TagName
{
    std::string_view text;
    std::size_t offset = 0;
};

void ScanTagNames(std::string_view source, const std::vector<Token>& tokens, std::vector<TagName>& out)
{
    struct Word
    {
        std::string_view text;
        std::size_t offset = 0;
    };
    std::vector<Word> words;
    words.reserve(tokens.size());
    for (const auto& token : tokens)
    {
        if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
            token.kind == TokenKind::BlockComment)
        {
            continue;
        }
        words.push_back({ TokenText(source, token), token.offset });
    }
    auto is_word = [](std::string_view value) {
        return !value.empty() && IsIdentStart(value.front());
    };
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        auto emit = [&](std::size_t index) {
            if (index < words.size() && is_word(words[index].text))
            {
                out.push_back({ words[index].text, words[index].offset });
            }
        };
        if (words[i].text == "using" || words[i].text == "concept")
        {
            emit(i + 1);
        }
        else if (words[i].text == "class" || words[i].text == "struct" || words[i].text == "union" ||
                 words[i].text == "enum")
        {
            std::size_t name = i + 1;
            if (words[i].text == "enum" && name < words.size() &&
                (words[name].text == "class" || words[name].text == "struct"))
            {
                ++name;
            }
            emit(name);
        }
    }
}

void CollectTagNamesIn(std::string_view source, const std::vector<Token>& tokens,
                       std::unordered_map<std::string, CompletionKind>& best, std::string_view prefix,
                       std::size_t range_start, std::size_t range_end)
{
    std::vector<TagName> tags;
    ScanTagNames(source, tokens, tags);
    for (const auto& tag : tags)
    {
        if (IsKeyword(tag.text)) continue;
        if (tag.offset < range_start || tag.offset >= range_end) continue;
        if (!prefix.empty() && !StartsWith(tag.text, prefix)) continue;
        InsertCandidate(best, tag.text, CompletionKind::Type);
    }
}

void CollectTagNames(std::string_view source, const std::vector<Token>& tokens,
                     std::unordered_map<std::string, CompletionKind>& best, std::string_view prefix)
{
    CollectTagNamesIn(source, tokens, best, prefix, 0, source.size());
}

// Nested scope names below a qualifier: for `ns::`, a scope with path
// `ns::inner` contributes `inner`. Powers both `ns::` member listing and the
// global `::` scope, including names introduced by compound definitions
// (`namespace a::b`) that have no intermediate node to match exactly.
void CollectChildScopes(const ParseTree& tree, const std::vector<std::string>& qualifier,
                        std::unordered_map<std::string, CompletionKind>& best, std::string_view prefix)
{
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
        {
            continue;
        }
        const std::vector<std::string> path = ScopePath(tree, n);
        if (path.size() <= qualifier.size()) continue;
        bool matches = true;
        for (std::size_t i = 0; i < qualifier.size(); ++i)
        {
            if (path[i] != qualifier[i])
            {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        const std::string& name = path[qualifier.size()];
        if (name.empty()) continue;
        // Reserved (`_`-leading) scopes are never meant to be named.
        if (name.front() == '_') continue;
        if (!prefix.empty() && !StartsWith(name, prefix)) continue;
        InsertCandidate(best, name,
                        kind == GrammarKind::NamespaceDefinition ? CompletionKind::Namespace
                                                                : CompletionKind::Type);
    }
}

} // namespace

ScopeIndex CompletionEngine::IndexScopes(std::string_view source, const ParserOptions& options)
{
    const std::vector<Token> tokens = Lexer(source).Lex();
    const ParseTree tree = ParseTree::Parse(source, options);
    ScopeIndex index;
    auto entry_for = [&](const std::vector<std::string>& path, CompletionKind kind) -> IndexedScope& {
        for (auto& entry : index)
        {
            if (entry.path == path) return entry;
        }
        index.push_back({ path, kind, {} });
        return index.back();
    };
    entry_for({}, CompletionKind::Keyword);

    // Declared names bucketed by owning scope; function locals land on body
    // blocks (no entry) and are skipped: they are not qualifier-addressable.
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        if (tree.Nodes()[n].kind != GrammarKind::DeclaredName) continue;
        const std::size_t scope = MemberScope(tree, n);
        if (scope >= tree.Nodes().size()) continue;
        const GrammarKind scope_kind = tree.Nodes()[scope].kind;
        if (scope_kind != GrammarKind::NamespaceDefinition && scope_kind != GrammarKind::RecordDefinition &&
            scope != ParseTree::RootNode)
        {
            continue;
        }
        const std::size_t token_index = tree.Nodes()[n].first_token;
        if (token_index >= tree.Tokens().size()) continue;
        const std::string_view name = tree.Text(tree.Tokens()[token_index]);
        if (name.empty() || IsKeyword(name)) continue;
        const CompletionKind kind = ClassifyDeclaredName(tree, n);
        if (kind == CompletionKind::Variable && name.front() == '_') continue;
        IndexedScope& entry = entry_for(ScopePath(tree, scope), CompletionKind::Type);
        entry.members.push_back({ std::string(name), kind, KindDetail(kind) });
    }
    // Nested scope names owned by each entry's scope.
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
        {
            continue;
        }
        const std::size_t owner = MemberScope(tree, n);
        if (owner >= tree.Nodes().size()) continue;
        const GrammarKind owner_kind = tree.Nodes()[owner].kind;
        if (owner_kind != GrammarKind::NamespaceDefinition && owner_kind != GrammarKind::RecordDefinition &&
            owner != ParseTree::RootNode)
        {
            continue;
        }
        const auto elements = ScopeNameElements(tree, n);
        if (elements.empty() || elements.back().empty() || elements.back().front() == '_') continue;
        IndexedScope& entry = entry_for(ScopePath(tree, owner), CompletionKind::Type);
        const CompletionKind member_kind =
            kind == GrammarKind::NamespaceDefinition ? CompletionKind::Namespace : CompletionKind::Type;
        entry.members.push_back({ elements.back(), member_kind, KindDetail(member_kind) });
    }
    // Tag names bucketed by innermost enclosing named scope.
    std::vector<TagName> tags;
    ScanTagNames(source, tokens, tags);
    for (const auto& tag : tags)
    {
        if (IsKeyword(tag.text) || tag.text.front() == '_') continue;
        std::size_t bucket = ParseTree::RootNode;
        std::size_t span = static_cast<std::size_t>(-1);
        for (std::size_t n = 1; n < tree.Nodes().size(); ++n)
        {
            const GrammarKind kind = tree.Nodes()[n].kind;
            if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
            {
                continue;
            }
            const auto [start, end] = NodeRange(tree, n);
            if (start <= tag.offset && tag.offset < end && end - start < span)
            {
                bucket = n;
                span = end - start;
            }
        }
        IndexedScope& entry = entry_for(ScopePath(tree, bucket), CompletionKind::Type);
        entry.members.push_back({ std::string(tag.text), CompletionKind::Type, "type" });
    }
    // Macros are always global.
    {
        std::unordered_map<std::string, CompletionKind> macros;
        CollectDefines(source, tokens, macros, {});
        IndexedScope& entry = entry_for({}, CompletionKind::Keyword);
        for (const auto& [name, kind] : macros)
        {
            if (name.front() == '_') continue;
            entry.members.push_back({ name, kind, KindDetail(kind) });
        }
    }
    for (auto& entry : index)
    {
        std::sort(entry.members.begin(), entry.members.end(), [](const CompletionItem& left,
                                                                 const CompletionItem& right) {
            if (left.label != right.label) return left.label < right.label;
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        });
        entry.members.erase(
            std::unique(entry.members.begin(), entry.members.end(),
                        [](const CompletionItem& left, const CompletionItem& right) {
                            return left.label == right.label && left.kind == right.kind;
                        }),
            entry.members.end());
    }
    return index;
}

namespace
{

void CompleteExpression(const ParseTree& tree, std::string_view source,
                        const std::vector<Token>& tokens, const ParserOptions& options,
                        std::size_t offset, std::string_view prefix, const ScopeIndex* external,
                        std::unordered_map<std::string, CompletionKind>& best)
{
    for (const auto keyword : kKeywords)
    {
        if (!prefix.empty() && !StartsWith(keyword, prefix)) continue;
        // `compl`/`co_await` appear twice in the table; dedupe keeps one.
        InsertCandidate(best, keyword,
                        IsBuiltinType(keyword) ? CompletionKind::Type : CompletionKind::Keyword);
    }

    // Lexical fallback with occurrence visibility: an identifier is usable
    // when it occurs at global scope or earlier in the cursor's own function.
    // This hides locals of other functions while keeping words the grammar
    // has not modeled, and still works mid-recovery on incomplete code.
    const std::size_t cursor_callable = InnermostCallable(tree, offset);
    for (const auto& token : tokens)
    {
        if (token.kind != TokenKind::Identifier) continue;
        const std::string_view text = TokenText(source, token);
        if (text == prefix) continue; // don't echo the word being typed
        if (!prefix.empty() && !StartsWith(text, prefix)) continue;
        if (IsKeyword(text)) continue; // keyword entry already added
        const std::size_t owner = InnermostCallable(tree, token.offset);
        if (owner != NoIndex && (owner != cursor_callable || token.offset >= offset)) continue;
        InsertCandidate(best, text, CompletionKind::Variable);
    }

    // Declared names upgrade the kind (function/type vs plain variable).
    // Tag names (`struct Widget`) are not always DeclaredName nodes, so
    // collect them lexically as well.
    CollectTagNames(source, tokens, best, prefix);
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        if (tree.Nodes()[n].kind != GrammarKind::DeclaredName) continue;
        const std::size_t token_index = tree.Nodes()[n].first_token;
        if (token_index >= tree.Tokens().size()) continue;
        const std::string_view name = tree.Text(tree.Tokens()[token_index]);
        if (name.empty()) continue;
        if (!prefix.empty() && !StartsWith(name, prefix)) continue;
        const LocalInfo local = AnalyzeLocal(tree, n);
        if (local.is_local)
        {
            // Point of declaration: usable only inside the owning block and
            // once the declaration itself is complete.
            if (local.owner == NoIndex) continue;
            const auto [block_start, block_end] = NodeRange(tree, local.boundary);
            if (offset < block_start || offset > block_end) continue;
            const Token& name_token = tree.Tokens()[token_index];
            if (name_token.offset + name_token.length > offset) continue;
            InsertCandidate(best, name, CompletionKind::Variable);
            continue;
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

    // Header globals (top-level functions, macros, using-aliases) are visible
    // unqualified once included. Namespaced header members stay qualified-only.
    if (external != nullptr)
    {
        for (const auto& scope : *external)
        {
            if (!scope.path.empty()) continue;
            for (const auto& member : scope.members)
            {
                if (!prefix.empty() && !StartsWith(member.label, prefix)) continue;
                InsertCandidate(best, member.label, member.kind);
            }
        }
    }
}

bool MatchesPrefix(std::string_view name, std::string_view prefix)
{
    return !name.empty() && (prefix.empty() || StartsWith(name, prefix));
}

void CompleteQualified(const ParseTree& tree, std::string_view source,
                       const std::vector<Token>& tokens, std::size_t offset, std::string_view prefix,
                       const ScopeIndex* external,
                       std::unordered_map<std::string, CompletionKind>& best)
{
    const std::size_t scope_op = AccessOperatorBefore(source, tokens, offset, prefix);
    if (scope_op >= tokens.size() || TokenText(source, tokens[scope_op]) != "::") return;
    const Qualifier qualifier = QualifierBefore(source, tokens, scope_op);
    if (qualifier.unresolved) return; // `A<int>::x`, `call()::y`: no guesses

    if (qualifier.global)
    {
        // Leading `::` names the global scope: no keywords, builtins, macros
        // or function locals, and no record members.
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind != GrammarKind::DeclaredName) continue;
            if (MemberScope(tree, n) != ParseTree::RootNode) continue;
            const std::size_t token_index = tree.Nodes()[n].first_token;
            if (token_index >= tree.Tokens().size()) continue;
            const std::string_view name = tree.Text(tree.Tokens()[token_index]);
            if (!MatchesPrefix(name, prefix)) continue;
            const CompletionKind kind = ClassifyDeclaredName(tree, n);
            if (IsKeyword(name) && kind == CompletionKind::Variable) continue;
            InsertCandidate(best, name, kind);
        }
        for (const auto& token : tokens)
        {
            if (token.kind != TokenKind::Identifier) continue;
            const std::string_view text = TokenText(source, token);
            if (!MatchesPrefix(text, prefix) || IsKeyword(text)) continue;
            if (InnermostCallable(tree, token.offset) != NoIndex) continue;
            if (InNamedScope(tree, token.offset)) continue;
            InsertCandidate(best, text, CompletionKind::Variable);
        }
        CollectChildScopes(tree, {}, best, prefix);
        if (external != nullptr)
        {
            for (const auto& scope : *external)
            {
                if (scope.path.empty())
                {
                    for (const auto& member : scope.members)
                    {
                        if (!MatchesPrefix(member.label, prefix)) continue;
                        InsertCandidate(best, member.label, member.kind);
                    }
                }
                else if (scope.path.size() == 1)
                {
                    if (!MatchesPrefix(scope.path.front(), prefix)) continue;
                    InsertCandidate(best, scope.path.front(), scope.kind);
                }
            }
        }
        return;
    }

    const std::vector<std::size_t> targets = ResolveScope(tree, qualifier.path);
    // External (header) scopes matching the qualifier union with local ones.
    // An empty target set with no external match means an unknown qualifier.
    bool saw_external = false;
    if (external != nullptr)
    {
        for (const auto& scope : *external)
        {
            if (scope.path != qualifier.path) continue;
            saw_external = true;
            for (const auto& member : scope.members)
            {
                if (!MatchesPrefix(member.label, prefix)) continue;
                InsertCandidate(best, member.label, member.kind);
            }
        }
        // External nested scopes below the qualifier (`std::` offers `chrono`
        // from a `std::chrono` header scope).
        for (const auto& scope : *external)
        {
            if (scope.path.size() <= qualifier.path.size()) continue;
            bool matches = true;
            for (std::size_t i = 0; i < qualifier.path.size(); ++i)
            {
                if (scope.path[i] != qualifier.path[i])
                {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            saw_external = true;
            const std::string& name = scope.path[qualifier.path.size()];
            if (!MatchesPrefix(name, prefix)) continue;
            InsertCandidate(best, name, scope.kind);
        }
    }
    if (targets.empty() && !saw_external) return; // unknown qualifier (`std::`): no guesses
    auto is_target = [&](std::size_t node) {
        for (const auto target : targets)
        {
            if (node == target) return true;
        }
        return false;
    };
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        if (tree.Nodes()[n].kind != GrammarKind::DeclaredName) continue;
        if (!is_target(MemberScope(tree, n))) continue;
        const std::size_t token_index = tree.Nodes()[n].first_token;
        if (token_index >= tree.Tokens().size()) continue;
        const std::string_view name = tree.Text(tree.Tokens()[token_index]);
        if (!MatchesPrefix(name, prefix)) continue;
        const CompletionKind kind = ClassifyDeclaredName(tree, n);
        if (IsKeyword(name) && kind == CompletionKind::Variable) continue;
        InsertCandidate(best, name, kind);
    }
    // Nested scopes (`struct Inner` / `namespace inner`) owned by a target.
    for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
    {
        const GrammarKind kind = tree.Nodes()[n].kind;
        if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
        {
            continue;
        }
        if (!is_target(MemberScope(tree, n))) continue;
        const auto elements = ScopeNameElements(tree, n);
        if (elements.empty() || !MatchesPrefix(elements.back(), prefix)) continue;
        InsertCandidate(best, elements.back(),
                        kind == GrammarKind::NamespaceDefinition ? CompletionKind::Namespace
                                                                : CompletionKind::Type);
    }
    for (const auto target : targets)
    {
        const auto [start, end] = NodeRange(tree, target);
        CollectTagNamesIn(source, tokens, best, prefix, start, end);
    }
    // Members introduced by deeper compound definitions (`ns::a::b` makes `a`
    // visible under `ns::` even without an intermediate node).
    CollectChildScopes(tree, qualifier.path, best, prefix);
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
    return Complete(source, options, offset, nullptr);
}

std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source,
                                                      const ParserOptions& options, std::size_t offset,
                                                      const ScopeIndex* external)
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
        // The grammar tree carries the scope structure for both unqualified
        // visibility and qualified (`ns::`) member resolution.
        const ParseTree tree = ParseTree::Parse(source, options);
        if (context == CursorContext::ScopeAccess)
        {
            CompleteQualified(tree, source, tokens, offset, prefix, external, best);
        }
        else
        {
            CompleteExpression(tree, source, tokens, options, offset, prefix, external, best);
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
