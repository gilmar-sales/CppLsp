#include <CppLsp/Buffer.hpp>
#include <CppLsp/CompileCommands.hpp>
#include <CppLsp/Formatter.hpp>
#include <CppLsp/LineTable.hpp>
#include <CppLsp/Parser.hpp>
#include <CppLsp/Rules.hpp>
#include <CppLsp/Semantic.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace
{

enum class Command
{
    Lint,
    Check,
    Format,
    Parse
};

struct Options
{
    Command command;
    std::size_t jobs = std::max(1u, std::thread::hardware_concurrency());
    bool json = false;
    bool write = false;
    bool fix = false;
    bool semantic = false;
    bool std_override = false;
    cpplsp::CppStandard standard = cpplsp::CppStandard::Cpp20;
    fs::path compile_commands;
    std::vector<fs::path> inputs;
};

struct SyntaxDiagnostic
{
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::string code = "CPPLSP900";
    std::string message;
};

struct ParseNodeSummary
{
    std::string kind;
    std::size_t parent = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct FileResult
{
    fs::path path;
    std::string error;
    std::vector<cpplsp::Diagnostic> diagnostics;
    std::vector<cpplsp::SemanticDiagnostic> semantic_diagnostics;
    std::vector<SyntaxDiagnostic> syntax_diagnostics;
    std::vector<ParseNodeSummary> nodes;
    std::string standard_name = "c++20";
    std::string output;
    bool changed = false;
    bool has_semantic_context = false;
    std::size_t type_count = 0;
    std::size_t declaration_count = 0;
    std::size_t ambiguous_count = 0;
};

std::string_view GrammarKindName(cpplsp::GrammarKind kind)
{
    using cpplsp::GrammarKind;
    switch (kind)
    {
    case GrammarKind::TranslationUnit: return "TranslationUnit";
    case GrammarKind::PreprocessorDirective: return "PreprocessorDirective";
    case GrammarKind::Declaration: return "Declaration";
    case GrammarKind::ParameterDeclaration: return "ParameterDeclaration";
    case GrammarKind::InitDeclarator: return "InitDeclarator";
    case GrammarKind::FunctionDefinition: return "FunctionDefinition";
    case GrammarKind::FunctionDeclaration: return "FunctionDeclaration";
    case GrammarKind::NamespaceDefinition: return "NamespaceDefinition";
    case GrammarKind::RecordDefinition: return "RecordDefinition";
    case GrammarKind::Enumerator: return "Enumerator";
    case GrammarKind::CompoundStatement: return "CompoundStatement";
    case GrammarKind::DeclarationStatement: return "DeclarationStatement";
    case GrammarKind::ExpressionStatement: return "ExpressionStatement";
    case GrammarKind::ReturnStatement: return "ReturnStatement";
    case GrammarKind::IfStatement: return "IfStatement";
    case GrammarKind::LoopStatement: return "LoopStatement";
    case GrammarKind::SwitchStatement: return "SwitchStatement";
    case GrammarKind::JumpStatement: return "JumpStatement";
    case GrammarKind::EmptyStatement: return "EmptyStatement";
    case GrammarKind::IdentifierExpression: return "IdentifierExpression";
    case GrammarKind::LiteralExpression: return "LiteralExpression";
    case GrammarKind::ParenthesizedExpression: return "ParenthesizedExpression";
    case GrammarKind::UnaryExpression: return "UnaryExpression";
    case GrammarKind::BinaryExpression: return "BinaryExpression";
    case GrammarKind::ConditionalExpression: return "ConditionalExpression";
    case GrammarKind::CallExpression: return "CallExpression";
    case GrammarKind::SubscriptExpression: return "SubscriptExpression";
    case GrammarKind::MemberExpression: return "MemberExpression";
    case GrammarKind::LambdaExpression: return "LambdaExpression";
    case GrammarKind::CaseLabel: return "CaseLabel";
    case GrammarKind::TryStatement: return "TryStatement";
    case GrammarKind::DoStatement: return "DoStatement";
    case GrammarKind::TemplateDeclaration: return "TemplateDeclaration";
    case GrammarKind::TemplateArgument: return "TemplateArgument";
    case GrammarKind::TemplateIdExpression: return "TemplateIdExpression";
    case GrammarKind::TypeSpecifier: return "TypeSpecifier";
    case GrammarKind::Declarator: return "Declarator";
    case GrammarKind::DeclaredName: return "DeclaredName";
    case GrammarKind::PointerOperator: return "PointerOperator";
    case GrammarKind::NestedNameSpecifier: return "NestedNameSpecifier";
    case GrammarKind::ArraySuffix: return "ArraySuffix";
    case GrammarKind::FunctionSuffix: return "FunctionSuffix";
    case GrammarKind::TrailingReturnType: return "TrailingReturnType";
    case GrammarKind::NoexceptSpecifier: return "NoexceptSpecifier";
    case GrammarKind::AttributeSpecifier: return "AttributeSpecifier";
    case GrammarKind::BitfieldSuffix: return "BitfieldSuffix";
    case GrammarKind::ModuleDeclaration: return "ModuleDeclaration";
    case GrammarKind::ImportDeclaration: return "ImportDeclaration";
    case GrammarKind::UsingDeclaration: return "UsingDeclaration";
    case GrammarKind::ConceptDefinition: return "ConceptDefinition";
    case GrammarKind::RequiresClause: return "RequiresClause";
    case GrammarKind::RequiresExpression: return "RequiresExpression";
    case GrammarKind::Requirement: return "Requirement";
    case GrammarKind::ErrorExpression: return "ErrorExpression";
    case GrammarKind::Error: return "Error";
    }
    return "Unknown";
}

std::string_view StandardName(cpplsp::CppStandard standard)
{
    switch (standard)
    {
    case cpplsp::CppStandard::Cpp20: return "c++20";
    case cpplsp::CppStandard::Cpp23: return "c++23";
    case cpplsp::CppStandard::Cpp26: return "c++26";
    }
    return "c++20";
}

bool ParseStandardValue(std::string_view value, cpplsp::CppStandard& standard)
{
    if (value == "c++20" || value == "gnu++20" || value == "c++2a" || value == "gnu++2a")
    {
        standard = cpplsp::CppStandard::Cpp20;
        return true;
    }
    if (value == "c++23" || value == "gnu++23" || value == "c++2b" || value == "gnu++2b")
    {
        standard = cpplsp::CppStandard::Cpp23;
        return true;
    }
    if (value == "c++26" || value == "gnu++26" || value == "c++2c" || value == "gnu++2c" ||
        value == "c++latest")
    {
        standard = cpplsp::CppStandard::Cpp26;
        return true;
    }
    return false;
}

cpplsp::ParserOptions ParserOptionsForFile(const fs::path& path, const Options& options,
                                           const cpplsp::CompileDatabase* database)
{
    cpplsp::ParserOptions parser_options;
    if (options.std_override) parser_options.standard = options.standard;
    if (database != nullptr)
    {
        if (const auto* command = database->Find(path); command != nullptr)
        {
            if (!options.std_override) parser_options.standard = command->standard;
            parser_options.predefined_macros = command->defines;
        }
    }
    return parser_options;
}

std::vector<SyntaxDiagnostic> ToSyntaxDiagnostics(std::string_view source,
                                                 const std::vector<cpplsp::GrammarDiagnostic>& grammar)
{
    cpplsp::LineTable lines;
    lines.Build(source);
    std::vector<SyntaxDiagnostic> out;
    out.reserve(grammar.size());
    for (const auto& diagnostic : grammar)
    {
        const auto position = lines.Lookup(diagnostic.offset);
        out.push_back({ position.line, position.column, "CPPLSP900", diagnostic.message });
    }
    return out;
}

bool IsSourceFile(const fs::path& path)
{
    const auto ext = path.extension().string();
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" ||
           ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    if (argc < 2) return false;
    const std::string_view command = argv[1];
    if (command == "lint") options.command = Command::Lint;
    else if (command == "check") options.command = Command::Check;
    else if (command == "format") options.command = Command::Format;
    else if (command == "parse") options.command = Command::Parse;
    else return false;

    for (int i = 2; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--json") options.json = true;
        else if (arg == "--write") options.write = true;
        else if (arg == "--fix") options.fix = true;
        else if (arg == "--semantic") options.semantic = true;
        else if (arg == "--compile-commands" && i + 1 < argc) options.compile_commands = argv[++i];
        else if ((arg == "--std" && i + 1 < argc) ||
                 (arg.starts_with("--std=") && arg.size() > 6))
        {
            const std::string_view value =
                arg.starts_with("--std=") ? std::string_view(arg).substr(6) : std::string_view(argv[++i]);
            if (!ParseStandardValue(value, options.standard))
            {
                std::cerr << "invalid --std value: " << value << " (expected c++20, c++23 or c++26)\n";
                return false;
            }
            options.std_override = true;
        }
        else if (arg == "--jobs" && i + 1 < argc)
        {
            const std::string_view value = argv[++i];
            std::size_t jobs = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), jobs);
            if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || jobs == 0)
            {
                std::cerr << "invalid --jobs value: " << value << '\n';
                return false;
            }
            options.jobs = jobs;
        }
        else if (!arg.empty() && arg.front() == '-')
        {
            std::cerr << "unknown option: " << arg << '\n';
            return false;
        }
        else
        {
            options.inputs.emplace_back(arg);
        }
    }
    if (options.inputs.empty())
    {
        std::cerr << "no input paths provided\n";
        return false;
    }
    if (options.json && (options.command == Command::Format))
    {
        std::cerr << "--json is only supported by lint/check/parse\n";
        return false;
    }
    if (options.json && options.fix)
    {
        std::cerr << "--json cannot be combined with --fix\n";
        return false;
    }
    if (options.write && options.command != Command::Format)
    {
        std::cerr << "--write is only supported by format\n";
        return false;
    }
    if (options.fix && options.command != Command::Lint)
    {
        std::cerr << "--fix is only supported by lint\n";
        return false;
    }
    if (options.semantic && options.compile_commands.empty())
    {
        std::cerr << "--semantic requires --compile-commands <path>\n";
        return false;
    }
    return true;
}

