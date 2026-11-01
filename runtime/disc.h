#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace s3n {

// The game disc as a read-only file tree, from an ISO image or an extracted folder.
// Indexed once when mounted; lookups and reads are thread-safe.
class Disc {
public:
    struct Node {
        std::string name;  // as on the disc; empty for the root
        bool directory = false;
        uint8_t attributes = 0;  // FILE_ATTRIBUTE_* (0x10 directory, 0x80 normal, ...)
        uint64_t size = 0;
        uint64_t isoOffset = 0;      // ISO images: where the file's data starts
        std::filesystem::path host;  // extracted folders: the file on disk
        std::vector<uint32_t> children;  // directories: node indices, in disc order
    };

    ~Disc();
    bool Mount(const std::filesystem::path& source, std::string& error);
    const std::string& Layout() const { return layout_; }
    uint64_t TotalBytes() const { return totalBytes_; }
    size_t FileCount() const { return fileCount_; }
    uint64_t Timestamp() const { return timestamp_; }  // FILETIME of the source, for file times

    // The node at a path relative to the disc root ('\' or '/' separated, any case).
    // The root is node 0 and the empty path.
    std::optional<uint32_t> Find(std::string_view path) const;
    const Node& At(uint32_t index) const { return nodes_[index]; }

    // Reads up to `size` bytes from a file at `offset`. Returns the number read (0 at
    // or past the end), or -1 when the host read fails.
    int64_t Read(uint32_t index, uint64_t offset, void* dst, uint32_t size) const;

private:
    uint32_t AddNode(Node node, const std::string& path);

    std::vector<Node> nodes_;
    std::unordered_map<std::string, uint32_t> byPath_;  // lower case, '\' separated
    std::string layout_;
    uint64_t totalBytes_ = 0;
    size_t fileCount_ = 0;
    uint64_t timestamp_ = 0;
    void* iso_ = nullptr;  // HANDLE of the ISO image
    mutable std::mutex hostMutex_;
    mutable std::unordered_map<uint32_t, void*> hostFiles_;  // extracted folders: open HANDLEs
};

// Lower case with '/' turned into '\', the form Disc and the kernel compare paths in.
std::string NormalizeGuestPath(std::string_view path);

}  // namespace s3n
