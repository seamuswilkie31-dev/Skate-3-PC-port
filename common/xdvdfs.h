#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace s3n {

// Read-only access to XDVDFS, the filesystem on Xbox 360 game discs.
// Handles full XGD2/XGD3 rips and trimmed (xiso) images.
class XdvdfsImage {
public:
    struct Entry {
        std::string name;
        uint32_t sector = 0;
        uint32_t size = 0;
        uint8_t attributes = 0;
        bool IsDirectory() const { return attributes & 0x10; }
    };

    bool Open(const std::filesystem::path& path, std::string* error = nullptr);
    const char* Layout() const { return layout_; }

    std::vector<Entry> Root() { return ListDirectory(rootSector_, rootSize_); }
    std::vector<Entry> List(const Entry& dir) { return ListDirectory(dir.sector, dir.size); }
    std::optional<Entry> Find(std::string_view path);  // case-insensitive, '/' or '\'
    bool Read(const Entry& file, uint64_t offset, void* dst, size_t size);

    // Calls visit(path, entry) for every file and directory, depth first.
    template <class F> void Walk(F&& visit) { WalkImpl(Root(), std::string(), visit, 0); }

private:
    template <class F>
    void WalkImpl(const std::vector<Entry>& entries, const std::string& prefix, F& visit, int depth) {
        if (depth > 32) return;
        for (const Entry& e : entries) {
            std::string path = prefix + e.name;
            visit(path, e);
            if (e.IsDirectory()) WalkImpl(List(e), path + "/", visit, depth + 1);
        }
    }

    std::vector<Entry> ListDirectory(uint32_t sector, uint32_t size);
    bool ReadRaw(uint64_t offset, void* dst, size_t size);

    std::ifstream file_;
    uint64_t base_ = 0;
    const char* layout_ = "";
    uint32_t rootSector_ = 0;
    uint32_t rootSize_ = 0;
};

}  // namespace s3n
