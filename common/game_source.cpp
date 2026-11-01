#include "game_source.h"

#include "xdvdfs.h"

#include <fstream>
#include <functional>
#include <vector>

namespace s3n {
namespace {

using ReadFn = std::function<bool(uint64_t offset, void* dst, size_t size)>;

bool ReadXexHeaderBlock(const ReadFn& read, uint64_t fileSize, std::vector<uint8_t>& out, std::string& error) {
    uint8_t first[0x18];
    if (fileSize < sizeof first || !read(0, first, sizeof first)) {
        error = "default.xex is too small.";
        return false;
    }
    uint32_t headerSize = XexHeaderSize(first, sizeof first);
    if (!headerSize) {
        error = "default.xex is not an XEX2 executable.";
        return false;
    }
    if (headerSize > fileSize || headerSize > (8u << 20)) {
        error = "default.xex has a corrupt header.";
        return false;
    }
    out.resize(headerSize);
    if (!read(0, out.data(), headerSize)) {
        error = "Couldn't read default.xex.";
        return false;
    }
    return true;
}

}  // namespace

GameCheck InspectGame(const std::filesystem::path& source) {
    namespace fs = std::filesystem;
    GameCheck result;
    std::vector<uint8_t> header;
    std::error_code ec;

    if (source.empty()) {
        result.error = "Choose your Skate 3 ISO or game folder.";
        return result;
    }
    if (fs::is_directory(source, ec)) {
        result.layout = "Extracted folder";
        for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                ++result.fileCount;
                result.totalBytes += it->file_size(ec);
            }
        }
        fs::path xexPath = source / "default.xex";
        std::ifstream xex(xexPath, std::ios::binary);
        if (!xex) {
            result.error = "No default.xex in this folder.";
            return result;
        }
        uint64_t size = fs::file_size(xexPath, ec);
        ReadFn read = [&](uint64_t offset, void* dst, size_t n) {
            xex.clear();
            xex.seekg(static_cast<std::streamoff>(offset));
            xex.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
            return xex.gcount() == static_cast<std::streamsize>(n);
        };
        if (!ReadXexHeaderBlock(read, size, header, result.error)) return result;
    } else {
        XdvdfsImage image;
        if (!image.Open(source, &result.error)) return result;
        result.layout = image.Layout();
        image.Walk([&](const std::string&, const XdvdfsImage::Entry& e) {
            if (!e.IsDirectory()) {
                ++result.fileCount;
                result.totalBytes += e.size;
            }
        });
        auto xex = image.Find("default.xex");
        if (!xex) {
            result.error = "No default.xex on this disc image.";
            return result;
        }
        ReadFn read = [&](uint64_t offset, void* dst, size_t n) { return image.Read(*xex, offset, dst, n); };
        if (!ReadXexHeaderBlock(read, xex->size, header, result.error)) return result;
    }

    if (!ParseXexHeaders(header.data(), header.size(), result.xex, &result.error)) return result;
    result.readable = true;
    result.isSkate3 = result.xex.titleId == kSkate3TitleId;
    if (!result.isSkate3) result.error = "This is a different game, not Skate 3.";
    return result;
}

}  // namespace s3n
