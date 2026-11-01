#pragma once

#include "xex_info.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace s3n {

inline constexpr uint32_t kSkate3TitleId = 0x454108E6;
inline constexpr uint32_t kSkate3RetailMediaId = 0x5C087C2C;  // USA / Europe disc

struct GameCheck {
    bool readable = false;  // default.xex was found and its headers parsed
    bool isSkate3 = false;
    std::string error;
    std::string layout;  // "XGD2", "Extracted folder", ...
    size_t fileCount = 0;
    uint64_t totalBytes = 0;
    XexInfo xex;
};

// Identify the game in an ISO image or an extracted game folder.
GameCheck InspectGame(const std::filesystem::path& isoOrFolder);

}  // namespace s3n
