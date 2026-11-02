// Files: read-only from the game disc, and read-write in host folders mounted as devices.
//
// Guest paths go through symbolic links (ObCreateSymbolicLink), then to a device: the
// disc (d:\, game:\, \Device\Cdrom0) or a mounted folder (MountFolder: a content package's
// root such as save:\, backed by a folder under %APPDATA%\Skate3Native). Paths on other
// devices fail with "path not found".
//
// A file is a kernel object with a handle like any other, and is always signaled: every
// read and write completes before the call returns, then sets the caller's event and
// queues its APC.

#include "kernel/kernel.h"
#include "config.h"
#include "disc.h"
#include "memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint32_t kFileObjectSize = 0x20;
constexpr uint32_t kUnexpectedIoError = 0xC00000E9;  // STATUS_UNEXPECTED_IO_ERROR
constexpr uint32_t kDiskFull = 0xC000007F;           // STATUS_DISK_FULL
constexpr uint32_t kDirectoryNotEmpty = 0xC0000101;  // STATUS_DIRECTORY_NOT_EMPTY
constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;  // byte offset "at the current position"
constexpr uint64_t kAppendToFile = 0xFFFFFFFFFFFFFFFFull;    // byte offset "at the end"

// NtCreateFile dispositions and options, and what IO_STATUS_BLOCK's information says.
constexpr uint32_t kFileSupersede = 0;
constexpr uint32_t kFileOpen = 1;
constexpr uint32_t kFileCreate = 2;
constexpr uint32_t kFileOpenIf = 3;
constexpr uint32_t kFileOverwrite = 4;
constexpr uint32_t kFileOverwriteIf = 5;
constexpr uint32_t kFileDirectoryFile = 0x01;
constexpr uint32_t kFileNonDirectoryFile = 0x40;
constexpr uint32_t kFileDeleteOnClose = 0x1000;
constexpr uint32_t kFileSuperseded = 0;
constexpr uint32_t kFileOpened = 1;
constexpr uint32_t kFileCreated = 2;
constexpr uint32_t kFileOverwritten = 3;

// FILE_INFORMATION_CLASS and FS_INFORMATION_CLASS values.
constexpr uint32_t kFileBasicInformation = 4;
constexpr uint32_t kFileStandardInformation = 5;
constexpr uint32_t kFileInternalInformation = 6;
constexpr uint32_t kFileRenameInformation = 10;
constexpr uint32_t kFileDispositionInformation = 13;
constexpr uint32_t kFilePositionInformation = 14;
constexpr uint32_t kFileAllocationInformation = 19;
constexpr uint32_t kFileEndOfFileInformation = 20;
constexpr uint32_t kFileNetworkOpenInformation = 34;
constexpr uint32_t kFsVolumeInformation = 1;
constexpr uint32_t kFsSizeInformation = 3;
constexpr uint32_t kFsDeviceInformation = 4;
constexpr uint32_t kFsAttributeInformation = 5;

constexpr uint32_t kAttributeDirectory = 0x10;
constexpr uint32_t kAttributeNormal = 0x80;
constexpr uint32_t kSector = 2048;
constexpr uint32_t kCluster = 0x4000;               // the hard disk's allocation unit
constexpr uint64_t kFolderCapacity = 0x7FFF0000;    // a mounted folder's free space, as the device reports
constexpr size_t kLoggedOpens = 2000;  // log this many opens, then only failures

// Where a guest path leads: the disc ("" device) or a mounted folder ("save:").
struct Location {
    std::string device;
    std::string path;  // '\' separated, no leading '\'; lower case on the disc, as given elsewhere
};

// A guest path with '\' separators and its case kept: files in mounted folders keep the
// names the title gives them, as on the console's storage.
std::string Separators(std::string_view path) {
    std::string out(path);
    for (char& c : out)
        if (c == '/') c = '\\';
    return out;
}

// A directory entry, as NtQueryDirectoryFile reports it (its name in its own case).
struct HostEntry {
    std::string name;
    uint64_t size = 0, time = 0;
    bool directory = false;
};

struct GuestFile {
    Location location;
    uint32_t node = 0;                    // on the disc
    HANDLE host = INVALID_HANDLE_VALUE;   // a file in a mounted folder (directories have none)
    bool directory = false;
    bool deleteOnClose = false;
    uint64_t position = 0;
    bool enumerating = false;  // NtQueryDirectoryFile state
    std::string mask;
    size_t cursor = 0;
    size_t matched = 0;
    std::vector<HostEntry> entries;  // a mounted directory's, taken when enumeration starts
};

const Disc* gDisc = nullptr;
std::mutex gFilesMutex;
std::unordered_map<uint32_t, GuestFile> gFiles;                  // by file object
std::unordered_map<std::string, std::string> gLinks;            // symbolic link name -> target, normalized
std::unordered_map<std::string, std::filesystem::path> gMounts;  // "save:" -> host folder
std::atomic<size_t> gOpens{0};

