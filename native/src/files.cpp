#include "files.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <system_error>

namespace coop::files {

namespace fs = std::filesystem;

bool IsCompleteCinder(const uint8_t* data, size_t size) {
    constexpr size_t header = 10;
    constexpr size_t entrySize = 12;
    if (size < header || memcmp(data, "cndr", 4) != 0) return false;
    const size_t count = data[6] | (static_cast<size_t>(data[7]) << 8);
    if (count == 0 || count > (size - header) / entrySize) return false;
    const size_t tableEnd = header + count * entrySize;
    const auto read32 = [](const uint8_t* p) -> uint32_t {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    for (size_t at = header; at < tableEnd; at += entrySize) {
        const size_t length = read32(data + at + 4);
        const size_t offset = read32(data + at + 8);
        if (offset < tableEnd || offset > size || length > size - offset) return false;
    }
    return true;
}

bool WriteAtomic(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(temp, error);
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp, error);
        return false;
    }
    return true;
}

}  // namespace coop::files
