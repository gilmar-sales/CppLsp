#include <Heimdall/IncludeIndex.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{

namespace
{

struct IncludeRef
{
    std::string name;
    bool angled = false;
};

// Lexical `#include` scan: `# include "file"` or `# include <file>`.
// Deliberately shallow (no line continuations, no macro includes): enough to
// find headers, never executed as code.
std::vector<IncludeRef> ScanIncludes(std::string_view text)
{
    std::vector<IncludeRef> refs;
    const std::vector<Token> tokens = Lexer(text).Lex();
    auto significant = [&](std::size_t i) -> std::size_t {
        while (i < tokens.size() && (tokens[i].kind == TokenKind::Whitespace ||
                                     tokens[i].kind == TokenKind::LineComment ||
                                     tokens[i].kind == TokenKind::BlockComment))
        {
            ++i;
        }
        return i;
    };
    auto text_of = [&](std::size_t i) -> std::string_view {
        return text.substr(tokens[i].offset, tokens[i].length);
    };
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].kind != TokenKind::Punctuation || text_of(i) != "#") continue;
        i = significant(i + 1);
        if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier || text_of(i) != "include")
        {
            continue;
        }
        i = significant(i + 1);
        if (i >= tokens.size()) break;
        if (tokens[i].kind == TokenKind::StringLiteral)
        {
            std::string_view quoted = text_of(i);
            const std::size_t open = quoted.find('"');
            const std::size_t close = quoted.rfind('"');
            if (open != std::string_view::npos && close != std::string_view::npos && close > open)
            {
                refs.push_back({ std::string(quoted.substr(open + 1, close - open - 1)), false });
            }
        }
        else if (tokens[i].kind == TokenKind::Punctuation && text_of(i) == "<")
        {
            // Header names contain no `>`; slice raw source up to it.
            const std::size_t start = tokens[i].offset + tokens[i].length;
            std::size_t stop = start;
            while (stop < text.size() && text[stop] != '>' && text[stop] != '\n') ++stop;
            if (stop < text.size() && text[stop] == '>')
            {
                std::string_view name = text.substr(start, stop - start);
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                    name.remove_prefix(1);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                    name.remove_suffix(1);
                if (!name.empty()) refs.push_back({ std::string(name), true });
            }
        }
    }
    return refs;
}

std::filesystem::path NormalizedAbsolute(const std::filesystem::path& path)
{
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

std::filesystem::path TryResolve(const std::string& name, bool angled,
                                 const std::filesystem::path& including_dir,
                                 const CompileCommand* command,
                                 const std::vector<std::filesystem::path>& system_dirs)
{
    std::vector<std::filesystem::path> dirs;
    if (!angled)
    {
        if (!including_dir.empty()) dirs.push_back(including_dir);
        if (command != nullptr)
        {
            dirs.insert(dirs.end(), command->quote_directories.begin(),
                        command->quote_directories.end());
        }
    }
    if (command != nullptr)
    {
        dirs.insert(dirs.end(), command->include_directories.begin(),
                    command->include_directories.end());
    }
    dirs.insert(dirs.end(), system_dirs.begin(), system_dirs.end());
    std::error_code ec;
    for (const auto& dir : dirs)
    {
        if (dir.empty()) continue;
        const auto candidate = NormalizedAbsolute(dir / name);
        if (std::filesystem::exists(candidate, ec) && !ec &&
            std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }
    return {};
}

std::string DriverOf(const CompileCommand* command)
{
    if (command != nullptr && !command->arguments.empty() && !command->arguments.front().empty())
    {
        return command->arguments.front();
    }
    return "c++";
}

std::string ReadFile(const std::filesystem::path& path, std::size_t max_bytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > max_bytes) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string content;
    content.resize(static_cast<std::size_t>(size));
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(in.gcount()));
    return content;
}

bool IsReservedName(std::string_view name)
{
    return !name.empty() && name.front() == '_';
}

void MergeScope(ScopeIndex& index, const IndexedScope& scope)
{
    for (auto& entry : index)
    {
        if (entry.path == scope.path)
        {
            entry.members.insert(entry.members.end(), scope.members.begin(), scope.members.end());
            return;
        }
    }
    index.push_back(scope);
}

} // namespace

std::vector<std::filesystem::path> IncludeIndex::SystemIncludes(std::string_view compiler)
{
    static std::unordered_map<std::string, std::vector<std::filesystem::path>> cache;
    const std::string key(compiler.empty() ? "c++" : compiler);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;

    std::vector<std::filesystem::path> dirs;
#if defined(_WIN32)
    const std::string command = "\"" + key + "\" -xc++ -E -v - <nul 2>&1";
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(command.c_str(), "r"), _pclose);
#else
    const std::string command = "\"" + key + "\" -xc++ -E -v - </dev/null 2>&1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