bool OnDisc(const Location& l) { return l.device.empty(); }

std::filesystem::path HostPath(const Location& l) {
    std::filesystem::path path = gMounts.at(l.device);  // gFilesMutex held, or the mount is in use
    if (!l.path.empty()) path /= PathFromUtf8(l.path);
    return path;
}

uint64_t FileTimeOf(const FILETIME& time) { return uint64_t(time.dwHighDateTime) << 32 | time.dwLowDateTime; }

void ForgetFile(uint32_t object) {
    std::lock_guard lock(gFilesMutex);
    auto it = gFiles.find(object);
    if (it == gFiles.end()) return;
    GuestFile& f = it->second;
    if (f.host != INVALID_HANDLE_VALUE) ::CloseHandle(f.host);
    if (f.deleteOnClose && gMounts.count(f.location.device)) {
        std::error_code error;
        std::filesystem::remove(HostPath(f.location), error);
    }
    gFiles.erase(it);
}

bool StartsWithComponent(const std::string& path, const std::string& prefix) {
    if (prefix.empty() || path.compare(0, prefix.size(), prefix) != 0) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '\\' || prefix.back() == '\\' ||
           prefix.back() == ':';
}

// Drops empty and "." components and applies "..".
std::string Canonical(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('\\', start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out;
    for (const std::string& part : parts) out += (out.empty() ? "" : "\\") + part;
    return out;
}

std::string StripObjectPrefix(std::string path) {
    if (path.rfind("\\??\\", 0) == 0) path.erase(0, 4);
    return path;
}

// Where a guest path leads, or nothing if it's on a device that isn't served. Devices and
// links match whatever the case ("path"); a mounted folder's part keeps it ("original",
// the same length).
std::optional<Location> Locate(const std::string& guestPath) {
    std::string path = StripObjectPrefix(NormalizeGuestPath(guestPath));
    std::string original = StripObjectPrefix(Separators(guestPath));
    std::lock_guard lock(gFilesMutex);
    for (int depth = 0; depth < 8; ++depth) {
        const std::pair<const std::string, std::string>* best = nullptr;
        for (const auto& link : gLinks)
            if (StartsWithComponent(path, link.first) && (!best || link.first.size() > best->first.size()))
                best = &link;
        if (!best) break;
        path = StripObjectPrefix(best->second + path.substr(best->first.size()));
        original = StripObjectPrefix(best->second + original.substr(best->first.size()));
    }
    for (const char* device : {"d:", "game:", "\\device\\cdrom0"}) {
        if (StartsWithComponent(path, device)) return Location{"", Canonical(path.substr(std::string(device).size()))};
    }
    for (const auto& [name, folder] : gMounts)
        if (StartsWithComponent(path, name)) return Location{name, Canonical(original.substr(name.size()))};
    return std::nullopt;
}

std::optional<Location> LocationOfOpenFile(uint32_t handle) {
    uint32_t object = LookupHandle(handle);
    std::lock_guard lock(gFilesMutex);
    auto it = gFiles.find(object);
    if (it == gFiles.end()) return std::nullopt;
    return it->second.location;
}

std::string Shown(const Location& l) { return (l.device.empty() ? "d:" : l.device) + "\\" + l.path; }

// Resolves an OBJECT_ATTRIBUTES (with its optional root directory handle) to a location.
// A root of 0 or 0xFFFFFFFD (the object namespace root) means the name is absolute.
std::optional<Location> ResolveAttributes(const uint8_t* base, uint32_t attributes, std::string& shown) {
    shown = ObjectName(base, attributes);
    uint32_t root = attributes ? LoadBE32(base, attributes) : 0;
    if (root && root != 0xFFFFFFFD) {
        std::optional<Location> rootLocation = LocationOfOpenFile(root);
        if (!rootLocation) return std::nullopt;
        rootLocation->path = Canonical(rootLocation->path + "\\" +
                                       (OnDisc(*rootLocation) ? NormalizeGuestPath(shown) : Separators(shown)));
        return rootLocation;
    }
    return Locate(shown);
}

uint64_t AllocationSize(uint64_t size, uint32_t unit) { return (size + unit - 1) / unit * unit; }

void WriteIoStatus(uint8_t* base, uint32_t ioStatus, uint32_t status, uint32_t information) {
    if (!ioStatus) return;
    StoreBE32(base, ioStatus, status);
    StoreBE32(base, ioStatus + 4, information);
}

// What the file information calls report about a file, wherever it is.
struct Facts {
    uint64_t size = 0, time = 0;
    uint32_t attributes = kAttributeNormal;
    bool directory = false;
    uint64_t allocation() const { return AllocationSize(size, kCluster); }
};

Facts DiscFacts(uint32_t node) {
    const Disc::Node& n = gDisc->At(node);
    Facts facts{n.size, gDisc->Timestamp(), n.attributes, n.directory};
    return facts;
}

std::optional<Facts> HostFacts(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    Facts facts;
    facts.directory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    facts.size = facts.directory ? 0 : uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    facts.time = FileTimeOf(data.ftLastWriteTime);
    facts.attributes = facts.directory ? kAttributeDirectory : kAttributeNormal;
    return facts;
}

std::optional<Facts> FactsOf(const GuestFile& f) {
    if (OnDisc(f.location)) return DiscFacts(f.node);
    return HostFacts(HostPath(f.location));
}

// FILE_NETWORK_OPEN_INFORMATION: 4 times, allocation size, end of file, attributes, pad.
void WriteNetworkOpenInformation(uint8_t* base, uint32_t info, const Facts& facts) {
    for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + i * 8, facts.time);
    StoreBE64(base, info + 0x20, facts.allocation());
    StoreBE64(base, info + 0x28, facts.size);
    StoreBE32(base, info + 0x30, facts.attributes);
    StoreBE32(base, info + 0x34, 0);
}

