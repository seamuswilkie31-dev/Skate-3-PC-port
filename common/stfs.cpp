#include "stfs.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace s3n {
namespace {

constexpr size_t kContentType = 0x344;
constexpr size_t kTitleId = 0x360;
constexpr size_t kDisplayName = 0x411;  // first locale; 0x80 bytes of UTF-16BE
constexpr size_t kTitleName = 0x1691;   // 0x80 bytes of UTF-16BE
constexpr size_t kHeaderBytes = 0x1711;

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

std::string Utf16BeToUtf8(const uint8_t* p, size_t bytes) {
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t c = uint32_t(p[i]) << 8 | p[i + 1];
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < bytes) {
            uint32_t lo = uint32_t(p[i + 2]) << 8 | p[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | (c >> 12 & 0x3F));
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

bool ReadStfsInfo(const std::filesystem::path& path, StfsInfo& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("Can't open the file.");
    std::vector<uint8_t> h(kHeaderBytes);
    in.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()));
    if (in.gcount() != static_cast<std::streamsize>(h.size())) return fail("Too small to be an Xbox 360 package.");

    out.magic.assign(reinterpret_cast<const char*>(h.data()), 4);
    if (out.magic != "CON " && out.magic != "LIVE" && out.magic != "PIRS")
        return fail("Not an Xbox 360 save package (expected a CON file copied from the console).");
    out.contentType = Be32(&h[kContentType]);
    out.titleId = Be32(&h[kTitleId]);
    out.displayName = Utf16BeToUtf8(&h[kDisplayName], 0x80);
    out.titleName = Utf16BeToUtf8(&h[kTitleName], 0x80);
    return true;
}

}  // namespace s3n
