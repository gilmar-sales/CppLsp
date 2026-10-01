#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace cpplsp
{

// Read-only view of a file backed by a memory mapping when possible,
// with a buffered-read fallback. Move-only; the mapping lifetime is
// tied to this object, so any string_view derived from view() must not
// outlive it.
class MappedBuffer
{
  public:
    MappedBuffer() = default;
    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;
    MappedBuffer(MappedBuffer&& other) noexcept;
    MappedBuffer& operator=(MappedBuffer&& other) noexcept;
    ~MappedBuffer();

    static std::expected<MappedBuffer, std::string> Open(const char* path);
    static std::expected<MappedBuffer, std::string> Open(const std::string& path);

    const char* data() const noexcept { return m_data; }
    std::size_t size() const noexcept { return m_size; }
    std::string_view view() const noexcept { return { m_data, m_size }; }
    bool empty() const noexcept { return m_size == 0; }

  private:
    void Release();
    static std::expected<MappedBuffer, std::string> OpenBuffered(const char* path);

    const char* m_data = "";
    std::size_t m_size = 0;

    // Platform mapping handles (opaque to callers). Null when the
    // fallback buffered path is used.
    void* m_file_handle = nullptr;
    void* m_map_handle = nullptr;

    // Fallback storage when mapping is unavailable.
    std::string m_owned;
};

} // namespace cpplsp