// Signals the caller's event and queues its APC, as when asynchronous I/O completes.
void CompleteIo(uint8_t* base, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, uint32_t ioStatus) {
    if (event)
        if (uint32_t object = LookupHandle(event)) SignalObject(object, 1);
    if (apcRoutine) QueueUserApc(CurrentThread(), apcRoutine, apcContext, ioStatus, 0);
}

// Opens (or creates) a path on the disc. Sets `node`, or returns why not.
uint32_t OpenOnDisc(const Location& l, uint32_t disposition, uint32_t options, uint32_t& node, const char*& outcome) {
    std::optional<uint32_t> found = gDisc->Find(l.path);
    if (!found) {
        size_t cut = l.path.rfind('\\');
        bool parentExists = gDisc->Find(cut == std::string::npos ? "" : l.path.substr(0, cut)).has_value();
        outcome = "not found";
        if (disposition != kFileOpen && disposition != kFileOverwrite) return kStatusMediaWriteProtected;
        return parentExists ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
    }
    node = *found;
    bool directory = gDisc->At(node).directory;
    if (disposition == kFileCreate) return outcome = "exists (create-only)", kStatusObjectNameCollision;
    if (disposition != kFileOpen && disposition != kFileOpenIf) return outcome = "can't overwrite the disc", kStatusMediaWriteProtected;
    if ((options & kFileDirectoryFile) && !directory) return outcome = "not a directory", kStatusNotADirectory;
    if ((options & kFileNonDirectoryFile) && directory) return outcome = "is a directory", kStatusFileIsADirectory;
    return kStatusSuccess;
}

// Opens (or creates, or overwrites) a path in a mounted folder. Sets `host` and
// `directory`, and `information` to what happened, or returns why not.
uint32_t OpenOnHost(const std::filesystem::path& path, uint32_t disposition, uint32_t options, HANDLE& host,
                    bool& directory, uint32_t& information, const char*& outcome) {
    std::optional<Facts> facts = HostFacts(path);
    bool wantDirectory = options & kFileDirectoryFile;
    if (facts) {
        directory = facts->directory;
        if (disposition == kFileCreate) return outcome = "exists (create-only)", kStatusObjectNameCollision;
        if (wantDirectory && !directory) return outcome = "not a directory", kStatusNotADirectory;
        if ((options & kFileNonDirectoryFile) && directory) return outcome = "is a directory", kStatusFileIsADirectory;
        information = disposition == kFileSupersede                                       ? kFileSuperseded
                      : disposition == kFileOverwrite || disposition == kFileOverwriteIf ? kFileOverwritten
                                                                                          : kFileOpened;
    } else {
        if (!HostFacts(path.parent_path())) return outcome = "no such folder", kStatusObjectPathNotFound;
        if (disposition == kFileOpen || disposition == kFileOverwrite)
            return outcome = "not found", kStatusObjectNameNotFound;
        directory = wantDirectory;
        information = kFileCreated;
        if (directory && !CreateDirectoryW(path.c_str(), nullptr)) return outcome = "can't create", kDiskFull;
    }
    if (directory) return kStatusSuccess;
    DWORD creation = information == kFileCreated ? CREATE_NEW
                     : information == kFileOpened ? OPEN_EXISTING
                                                  : TRUNCATE_EXISTING;
    host = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (host == INVALID_HANDLE_VALUE) return outcome = "can't open on the host", kUnexpectedIoError;
    return kStatusSuccess;
}