bool CollectFiles(const std::vector<fs::path>& inputs, std::vector<fs::path>& files)
{
    for (const auto& input : inputs)
    {
        std::error_code ec;
        if (!fs::exists(input, ec) || ec)
        {
            std::cerr << "path does not exist: " << input.string() << '\n';
            return false;
        }
        if (fs::is_regular_file(input, ec))
        {
            if (IsSourceFile(input)) files.push_back(input);
            else std::cerr << "skipping unsupported file: " << input.string() << '\n';
        }
        else if (fs::is_directory(input, ec))
        {
            for (fs::recursive_directory_iterator it(input, ec), end; it != end && !ec; it.increment(ec))
            {
                if (it->is_regular_file(ec) && IsSourceFile(it->path())) files.push_back(it->path());
            }
            if (ec)
            {
                std::cerr << "error traversing directory " << input.string() << ": " << ec.message() << '\n';
                return false;
            }
        }
        else
        {
            std::cerr << "not a regular file or directory: " << input.string() << '\n';
            return false;
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty())
    {
        std::cerr << "no C++ source files found\n";
        return false;
    }
    return true;
}

bool WriteFile(const fs::path& path, std::string_view data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::string JsonEscape(std::string_view text)
{
    std::string out;
    for (const unsigned char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            }
            else out += static_cast<char>(c);
        }
    }
    return out;
}

