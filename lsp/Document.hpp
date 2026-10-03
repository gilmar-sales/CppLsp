#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace heimdall::lsp
{

struct Position
{
    std::size_t line = 0;
    std::size_t character = 0;
};

struct Document
{
    std::string text;
    std::int64_t version = 0;
};

Position ToPosition(std::string_view text, std::size_t offset);
std::size_t OffsetFromPosition(std::string_view text, Position position);
std::filesystem::path PathFromUri(std::string_view uri);
void AppendPosition(Position position, std::string& out);

} // namespace heimdall::lsp