uint32_t OpenPath(uint8_t* base, const char* caller, uint32_t handlePtr, uint32_t attributes, uint32_t ioStatus,
                  uint32_t disposition, uint32_t options) {
    if (handlePtr) StoreBE32(base, handlePtr, 0);
    std::string shown;
    std::optional<Location> location = ResolveAttributes(base, attributes, shown);
    GuestFile file;
    uint32_t status, information = kFileOpened;
    const char* outcome = nullptr;
    if (!location) {
        status = kStatusObjectPathNotFound;
        outcome = "not on a device that's served";
    } else if (OnDisc(*location)) {
        status = OpenOnDisc(*location, disposition, options, file.node, outcome);
        if (status == kStatusSuccess) file.directory = gDisc->At(file.node).directory;
    } else {
        std::filesystem::path path;
        {
            std::lock_guard lock(gFilesMutex);
            path = HostPath(*location);
        }
        status = OpenOnHost(path, disposition, options, file.host, file.directory, information, outcome);
    }

    size_t count = ++gOpens;
    if (status != kStatusSuccess) {
        Log("  t%u %s \"%s\": %s (%08X)\n", CurrentThreadNumber(), caller, shown.c_str(), outcome, status);
        WriteIoStatus(base, ioStatus, status, 0);
        return status;
    }
    file.location = *location;
    file.deleteOnClose = !OnDisc(*location) && (options & kFileDeleteOnClose);
    if (count <= kLoggedOpens || !OnDisc(*location))
        Log("  t%u %s \"%s\": %s%s%s\n", CurrentThreadNumber(), caller, shown.c_str(),
            file.directory ? "directory" : "file", information == kFileCreated ? " (created)" : "",
            file.deleteOnClose ? " (delete on close)" : "");

    uint32_t object = NewObject(kFileObjectSize, kFileObject, 1, ForgetFile);
    if (!object) {
        if (file.host != INVALID_HANDLE_VALUE) ::CloseHandle(file.host);
        WriteIoStatus(base, ioStatus, kStatusNoMemory, 0);
        return kStatusNoMemory;
    }
    {
        std::lock_guard lock(gFilesMutex);
        gFiles[object] = std::move(file);
    }
    if (handlePtr) StoreBE32(base, handlePtr, CreateHandle(object));
    DereferenceObject(object);
    WriteIoStatus(base, ioStatus, kStatusSuccess, information);
    return kStatusSuccess;
}

// The open file behind a handle; holds gFilesMutex while `use` runs. False for a bad handle.
template <typename Use>
bool WithFile(uint32_t handle, Use use) {
    uint32_t object = LookupHandle(handle);
    std::lock_guard lock(gFilesMutex);
    auto it = gFiles.find(object);
    if (it == gFiles.end()) return false;
    use(it->second);
    return true;
}

// Case-insensitive wildcard match ('*' and '?'); both sides are already lower case.
bool MatchesMask(const std::string& name, const std::string& mask) {
    size_t n = 0, m = 0, starM = std::string::npos, starN = 0;
    while (n < name.size()) {
        if (m < mask.size() && (mask[m] == '?' || mask[m] == name[n])) {
            ++n;
            ++m;
        } else if (m < mask.size() && mask[m] == '*') {
            starM = m++;
            starN = n;
        } else if (starM != std::string::npos) {
            m = starM + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (m < mask.size() && mask[m] == '*') ++m;
    return m == mask.size();
}

// Reads `length` bytes at `offset` from a host file. Returns the bytes read, or -1.
int64_t ReadHost(HANDLE host, uint64_t offset, uint8_t* out, uint32_t length) {
    OVERLAPPED at{};
    at.Offset = DWORD(offset);
    at.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(host, out, length, &got, &at) && GetLastError() != ERROR_HANDLE_EOF) return -1;
    return got;
}

// Reads `length` bytes at `offset` into guest memory, a page list or one buffer.
int64_t ReadInto(uint8_t* base, const GuestFile& f, uint64_t offset, uint32_t buffer, uint32_t segments,
                 uint32_t length) {
    auto read = [&](uint64_t at, uint8_t* out, uint32_t size) {
        return OnDisc(f.location) ? gDisc->Read(f.node, at, out, size) : ReadHost(f.host, at, out, size);
    };
    if (!segments) return read(offset, base + buffer, length);
    int64_t total = 0;
    for (uint32_t page = 0; total < length; ++page) {
        uint32_t chunk = std::min<uint32_t>(0x1000, length - uint32_t(total));
        int64_t got = read(offset + total, base + LoadBE32(base, segments + page * 4), chunk);
        if (got < 0) return -1;
        total += got;
        if (got < chunk) break;
    }
    return total;
}

// NtReadFile and NtReadFileScatter: (handle, event, apcRoutine, apcContext, ioStatus,
// buffer or page list, length, s64* byteOffset).
void ReadFileImport(PPCContext& ctx, uint8_t* base, bool scatter) {
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, apcContext = ctx.r6.u32;
    uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    uint32_t status = kStatusSuccess;
    int64_t got = 0;
    bool found = WithFile(handle, [&](GuestFile& f) {
        uint64_t offset = f.position;
        if (offsetPtr) {
            uint64_t requested = LoadBE64(base, offsetPtr);
            if (requested != kUseFilePointer && requested != kAppendToFile) offset = requested;
        }
        if (f.directory) {
            status = kStatusInvalidDeviceRequest;
            return;
        }
        got = ReadInto(base, f, offset, scatter ? 0 : buffer, scatter ? buffer : 0, length);
        status = got < 0 ? kUnexpectedIoError : (got == 0 && length > 0) ? kStatusEndOfFile : kStatusSuccess;
        if (got > 0) f.position = offset + uint64_t(got);
        if (!OnDisc(f.location))  // saves: rare enough to log every read
            Log("  t%u read \"%s\" at %llX, %u bytes: %lld\n", CurrentThreadNumber(), Shown(f.location).c_str(),
                offset, length, got);
    });
    if (!found) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    WriteIoStatus(base, ioStatus, status, got > 0 ? uint32_t(got) : 0);
    CompleteIo(base, event, apc, apcContext, ioStatus);
    ctx.r3.u64 = status;
}

// Snapshot of a mounted directory's entries, for NtQueryDirectoryFile.
std::vector<HostEntry> ListHost(const std::filesystem::path& folder) {
    std::vector<HostEntry> entries;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        std::optional<Facts> facts = HostFacts(entry.path());
        if (!facts) continue;
        entries.push_back({PathToUtf8(entry.path().filename()), facts->size, facts->time, facts->directory});
    }
    return entries;
}

}  // namespace