void ProcessFile(const fs::path& path, const Options& options, const cpplsp::CompileDatabase* database,
                 FileResult& result)
{
    result.path = path;
    auto buffer = cpplsp::MappedBuffer::Open(path.string());
    if (!buffer)
    {
        result.error = buffer.error();
        return;
    }
    const std::string_view source = buffer->view();
    if (options.command == Command::Parse || options.command == Command::Lint ||
        options.command == Command::Check)
    {
        const auto parser_options = ParserOptionsForFile(path, options, database);
        const auto tree = cpplsp::ParseTree::Parse(source, parser_options);
        result.standard_name = std::string(StandardName(tree.Standard()));
        result.syntax_diagnostics = ToSyntaxDiagnostics(source, tree.Diagnostics());
        if (options.command == Command::Parse)
        {
            result.nodes.reserve(tree.Nodes().size());
            for (const auto& node : tree.Nodes())
            {
                std::size_t offset = source.size();
                std::size_t length = 0;
                if (node.first_token < tree.Tokens().size() && node.token_count > 0)
                {
                    const std::size_t last_index =
                        std::min(node.first_token + node.token_count, tree.Tokens().size()) - 1;
                    offset = tree.Tokens()[node.first_token].offset;
                    const auto& last_token = tree.Tokens()[last_index];
                    length = last_token.offset + last_token.length - offset;
                }
                result.nodes.push_back({ std::string(GrammarKindName(node.kind)),
                                         node.parent,
                                         offset,
                                         length });
            }
            return;
        }
    }
    if (options.semantic && database != nullptr)
    {
        const auto* command = database->Find(path);
        if (command != nullptr)
        {
            result.has_semantic_context = true;
            const cpplsp::SemanticAnalyzer analyzer;
            const auto types = analyzer.CollectTypeNames(source, command);
            result.type_count = types.size();
            result.semantic_diagnostics = analyzer.AnalyzeUnusedLocals(source, command);
            std::unordered_set<std::string> no_values;
            std::size_t line_start = 0;
            while (line_start < source.size())
            {
                std::size_t line_end = source.find('\n', line_start);
                if (line_end == std::string_view::npos) line_end = source.size();
                const auto line = source.substr(line_start, line_end - line_start);
                switch (analyzer.ClassifyAsteriskStatement(line, types, no_values))
                {
                case cpplsp::AsteriskMeaning::Declaration: ++result.declaration_count; break;
                case cpplsp::AsteriskMeaning::Multiplication: break;
                case cpplsp::AsteriskMeaning::Ambiguous: ++result.ambiguous_count; break;
                case cpplsp::AsteriskMeaning::NotApplicable: break;
                }
                line_start = line_end == source.size() ? source.size() : line_end + 1;
            }
        }
    }
    if (options.command == Command::Format)
    {
        result.output = cpplsp::Formatter().Format(source);
        result.changed = result.output != source;
    }
    else
    {
        result.diagnostics = cpplsp::RuleEngine().Analyze(source);
        if (options.fix && !result.diagnostics.empty())
        {
            result.output = cpplsp::RuleEngine::ApplyFixes(source, result.diagnostics);
            result.changed = result.output != source;
        }
    }
}

