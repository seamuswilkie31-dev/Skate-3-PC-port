#include "xdvdfs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace s3n {
namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct DiscLayout {
    uint64_t offset;
    const char* name;
};
// Game partition offsets of the known disc layouts; 0 = trimmed (xiso) image.
constexpr DiscLayout kLayouts[] = {
    {0x0, "XDVDFS (trimmed)"},
    {0xFD90000, "XGD2"},
    {0x2080000, "XGD3"},
    {0x18300000, "XGD1"},
};

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

bool XdvdfsImage::Open(const std::filesystem::path& path, std::string* error) {
    file_.open(path, std::ios::binary);
    if (!file_) {
        if (error) *error = "Can't open the file.";
        return false;
    }
    uint8_t descriptor[kSector];
    for (const DiscLayout& layout : kLayouts) {
        if (!ReadRaw(layout.offset + 32 * kSector, descriptor, sizeof descriptor)) continue;
        if (std::memcmp(descriptor, kMagic, 20) != 0) continue;
        base_ = layout.offset;
        layout_ = layout.name;
        rootSector_ = Le32(descriptor + 20);
        rootSize_ = Le32(descriptor + 24);
        return true;
    }
    if (error) *error = "No Xbox 360 filesystem found. Is this an Xbox 360 disc image?";
    return false;
}

bool XdvdfsImage::ReadRaw(uint64_t offset, void* dst, size_t size) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return file_.gcount() == static_cast<std::streamsize>(size);
}

bool XdvdfsImage::Read(const Entry& file, uint64_t offset, void* dst, size_t size) {
    if (offset + size > file.size) return false;
    return ReadRaw(base_ + file.sector * kSector + offset, dst, size);
}

std::vector<XdvdfsImage::Entry> XdvdfsImage::ListDirectory(uint32_t sector, uint32_t size) {
    std::vector<Entry> out;
    if (size < 14 || size > (16u << 20)) return out;
    std::vector<uint8_t> data(size);
    if (!ReadRaw(base_ + sector * kSector, data.data(), size)) return out;
    if (Le16(&data[0]) == 0xFFFF && Le16(&data[2]) == 0xFFFF) return out;  // empty directory

    // In-order walk of the directory's binary tree. Child offsets are in dwords; 0 = none.
    auto child = [](uint16_t dwords) -> std::optional<size_t> {
        return dwords && dwords != 0xFFFF ? std::optional<size_t>(size_t(dwords) * 4) : std::nullopt;
    };
    std::vector<size_t> stack;
    std::unordered_set<size_t> seen;
    std::optional<size_t> node = 0;
    while (node || !stack.empty()) {
        while (node) {
            size_t off = *node;
            if (off + 14 > size || !seen.insert(off).second) break;
            stack.push_back(off);
            node = child(Le16(&data[off]));
        }
        if (stack.empty()) break;
        size_t off = stack.back();
        stack.pop_back();
        const uint8_t* e = &data[off];
        uint8_t nameLength = e[13];
        if (off + 14 + nameLength <= size)
            out.push_back({std::string(reinterpret_cast<const char*>(e + 14), nameLength), Le32(e + 4),
                           Le32(e + 8), e[12]});
        node = child(Le16(e + 2));
    }
    return out;
}

std::optional<XdvdfsImage::Entry> XdvdfsImage::Find(std::string_view path) {
    std::vector<Entry> entries = Root();
    for (;;) {
        while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.remove_prefix(1);
        size_t cut = path.find_first_of("/\\");
        std::string_view part = path.substr(0, cut);
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const Entry& e) { return EqualsNoCase(e.name, part); });
        if (it == entries.end()) return std::nullopt;
        if (cut == std::string_view::npos) return *it;
        if (!it->IsDirectory()) return std::nullopt;
        path.remove_prefix(cut);
        entries = List(*it);
    }
}

}  // namespace s3n
