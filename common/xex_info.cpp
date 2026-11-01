#include "xex_info.h"

#include <algorithm>
#include <cstring>

namespace s3n {
namespace {

constexpr uint32_t kEntryPoint = 0x00010100;
constexpr uint32_t kFileFormatInfo = 0x000003FF;
constexpr uint32_t kOriginalPeName = 0x000183FF;
constexpr uint32_t kExecutionInfo = 0x00040006;
constexpr uint32_t kImportLibraries = 0x000103FF;
constexpr uint32_t kTlsInfo = 0x00020104;
constexpr uint32_t kDefaultStackSize = 0x00020200;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

}  // namespace

uint32_t XexHeaderSize(const uint8_t* data, size_t size) {
    if (size < 0x18 || std::memcmp(data, "XEX2", 4) != 0) return 0;
    return Be32(data + 8);  // the PE data starts right after the headers
}

bool ParseXexHeaders(const uint8_t* d, size_t n, XexInfo& x, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (n < 0x18 || std::memcmp(d, "XEX2", 4) != 0) return fail("default.xex is not an XEX2 executable.");
    x.moduleFlags = Be32(d + 4);
    uint32_t securityOffset = Be32(d + 0x10);
    uint32_t count = Be32(d + 0x14);
    if (0x18 + uint64_t(count) * 8 > n) return fail("default.xex has a truncated header table.");

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key = Be32(d + 0x18 + i * 8);
        uint32_t value = Be32(d + 0x18 + i * 8 + 4);
        // Keys whose low byte is 0 or 1 hold their value inline; the rest hold an offset.
        auto at = [&](uint64_t length) -> const uint8_t* { return value + length <= n ? d + value : nullptr; };
        switch (key) {
        case kEntryPoint:
            x.entryPoint = value;
            break;
        case kExecutionInfo:
            if (const uint8_t* p = at(24)) {
                x.mediaId = Be32(p);
                x.version = Be32(p + 4);
                x.baseVersion = Be32(p + 8);
                x.titleId = Be32(p + 12);
                x.discNumber = p[18];
                x.discCount = p[19];
            }
            break;
        case kFileFormatInfo:
            if (const uint8_t* p = at(8)) {
                x.encryption = Be16(p + 4);
                x.compression = Be16(p + 6);
            }
            break;
        case kOriginalPeName:
            if (const uint8_t* p = at(4)) {
                uint32_t length = Be32(p);
                if (length > 4 && at(length)) {
                    const char* s = reinterpret_cast<const char*>(p + 4);
                    x.originalPeName.assign(s, std::find(s, s + length - 4, '\0'));
                }
            }
            break;
        case kDefaultStackSize:
            x.defaultStackSize = value;
            break;
        case kTlsInfo:
            if (const uint8_t* p = at(16)) {
                x.tlsSlotCount = Be32(p);
                x.tlsRawDataAddress = Be32(p + 4);
                x.tlsDataSize = Be32(p + 8);
                x.tlsRawDataSize = Be32(p + 12);
            }
            break;
        }
    }

    if (securityOffset && uint64_t(securityOffset) + 0x17C <= n) {
        x.imageSize = Be32(d + securityOffset + 0x4);
        x.loadAddress = Be32(d + securityOffset + 0x110);
        x.region = Be32(d + securityOffset + 0x178);
    }
    if (!x.titleId) return fail("default.xex has no execution info (title ID).");
    return true;
}

bool ParseXexImports(const uint8_t* d, size_t n, std::vector<XexImportLibrary>& out) {
    out.clear();
    if (n < 0x18 || std::memcmp(d, "XEX2", 4) != 0) return false;
    uint32_t count = Be32(d + 0x14);
    for (uint32_t i = 0; i < count && 0x18 + uint64_t(i) * 8 + 8 <= n; ++i) {
        if (Be32(d + 0x18 + i * 8) != kImportLibraries) continue;
        uint64_t offset = Be32(d + 0x18 + i * 8 + 4);
        if (offset + 12 > n) return false;
        const uint8_t* h = d + offset;
        uint32_t total = Be32(h), tableSize = Be32(h + 4), nameCount = Be32(h + 8);
        if (offset + total > n || 12 + uint64_t(tableSize) > total) return false;

        // Library names: NUL-terminated, each padded to a multiple of 4 bytes.
        std::vector<std::string> names;
        for (uint32_t j = 0; j < tableSize && names.size() < nameCount;) {
            const char* s = reinterpret_cast<const char*>(h + 12 + j);
            const char* end = std::find(s, reinterpret_cast<const char*>(h + 12 + tableSize), '\0');
            names.emplace_back(s, end);
            j = (j + uint32_t(end - s) + 1 + 3) & ~3u;
        }

        // Libraries: size, digest[20], id, version, min version, name index, record count, records.
        for (uint64_t at = 12 + uint64_t(tableSize); at + 0x28 <= total;) {
            uint32_t size = Be32(h + at);
            uint16_t nameIndex = uint16_t(h[at + 0x24] << 8 | h[at + 0x25]);
            uint16_t records = uint16_t(h[at + 0x26] << 8 | h[at + 0x27]);
            if (size < 0x28 || at + size > total || 0x28 + uint64_t(records) * 4 > size) return false;
            XexImportLibrary library;
            library.name = nameIndex < names.size() ? names[nameIndex] : "?";
            for (uint32_t r = 0; r < records; ++r) library.records.push_back(Be32(h + at + 0x28 + r * 4));
            out.push_back(std::move(library));
            at += size;
        }
        return true;
    }
    return true;  // no imports
}

std::string FormatXexVersion(uint32_t v) {
    return std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." +
           std::to_string((v >> 8) & 0xFFFF) + "." + std::to_string(v & 0xFF);
}

const char* XexCompressionName(uint16_t compression) {
    switch (compression) {
    case 0: return "none";
    case 1: return "basic";
    case 2: return "LZX";
    case 3: return "delta";
    default: return "unknown";
    }
}

}  // namespace s3n
