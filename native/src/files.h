// Writing files that must not be left half-written (saves, a guest's character): the data goes to a temporary file
// next to the target, which then replaces it in one step.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace coop::files {

// Creates the parent folders, writes `data` and replaces `path` with it. False (and `path` untouched) on failure.
bool WriteAtomic(const std::filesystem::path& path, const uint8_t* data, size_t size);

}  // namespace coop::files
