#include "files.h"

#include <windows.h>

#include <fstream>
#include <system_error>

namespace coop::files {

namespace fs = std::filesystem;

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