#endif
    if (pipe)
    {
        bool listing = false;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
        {
            std::string_view line(buffer);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.remove_suffix(1);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (line == "#include <...> search starts here:") listing = true;
            else if (line == "End of search list.") listing = false;
            else if (listing && !line.empty())
            {
                constexpr std::string_view framework = " (framework directory)";
                if (line.ends_with(framework)) line.remove_suffix(framework.size());
                if (!line.empty()) dirs.push_back(NormalizedAbsolute(std::filesystem::path(line)));
            }
        }
    }
    cache.emplace(key, dirs);
    return dirs;
}

std::vector<std::filesystem::path> IncludeIndex::ResolveHeaders(const std::filesystem::path& base_dir,
                                                               std::string_view text,
                                                               const CompileCommand* command,
                                                               const Limits& limits)
{
    const std::vector<std::filesystem::path> system_dirs = SystemIncludes(DriverOf(command));
    std::vector<std::filesystem::path> ordered;
    std::unordered_set<std::string> visited;
    // Worklist of (including directory, include name, angled, depth).
    struct Work
    {
        std::filesystem::path dir;
        std::string name;
        bool angled = false;
        int depth = 0;
    };
    std::vector<Work> stack;
    for (const auto& ref : ScanIncludes(text))
    {
        stack.push_back({ base_dir, ref.name, ref.angled, 0 });
    }
    while (!stack.empty() && ordered.size() < limits.max_headers)
    {
        Work work = std::move(stack.back());
        stack.pop_back();
        if (work.depth > limits.max_depth) continue;
        const auto resolved = TryResolve(work.name, work.angled, work.dir, command, system_dirs);
        if (resolved.empty()) continue;
        const std::string key = resolved.string();
        if (!visited.insert(key).second) continue;
        ordered.push_back(resolved);
        const std::string nested = ReadFile(resolved, limits.max_file_bytes);
        if (nested.empty()) continue;
        for (const auto& ref : ScanIncludes(nested))
        {
            stack.push_back({ resolved.parent_path(), ref.name, ref.angled, work.depth + 1 });
        }
    }
    return ordered;
}

std::string IncludeIndex::CacheKey(const std::vector<std::filesystem::path>& headers,
                                   const CompileCommand* command)
{
    std::string key;
    for (const auto& header : headers)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(header, ec);
        const auto time = std::filesystem::last_write_time(header, ec);
        key += header.string();
        key += '\0';
        key += std::to_string(ec ? 0 : size);
        key += '\0';
        key += std::to_string(ec ? 0 : time.time_since_epoch().count());
        key += '\0';
    }
    key += '\1';
    if (command != nullptr)
    {
        key += std::to_string(static_cast<int>(command->standard));
        key += '\0';
        std::vector<std::string> defines;
        for (const auto& [name, value] : command->defines)
        {
            defines.push_back(name + "=" + value);
        }
        std::sort(defines.begin(), defines.end());
        for (const auto& define : defines)
        {
            key += define;
            key += '\0';
        }
    }
    return key;
}

IncludeIndex IncludeIndex::Build(const std::vector<std::filesystem::path>& headers,
                                 const CompileCommand* command, const Limits& limits)
{
    IncludeIndex index;
    ParserOptions options;
    if (command != nullptr)
    {
        options.standard = command->standard;
        options.predefined_macros = command->defines;
        for (const auto& name : command->undefines) options.predefined_macros.erase(name);
    }
    for (const auto& header : headers)
    {
        const std::string content = ReadFile(header, limits.max_file_bytes);
        if (content.empty()) continue;
        for (const auto& scope : CompletionEngine::IndexScopes(content, options))
        {
            if (!scope.path.empty())
            {
                bool reserved = false;
                for (const auto& element : scope.path)
                {
                    if (IsReservedName(element))
                    {
                        reserved = true;
                        break;
                    }
                }
                if (reserved) continue;
            }
            IndexedScope filtered = scope;
            filtered.members.clear();
            for (const auto& member : scope.members)
            {
                if (IsReservedName(member.label)) continue;
                filtered.members.push_back(member);
            }
            if (!filtered.members.empty()) MergeScope(index.m_scopes, filtered);
        }
    }
    return index;
}

IncludeIndex IncludeIndex::Build(const std::filesystem::path& base_dir, std::string_view text,
                                 const CompileCommand* command, const Limits& limits)
{
    return Build(ResolveHeaders(base_dir, text, command, limits), command, limits);
}

} // namespace heimdall