void MountDisc(const Disc* disc) { gDisc = disc; }

void MountFolder(const std::string& name, const std::filesystem::path& folder) {
    std::string device = NormalizeGuestPath(name);
    if (device.empty() || device.back() != ':') device += ':';
    std::lock_guard lock(gFilesMutex);
    gMounts[device] = folder;
}

bool UnmountFolder(const std::string& name) {
    std::string device = NormalizeGuestPath(name);
    if (device.empty() || device.back() != ':') device += ':';
    std::lock_guard lock(gFilesMutex);
    return gMounts.erase(device) != 0;
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// NTSTATUS NtCreateFile(HANDLE* out, u32 access, OBJECT_ATTRIBUTES* attributes,
//                       IO_STATUS_BLOCK* ioStatus, s64* allocationSize, u32 fileAttributes,
//                       u32 shareAccess, u32 disposition, u32 options)
// The ninth argument is on the stack.
PPC_FUNC(__imp__NtCreateFile) {
    Trace(ctx, "NtCreateFile", true);
    uint32_t options = LoadBE32(base, ctx.r1.u32 + 0x54);
    ctx.r3.u64 = OpenPath(base, "NtCreateFile", ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}

// NTSTATUS NtOpenFile(HANDLE* out, u32 access, OBJECT_ATTRIBUTES* attributes,
//                     IO_STATUS_BLOCK* ioStatus, u32 options)
PPC_FUNC(__imp__NtOpenFile) {
    Trace(ctx, "NtOpenFile", true);
    ctx.r3.u64 = OpenPath(base, "NtOpenFile", ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, kFileOpen, ctx.r7.u32);
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attributes, FILE_NETWORK_OPEN_INFORMATION* info)
PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    Trace(ctx, "NtQueryFullAttributesFile", true);
    std::string shown;
    std::optional<Location> location = ResolveAttributes(base, ctx.r3.u32, shown);
    std::optional<Facts> facts;
    if (location && OnDisc(*location)) {
        if (std::optional<uint32_t> node = gDisc->Find(location->path)) facts = DiscFacts(*node);
    } else if (location) {
        std::filesystem::path path;
        {
            std::lock_guard lock(gFilesMutex);
            path = HostPath(*location);
        }
        facts = HostFacts(path);
    }
    if (!facts) {
        Log("  t%u NtQueryFullAttributesFile \"%s\": not found\n", CurrentThreadNumber(), shown.c_str());
        ctx.r3.u64 = location ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
        return;
    }
    WriteNetworkOpenInformation(base, ctx.r4.u32, *facts);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtReadFile(HANDLE file, HANDLE event, void* apcRoutine, void* apcContext,
//                     IO_STATUS_BLOCK* ioStatus, void* buffer, u32 length, s64* byteOffset)
PPC_FUNC(__imp__NtReadFile) {
    Trace(ctx, "NtReadFile", true);
    ReadFileImport(ctx, base, false);
}

// NTSTATUS NtReadFileScatter(HANDLE file, HANDLE event, void* apcRoutine, void* apcContext,
//                            IO_STATUS_BLOCK* ioStatus, u32* pages, u32 length, s64* byteOffset)
// `pages` lists the 4 KB pages to fill, in order.
PPC_FUNC(__imp__NtReadFileScatter) {
    Trace(ctx, "NtReadFileScatter", true);
    ReadFileImport(ctx, base, true);
}

// NTSTATUS NtWriteFile(HANDLE file, HANDLE event, void* apcRoutine, void* apcContext,
//                      IO_STATUS_BLOCK* ioStatus, void* buffer, u32 length, s64* byteOffset)
// Only files in mounted folders can be written; the disc is read-only.
PPC_FUNC(__imp__NtWriteFile) {
    Trace(ctx, "NtWriteFile", true);
    uint32_t event = ctx.r4.u32, apc = ctx.r5.u32, apcContext = ctx.r6.u32, ioStatus = ctx.r7.u32;
    uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    uint32_t status = kStatusSuccess;
    DWORD written = 0;
    std::string shown;
    bool found = WithFile(ctx.r3.u32, [&](GuestFile& f) {
        shown = Shown(f.location);
        if (OnDisc(f.location) || f.directory) {
            status = OnDisc(f.location) ? kStatusMediaWriteProtected : kStatusInvalidDeviceRequest;
            return;
        }
        uint64_t offset = f.position;
        if (offsetPtr) {
            uint64_t requested = LoadBE64(base, offsetPtr);
            if (requested == kAppendToFile) {
                LARGE_INTEGER size;
                offset = GetFileSizeEx(f.host, &size) ? uint64_t(size.QuadPart) : 0;
            } else if (requested != kUseFilePointer) {
                offset = requested;
            }
        }
        OVERLAPPED at{};
        at.Offset = DWORD(offset);
        at.OffsetHigh = DWORD(offset >> 32);
        if (!WriteFile(f.host, base + buffer, length, &written, &at)) status = kDiskFull;
        f.position = offset + written;
        Log("  t%u write \"%s\" at %llX, %u bytes: %u\n", CurrentThreadNumber(), shown.c_str(), offset, length, written);
    });
    if (!found) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (status != kStatusSuccess) Log("  t%u NtWriteFile to \"%s\" failed (%08X)\n", CurrentThreadNumber(), shown.c_str(), status);
    WriteIoStatus(base, ioStatus, status, written);
    CompleteIo(base, event, apc, apcContext, ioStatus);
    ctx.r3.u64 = status;
}

// NTSTATUS NtFlushBuffersFile(HANDLE file, IO_STATUS_BLOCK* ioStatus)
PPC_FUNC(__imp__NtFlushBuffersFile) {
    Trace(ctx, "NtFlushBuffersFile", true);
    WithFile(ctx.r3.u32, [](GuestFile& f) {
        if (f.host != INVALID_HANDLE_VALUE) FlushFileBuffers(f.host);
    });
    WriteIoStatus(base, ctx.r4.u32, kStatusSuccess, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                                 u32 length, u32 infoClass)
PPC_FUNC(__imp__NtQueryInformationFile) {
    Trace(ctx, "NtQueryInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    std::optional<Facts> facts;
    uint64_t position = 0, id = 0;
    if (!WithFile(ctx.r3.u32, [&](GuestFile& f) {
            facts = FactsOf(f);
            position = f.position;
            id = OnDisc(f.location) ? f.node : std::hash<std::string>()(Shown(f.location));
        })) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (!facts) {
        ctx.r3.u64 = kUnexpectedIoError;
        return;
    }
    uint32_t size;
    switch (infoClass) {
    case kFileBasicInformation: size = 40; break;
    case kFileStandardInformation: size = 24; break;
    case kFileInternalInformation:
    case kFilePositionInformation:
    case kFileEndOfFileInformation: size = 8; break;
    case kFileNetworkOpenInformation: size = 56; break;
    default:
        Log("  t%u NtQueryInformationFile: class %u isn't supported\n", CurrentThreadNumber(), infoClass);
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }
    if (length < size) {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }
    for (uint32_t i = 0; i < size; i += 4) StoreBE32(base, info + i, 0);
    switch (infoClass) {
    case kFileBasicInformation:  // 4 times, attributes
        for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + i * 8, facts->time);
        StoreBE32(base, info + 0x20, facts->attributes);
        break;
    case kFileStandardInformation:  // allocation size, end of file, links, delete pending, directory
        StoreBE64(base, info, facts->allocation());
        StoreBE64(base, info + 8, facts->size);
        StoreBE32(base, info + 16, 1);
        base[info + 21] = facts->directory ? 1 : 0;
        break;
    case kFileInternalInformation: StoreBE64(base, info, id); break;
    case kFilePositionInformation: StoreBE64(base, info, position); break;
    case kFileEndOfFileInformation: StoreBE64(base, info, facts->size); break;
    case kFileNetworkOpenInformation: WriteNetworkOpenInformation(base, info, *facts); break;
    }
    WriteIoStatus(base, ioStatus, kStatusSuccess, size);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                               u32 length, u32 infoClass)
// FILE_RENAME_INFORMATION: replace if exists (u8), root directory handle, ANSI_STRING name.
// FILE_DISPOSITION_INFORMATION: delete on close (u8).
PPC_FUNC(__imp__NtSetInformationFile) {
    Trace(ctx, "NtSetInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    uint32_t status = kStatusSuccess;
    // A rename's target, worked out before the file's lock is taken.
    std::optional<Location> target;
    std::string shown;  // a mounted folder's file, for the log
    bool replace = false;
    if (infoClass == kFileRenameInformation && length >= 12) {
        replace = base[info] != 0;
        uint32_t root = LoadBE32(base, info + 4);
        std::string name = Separators(AnsiString(base, info + 8));
        if (root) {
            target = LocationOfOpenFile(root);
            if (target) target->path = Canonical(target->path + "\\" + name);
        } else {
            target = Locate(name);
        }
    }
    bool found = WithFile(ctx.r3.u32, [&](GuestFile& f) {
        if (infoClass == kFilePositionInformation) {
            if (length < 8) status = kStatusInfoLengthMismatch;
            else f.position = LoadBE64(base, info);
            return;
        }
        if (infoClass != kFileRenameInformation && infoClass != kFileDispositionInformation &&
            infoClass != kFileAllocationInformation && infoClass != kFileEndOfFileInformation) {
            status = kStatusInvalidInfoClass;
            return;
        }
        if (OnDisc(f.location)) {
            status = kStatusMediaWriteProtected;
            return;
        }
        shown = Shown(f.location);
        switch (infoClass) {
        case kFileDispositionInformation: f.deleteOnClose = length >= 1 && base[info] != 0; break;
        case kFileAllocationInformation: break;  // space is the host's business
        case kFileEndOfFileInformation: {
            LARGE_INTEGER size;
            size.QuadPart = int64_t(LoadBE64(base, info));
            if (f.host == INVALID_HANDLE_VALUE || !SetFileInformationByHandle(f.host, FileEndOfFileInfo, &size, sizeof size))
                status = kDiskFull;
            break;
        }
        case kFileRenameInformation: {
            if (!target || OnDisc(*target) || target->device != f.location.device) {
                status = kStatusObjectPathNotFound;
                break;
            }
            std::filesystem::path from = HostPath(f.location), to = HostPath(*target);
            if (!MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) {
                status = GetLastError() == ERROR_ALREADY_EXISTS ? kStatusObjectNameCollision : kStatusObjectPathNotFound;
                break;
            }
            f.location = *target;
            break;
        }
        }
    });
    if (!found) status = kStatusInvalidHandle;
    if (status == kStatusInvalidInfoClass)
        Log("  t%u NtSetInformationFile: class %u isn't supported\n", CurrentThreadNumber(), infoClass);
    else if (found && infoClass != kFilePositionInformation && !shown.empty())
        Log("  t%u NtSetInformationFile \"%s\": class %u (%08X)\n", CurrentThreadNumber(), shown.c_str(), infoClass, status);
    WriteIoStatus(base, ioStatus, status, 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                                       u32 length, u32 infoClass)
PPC_FUNC(__imp__NtQueryVolumeInformationFile) {
    Trace(ctx, "NtQueryVolumeInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    std::optional<Location> location = LocationOfOpenFile(ctx.r3.u32);
    if (!location) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    bool disc = OnDisc(*location);
    uint32_t size;
    switch (infoClass) {
    case kFsVolumeInformation: size = 24; break;
    case kFsSizeInformation: size = 24; break;
    case kFsDeviceInformation: size = 8; break;
    case kFsAttributeInformation: size = 16; break;
    default:
        Log("  t%u NtQueryVolumeInformationFile: class %u isn't supported\n", CurrentThreadNumber(), infoClass);
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }
    if (length < size) {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }
    for (uint32_t i = 0; i < size; i += 4) StoreBE32(base, info + i, 0);
    switch (infoClass) {
    case kFsVolumeInformation:  // creation time, serial number, label length, supports objects
        StoreBE64(base, info, gDisc->Timestamp());
        StoreBE32(base, info + 8, disc ? 0x53334E31 : 0x53334E32);
        break;
    case kFsSizeInformation:  // total units, available units, sectors per unit, bytes per sector
        if (disc) {
            StoreBE64(base, info, (gDisc->TotalBytes() + kSector - 1) / kSector);
            StoreBE32(base, info + 16, 1);
            StoreBE32(base, info + 20, kSector);
        } else {  // a mounted folder: plenty of room, in the hard disk's clusters
            StoreBE64(base, info, kFolderCapacity / kCluster);
            StoreBE64(base, info + 8, kFolderCapacity / kCluster);
            StoreBE32(base, info + 16, kCluster / 512);
            StoreBE32(base, info + 20, 512);
        }
        break;
    case kFsDeviceInformation:  // FILE_DEVICE_CD_ROM (removable, read-only) or FILE_DEVICE_DISK
        StoreBE32(base, info, disc ? 0x02 : 0x07);
        StoreBE32(base, info + 4, disc ? 0x03 : 0x00);
        break;
    case kFsAttributeInformation:  // attributes, longest name, file system name
        StoreBE32(base, info + 4, disc ? 255 : 42);
        StoreBE32(base, info + 8, 4);
        std::memcpy(base + info + 12, disc ? "GDFX" : "FATX", 4);
        break;
    }
    WriteIoStatus(base, ioStatus, kStatusSuccess, size);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE directory, HANDLE event, void* apcRoutine,
//                               void* apcContext, IO_STATUS_BLOCK* ioStatus, void* info,
//                               u32 length, ANSI_STRING* mask, BOOL restartScan)
// Returns one FILE_DIRECTORY_INFORMATION per call: next entry offset, index, 4 times, end
// of file, allocation size, attributes, name length, name. The ninth argument is on the stack.
PPC_FUNC(__imp__NtQueryDirectoryFile) {
    Trace(ctx, "NtQueryDirectoryFile", true);
    uint32_t event = ctx.r4.u32, apc = ctx.r5.u32, apcContext = ctx.r6.u32, ioStatus = ctx.r7.u32;
    uint32_t info = ctx.r8.u32, length = ctx.r9.u32, maskPtr = ctx.r10.u32;
    bool restart = LoadBE32(base, ctx.r1.u32 + 0x54) != 0;
    if (length < 0x48) {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }
    std::string mask = NormalizeGuestPath(AnsiString(base, maskPtr));
    uint32_t status = kStatusSuccess, written = 0;
    bool found = WithFile(ctx.r3.u32, [&](GuestFile& f) {
        if (!f.directory) {
            status = kStatusInvalidParameter;
            return;
        }
        if (!f.enumerating || restart) {
            f.enumerating = true;
            f.mask = mask.empty() ? "*" : mask;
            f.cursor = 0;
            f.matched = 0;
            if (!OnDisc(f.location)) f.entries = ListHost(HostPath(f.location));
        }
        // The directory's entries, from the disc or the snapshot.
        size_t count = OnDisc(f.location) ? gDisc->At(f.node).children.size() : f.entries.size();
        auto entryAt = [&](size_t i) {
            if (!OnDisc(f.location)) return f.entries[i];
            const Disc::Node& n = gDisc->At(gDisc->At(f.node).children[i]);
            return HostEntry{n.name, n.size, gDisc->Timestamp(), n.directory};
        };
        while (f.cursor < count && !MatchesMask(NormalizeGuestPath(entryAt(f.cursor).name), f.mask)) ++f.cursor;
        if (f.cursor >= count) {
            status = f.matched ? kStatusNoMoreFiles : kStatusNoSuchFile;
            return;
        }
        HostEntry entry = entryAt(f.cursor);
        uint32_t nameLength = uint32_t(entry.name.size());
        if (0x40 + nameLength > length) {
            status = kStatusBufferOverflow;
            return;
        }
        StoreBE32(base, info + 0x00, 0);
        StoreBE32(base, info + 0x04, uint32_t(f.cursor));
        for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + 0x08 + i * 8, entry.time);
        StoreBE64(base, info + 0x28, entry.size);
        StoreBE64(base, info + 0x30, AllocationSize(entry.size, OnDisc(f.location) ? kSector : kCluster));
        StoreBE32(base, info + 0x38,
                  OnDisc(f.location) ? gDisc->At(gDisc->At(f.node).children[f.cursor]).attributes
                                     : entry.directory ? kAttributeDirectory : kAttributeNormal);
        StoreBE32(base, info + 0x3C, nameLength);
        std::memcpy(base + info + 0x40, entry.name.data(), nameLength);
        written = 0x40 + nameLength;
        ++f.cursor;
        ++f.matched;
    });
    if (!found) status = kStatusInvalidHandle;
    WriteIoStatus(base, ioStatus, status, written);
    if (found) CompleteIo(base, event, apc, apcContext, ioStatus);
    ctx.r3.u64 = status;
}

// NTSTATUS ObCreateSymbolicLink(ANSI_STRING* name, ANSI_STRING* target)
PPC_FUNC(__imp__ObCreateSymbolicLink) {
    Trace(ctx, "ObCreateSymbolicLink", true);
    std::string name = StripObjectPrefix(NormalizeGuestPath(AnsiString(base, ctx.r3.u32)));
    std::string target = NormalizeGuestPath(AnsiString(base, ctx.r4.u32));
    while (!target.empty() && target.back() == '\\') target.pop_back();
    Log("  symbolic link %s -> %s\n", name.c_str(), target.c_str());
    {
        std::lock_guard lock(gFilesMutex);
        gLinks[name] = target;
    }
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS ObDeleteSymbolicLink(ANSI_STRING* name)
PPC_FUNC(__imp__ObDeleteSymbolicLink) {
    Trace(ctx, "ObDeleteSymbolicLink", true);
    std::string name = StripObjectPrefix(NormalizeGuestPath(AnsiString(base, ctx.r3.u32)));
    std::lock_guard lock(gFilesMutex);
    ctx.r3.u64 = gLinks.erase(name) ? kStatusSuccess : kStatusObjectNameNotFound;
}
