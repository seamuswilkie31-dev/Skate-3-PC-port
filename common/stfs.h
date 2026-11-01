#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace s3n {

inline constexpr uint32_t kStfsSavedGame = 0x00000001;

// Header fields of an Xbox 360 STFS package (CON / LIVE / PIRS), e.g. a game save
// copied off a console. Reading the files inside the package comes later.
struct StfsInfo {
    std::string magic;  // "CON ", "LIVE" or "PIRS"
    uint32_t contentType = 0;
    uint32_t titleId = 0;
    std::string displayName;  // UTF-8
    std::string titleName;    // UTF-8
};

bool ReadStfsInfo(const std::filesystem::path& path, StfsInfo& out, std::string* error = nullptr);

}  // namespace s3n
