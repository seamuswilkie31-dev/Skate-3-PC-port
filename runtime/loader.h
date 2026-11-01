#pragma once

#include "memory.h"
#include "xex_info.h"

#include <filesystem>
#include <string>
#include <vector>

namespace s3n {

struct LoadedGame {
    XexInfo xex;
    uint32_t entryPoint = 0;
    size_t functionsMapped = 0;      // recompiled functions in the indirect-call table
    size_t functionImports = 0;      // kernel/XAM functions the game imports
    std::vector<std::string> variables;  // imported kernel variables given zeroed storage
};

// Decrypts and decompresses default.xex into guest memory at its base address, fills
// the indirect-call table, and points every imported variable at runtime storage.
bool LoadGame(GuestMemory& memory, const std::filesystem::path& xexPath, LoadedGame& game, std::string& error);

}  // namespace s3n
