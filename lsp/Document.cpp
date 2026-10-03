#include "Document.hpp"

#include <algorithm>

namespace heimdall::lsp
{

Position ToPosition(std::string_view text, std::size_t offset)
{
    Position position;
    const std::size_t stop = offset < text.size() ? offset : text.size();
    for (std::size_t i = 0; i < stop;)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n')
        {
            ++position.line;
            position.character = 0;
            ++i;
            continue;
        }
        if ((c & 0x80) == 0)
        {
            ++position.character;
            ++i;
        }
        else
        {
            std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            std::uint32_t codepoint = c & (width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
            for (std::size_t j = 1; j < width && i + j < stop; ++j)
            {
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
            }
            position.character += codepoint > 0xffff ? 2 : 1;
            i += width;
        }
    }
    return position;
}

std::filesystem::path PathFromUri(std::string_view uri)
{
    constexpr std::string_view prefix = "file://";
    if (uri.starts_with(prefix)) uri.remove_prefix(prefix.size());
    std::string decoded;
    decoded.reserve(uri.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0)
        {
            decoded += static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        }
        else decoded += uri[i];
    }
#if defined(_WIN32)
    if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(decoded.begin());
    std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
    return std::filesystem::path(decoded);
}

void AppendPosition(Position position, std::string& out)
{
    out += "{\"line\":" + std::to_string(position.line) + ",\"character\":" +
           std::to_string(position.character) + "}";
}

std::size_t OffsetFromPosition(std::string_view text, Position position)
{
    std::size_t line = 0;
    std::size_t character = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        if (line == position.line && character == position.character) return i;
        // Mirror ToPosition: '\n' is the only line break; '\r' counts as a column.
        if (line == position.line && character > position.character) return i;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n')
        {
            // A position past end-of-line clamps to the newline itself.
            if (line == position.line) return i;
            ++line;
            character = 0;
            ++i;
            continue;
        }
        if (line > position.line) return i;
        if ((c & 0x80) == 0)
        {
            ++character;
            ++i;
        }
        else
        {
            std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (i + width > text.size()) width = text.size() - i;
            std::uint32_t codepoint = c & (width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
            for (std::size_t j = 1; j < width; ++j)
            {
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
            }
            character += codepoint > 0xffff ? 2 : 1;
            i += width;
        }
    }
    return text.size();
}

} // namespace heimdall::lsp
