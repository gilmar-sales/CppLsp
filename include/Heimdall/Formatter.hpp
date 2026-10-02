#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace heimdall
{

struct FormatOptions
{
    std::size_t indent_width = 4;
    bool use_tabs = false;
};

class Formatter
{
  public:
    explicit Formatter(FormatOptions options = {}) : m_options(options) {}

    // Normalizes leading indentation according to brace nesting. All other
    // source bytes are preserved except leading indentation and whitespace-only
    // blank lines. Directives are copied unchanged and braces within trivia or
    // literals are not interpreted as structure.
    std::string Format(std::string_view source) const;

  private:
    FormatOptions m_options;
};

} // namespace heimdall
