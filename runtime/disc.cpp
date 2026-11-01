#include "disc.h"

#include "xdvdfs.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace s3n {
namespace {

constexpr uint8_t kAttributeDirectory = 0x10;
constexpr uint8_t kAttributeNormal = 0x80;

int64_t ReadAt(HANDLE file, uint64_t offset, void* dst, uint32_t size) {
    OVERLAPPED at{};
    at.Offset = uint32_t(offset);
    at.OffsetHigh = uint32_t(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(file, dst, size, &got, &at)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return got;
}

uint64_t FileTimeOf(HANDLE file) {
    FILETIME written{};
    GetFileTime(file, nullptr, nullptr, &written);
    return uint64_t(written.dwHighDateTime) << 32 | written.dwLowDateTime;
}

}  // namespace

std::string NormalizeGuestPath(std::string_view path) {
    std::string out(path);
    for (char& c : out) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Disc::~Disc() {
    if (iso_) CloseHandle(iso_);
    for (auto& [index, handle] : hostFiles_) CloseHandle(handle);
}

uint32_t Disc::AddNode(Node node, const std::string& path) {
    uint32_t index = uint32_t(nodes_.size());
    if (node.directory) {
        node.attributes |= kAttributeDirectory;
    } else {
        ++fileCount_;
        totalBytes_ += node.size;
    }
    nodes_.push_back(std::move(node));
    byPath_[NormalizeGuestPath(path)] = index;
    return index;
}

bool Disc::Mount(const std::filesystem::path& source, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    Node root;
    root.directory = true;
    AddNode(root, "");

    if (fs::is_directory(source, ec)) {
        layout_ = "extracted folder";
        HANDLE folder = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (folder != INVALID_HANDLE_VALUE) {
            timestamp_ = FileTimeOf(folder);
            CloseHandle(folder);
        }
        // Breadth-first, so each directory's children are added together.
        std::vector<std::pair<uint32_t, fs::path>> pending{{0, source}};
        for (size_t i = 0; i < pending.size(); ++i) {
            auto [parent, dir] = pending[i];
            for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
                Node node;
                node.name = e.path().filename().string();
                node.directory = e.is_directory(ec);
                node.attributes = node.directory ? 0 : kAttributeNormal;
                node.size = node.directory ? 0 : e.file_size(ec);
                node.host = e.path();
                std::string path = fs::relative(e.path(), source, ec).string();
                uint32_t index = AddNode(std::move(node), path);
                nodes_[parent].children.push_back(index);
                if (nodes_[index].directory) pending.push_back({index, e.path()});
            }
        }
        return true;
    }

    XdvdfsImage image;
    if (!image.Open(source, &error)) return false;
    layout_ = image.Layout();
    iso_ = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (iso_ == INVALID_HANDLE_VALUE) {
        iso_ = nullptr;
        error = "can't open the disc image for reading";
        return false;
    }
    timestamp_ = FileTimeOf(iso_);
    // Breadth-first, so each directory's children are added together.
    std::vector<std::pair<uint32_t, XdvdfsImage::Entry>> pending;
    std::vector<std::string> paths{""};  // by node index
    auto addChildren = [&](uint32_t parent, const std::vector<XdvdfsImage::Entry>& entries) {
        for (const XdvdfsImage::Entry& e : entries) {
            Node node;
            node.name = e.name;
            node.directory = e.IsDirectory();
            node.attributes = e.attributes ? e.attributes : kAttributeNormal;
            node.size = node.directory ? 0 : e.size;
            node.isoOffset = image.Offset(e);
            std::string path = paths[parent].empty() ? e.name : paths[parent] + "\\" + e.name;
            uint32_t index = AddNode(std::move(node), path);
            paths.resize(nodes_.size());
            paths[index] = path;
            nodes_[parent].children.push_back(index);
            if (nodes_[index].directory) pending.push_back({index, e});
        }
    };
    addChildren(0, image.Root());
    for (size_t i = 0; i < pending.size() && i < 100000; ++i) {
        auto [index, entry] = pending[i];
        addChildren(index, image.List(entry));
    }
    return true;
}

std::optional<uint32_t> Disc::Find(std::string_view path) const {
    std::string key = NormalizeGuestPath(path);
    while (!key.empty() && key.front() == '\\') key.erase(key.begin());
    while (!key.empty() && key.back() == '\\') key.pop_back();
    auto it = byPath_.find(key);
    if (it == byPath_.end()) return std::nullopt;
    return it->second;
}

int64_t Disc::Read(uint32_t index, uint64_t offset, void* dst, uint32_t size) const {
    const Node& node = nodes_[index];
    if (node.directory) return -1;
    if (offset >= node.size) return 0;
    size = uint32_t(std::min<uint64_t>(size, node.size - offset));
    if (iso_) return ReadAt(iso_, node.isoOffset + offset, dst, size);

    HANDLE file;
    {
        std::lock_guard lock(hostMutex_);
        auto it = hostFiles_.find(index);
        if (it == hostFiles_.end()) {
            file = CreateFileW(node.host.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (file == INVALID_HANDLE_VALUE) return -1;
            hostFiles_[index] = file;
        } else {
            file = it->second;
        }
    }
    return ReadAt(file, offset, dst, size);
}

}  // namespace s3n