void RunParallel(const std::vector<fs::path>& files, const Options& options,
                 const cpplsp::CompileDatabase* database, std::vector<FileResult>& results)
{
    results.resize(files.size());
    std::atomic_size_t next { 0 };
    const std::size_t count = std::min(options.jobs, files.size());
    std::vector<std::jthread> workers;
    workers.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        workers.emplace_back([&]() {
            while (true)
            {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= files.size()) break;
                ProcessFile(files[index], options, database, results[index]);
            }
        });
    }
    workers.clear();
}

} // namespace

int main(int argc, char** argv)
{
    Options options {};
    if (!ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: cpplsp <lint|check|format|parse> [--jobs N] [--json|--fix|--write] [--std <c++20|c++23|c++26>] [--compile-commands <path>] <files-or-directories...>\n";
        return 2;
    }

    std::vector<fs::path> files;
    if (!CollectFiles(options.inputs, files)) return 2;

    cpplsp::CompileDatabase database;
    const cpplsp::CompileDatabase* database_ptr = nullptr;
    if (!options.compile_commands.empty())
    {
        auto loaded = cpplsp::CompileDatabase::Load(options.compile_commands);
        if (!loaded)
        {
            std::cerr << loaded.error() << '\n';
            return 2;
        }
        database = std::move(*loaded);
        database_ptr = &database;
    }
    if (options.command == Command::Format && !options.write && files.size() != 1)
    {
        std::cerr << "format without --write requires exactly one file\n";
        return 2;
    }

    std::vector<FileResult> results;
    RunParallel(files, options, database_ptr, results);

    bool failed = false;
    bool has_diagnostics = false;
    const auto severity_name = [](cpplsp::Severity severity) {
        return severity == cpplsp::Severity::Error ? "error" : "warning";
    };
    if (options.command == Command::Parse && options.json)
    {
        std::cout << '[';
        bool first_file = true;
        for (const auto& result : results)
        {
            if (!result.error.empty())
            {
                std::cerr << result.path.string() << ": " << result.error << '\n';
                failed = true;
                continue;
            }
            if (!first_file) std::cout << ',';
            first_file = false;
            std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"standard\":\""
                      << JsonEscape(result.standard_name) << "\",\"nodes\":[";
            bool first_node = true;
            for (const auto& node : result.nodes)
            {
                if (!first_node) std::cout << ',';
                first_node = false;
                std::cout << "{\"kind\":\"" << JsonEscape(node.kind) << "\",\"parent\":" << node.parent
                          << ",\"offset\":" << node.offset << ",\"length\":" << node.length << '}';
            }
            std::cout << "],\"diagnostics\":[";
            bool first_diag = true;
            for (const auto& diagnostic : result.syntax_diagnostics)
            {
                if (!first_diag) std::cout << ',';
                first_diag = false;
                std::cout << "{\"line\":" << diagnostic.line << ",\"column\":" << diagnostic.column
                          << ",\"severity\":\"error\",\"code\":\"" << JsonEscape(diagnostic.code)
                          << "\",\"message\":\"" << JsonEscape(diagnostic.message) << "\"}";
                has_diagnostics = true;
            }
            std::cout << "]}";
        }
        std::cout << "]\n";
    }
    else if (options.json)
    {
        std::cout << '[';
        bool first = true;
        for (const auto& result : results)
        {
            if (!result.error.empty())
            {
                std::cerr << result.path.string() << ": " << result.error << '\n';
                failed = true;
                continue;
            }
            if (options.semantic)
            {
                if (result.has_semantic_context)
                {
                    std::cerr << result.path.string() << ": semantic context: " << result.type_count
                              << " types, " << result.declaration_count << " pointer declarations, "
                              << result.ambiguous_count << " unresolved forms\n";
                }
                else
                {
                    std::cerr << result.path.string() << ": no matching compile command; semantic checks skipped\n";
                }
            }
            for (const auto& diagnostic : result.diagnostics)
            {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                          << ",\"column\":" << diagnostic.column << ",\"severity\":\"warning\",\"code\":\""
                          << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                          << "\"}";
                has_diagnostics = true;
            }
            for (const auto& diagnostic : result.semantic_diagnostics)
            {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                          << ",\"column\":" << diagnostic.column << ",\"severity\":\"warning\",\"code\":\""
                          << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                          << "\"}";
                has_diagnostics = true;
            }
            for (const auto& diagnostic : result.syntax_diagnostics)
            {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                          << ",\"column\":" << diagnostic.column << ",\"severity\":\"error\",\"code\":\""
                          << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                          << "\"}";
                has_diagnostics = true;
            }
        }
        std::cout << "]\n";
    }
    else if (options.command == Command::Parse)
    {
        for (const auto& result : results)
        {
            if (!result.error.empty())
            {
                std::cerr << result.path.string() << ": " << result.error << '\n';
                failed = true;
                continue;
            }
            for (const auto& diagnostic : result.syntax_diagnostics)
            {
                std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                          << ": error " << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            has_diagnostics |= !result.syntax_diagnostics.empty();
            std::cout << result.path.string() << ": parsed " << result.nodes.size() << " nodes (standard "
                      << result.standard_name << "): " << result.syntax_diagnostics.size() << " syntax errors\n";
        }
    }
    else
    {
        for (const auto& result : results)
        {
            if (!result.error.empty())
            {
                std::cerr << result.path.string() << ": " << result.error << '\n';
                failed = true;
                continue;
            }
            if (options.semantic)
            {
                if (result.has_semantic_context)
                {
                    std::cerr << result.path.string() << ": semantic context: " << result.type_count
                              << " known types, " << result.declaration_count << " pointer declarations, "
                              << result.ambiguous_count << " unresolved forms\n";
                }
                else
                {
                    std::cerr << result.path.string() << ": no matching compile command; semantic checks skipped\n";
                }
            }
            if (options.command == Command::Format)
            {
                if (options.write)
                {
                    if (result.changed && !WriteFile(result.path, result.output))
                    {
                        std::cerr << result.path.string() << ": failed to write formatted file\n";
                        failed = true;
                    }
                }
                else
                {
                    std::cout << result.output;
                }
                continue;
            }

            has_diagnostics |= !result.diagnostics.empty();
            for (const auto& diagnostic : result.diagnostics)
            {
                std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column << ": "
                          << severity_name(diagnostic.severity) << ' ' << diagnostic.code << ": "
                          << diagnostic.message << '\n';
            }
            for (const auto& diagnostic : result.semantic_diagnostics)
            {
                std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                          << ": warning " << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            for (const auto& diagnostic : result.syntax_diagnostics)
            {
                std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                          << ": error " << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            has_diagnostics |= !result.semantic_diagnostics.empty() || !result.syntax_diagnostics.empty();
            if (options.fix && result.changed && !WriteFile(result.path, result.output))
            {
                std::cerr << result.path.string() << ": failed to write fixes\n";
                failed = true;
            }
        }
    }

    if (failed) return 2;
    if ((options.command == Command::Check || options.command == Command::Parse) && has_diagnostics) return 1;
    return 0;
}
