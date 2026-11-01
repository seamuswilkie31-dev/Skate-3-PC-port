#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace s3n {

// Plaintext header fields of an XEX2 executable (default.xex).
struct XexInfo {
    uint32_t titleId = 0;
    uint32_t mediaId = 0;
    uint32_t version = 0;
    uint32_t baseVersion = 0;
    uint8_t discNumber = 0;
    uint8_t discCount = 0;
    uint32_t moduleFlags = 0;
    uint16_t encryption = 0;   // 0 none, 1 normal
    uint16_t compression = 0;  // 0 none, 1 basic, 2 normal (LZX), 3 delta
    uint32_t entryPoint = 0;
    uint32_t loadAddress = 0;
    uint32_t imageSize = 0;
    uint32_t region = 0;
    std::string originalPeName;
    uint32_t defaultStackSize = 0;
    // Thread-local storage template: every thread gets dataSize bytes, the first
    // rawDataSize of them copied from rawDataAddress in the image.
    uint32_t tlsSlotCount = 0;
    uint32_t tlsRawDataAddress = 0;
    uint32_t tlsDataSize = 0;
    uint32_t tlsRawDataSize = 0;
};

struct XexImportLibrary {
    std::string name;               // "xboxkrnl.exe", "xam.xex"
    std::vector<uint32_t> records;  // guest addresses of this library's import slots
};

// Size of the full header block given its first 0x18 bytes, or 0 if this isn't XEX2.
uint32_t XexHeaderSize(const uint8_t* data, size_t size);
bool ParseXexHeaders(const uint8_t* data, size_t size, XexInfo& out, std::string* error = nullptr);
bool ParseXexImports(const uint8_t* data, size_t size, std::vector<XexImportLibrary>& out);

std::string FormatXexVersion(uint32_t version);  // "major.minor.build.qfe"
const char* XexCompressionName(uint16_t compression);

}  // namespace s3n
