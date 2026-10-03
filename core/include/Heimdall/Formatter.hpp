#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace heimdall
{

    struct FormatOptions
    {
        std::size_t indent_width = 4;
        bool use_tabs = false;
        std::size_t max_empty_lines = 1;
    };

    class Formatter
    {
    public:
        explicit Formatter(FormatOptions options = {}) : m_options(options) {}

        std::string Format(std::string_view source) const;
        std::string Format(const ParseTree& tree) const;

    private:
        std::string FormatImpl(std::string_view source, const std::vector<Token>& tokens,
        const std::vector<PreprocessorDirective>& directives) const;
        FormatOptions m_options;
    };

} // namespace heimdall
