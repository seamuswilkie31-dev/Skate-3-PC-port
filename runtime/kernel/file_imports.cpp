// Files, served read-only from the game disc.
//
// Guest paths go through symbolic links (ObCreateSymbolicLink) and then the disc's
// device names: d:\, game:\ and \Device\Cdrom0. Other devices (the hard disk, cache
// partitions) aren't served yet, so opens there fail with "path not found".
//
// A file is a kernel object with a handle like any other, and is always signaled: every
// read completes before the call returns, then sets the caller's event and queues its APC.

#include "kernel/kernel.h"
#include "disc.h"
#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint32_t kFileObjectSize = 0x20;
constexpr uint32_t kUnexpectedIoError = 0xC00000E9;  // STATUS_UNEXPECTED_IO_ERROR
constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;  // byte offset "at the current position"

// NtCreateFile dispositions and options.
constexpr uint32_t kFileOpen = 1;
constexpr uint32_t kFileCreate = 2;
constexpr uint32_t kFileOpenIf = 3;
constexpr uint32_t kFileDirectoryFile = 0x01;
constexpr uint32_t kFileNonDirectoryFile = 0x40;
constexpr uint32_t kFileOpened = 1;  // IO_STATUS_BLOCK information

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

constexpr uint32_t kSector = 2048;
constexpr size_t kLoggedOpens = 2000;  // log this many opens, then only failures

struct OpenFile {
    uint32_t node = 0;
    std::string path;  // on the disc, lower case, '\' separated, no leading '\'
    uint64_t position = 0;
    bool enumerating = false;  // NtQueryDirectoryFile state
    std::string mask;
    size_t cursor = 0;
    size_t matched = 0;
};

const Disc* gDisc = nullptr;
std::mutex gFilesMutex;
std::unordered_map<uint32_t, OpenFile> gFiles;           // by file object
std::unordered_map<std::string, std::string> gLinks;     // symbolic link name -> target, normalized
std::atomic<size_t> gOpens{0};

void ForgetFile(uint32_t object) {
    std::lock_guard lock(gFilesMutex);
    gFiles.erase(object);
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

// A guest path's location on the disc, or nothing if it's on another device.
std::optional<std::string> DiscPath(const std::string& guestPath) {
    std::string path = StripObjectPrefix(NormalizeGuestPath(guestPath));
    {
        std::lock_guard lock(gFilesMutex);
        for (int depth = 0; depth < 8; ++depth) {
            const std::pair<const std::string, std::string>* best = nullptr;
            for (const auto& link : gLinks)
                if (StartsWithComponent(path, link.first) && (!best || link.first.size() > best->first.size()))
                    best = &link;
            if (!best) break;
            path = StripObjectPrefix(best->second + path.substr(best->first.size()));
        }
    }
    for (const char* device : {"d:", "game:", "\\device\\cdrom0"}) {
        if (StartsWithComponent(path, device)) return Canonical(path.substr(std::string(device).size()));
    }
    return std::nullopt;
}

std::optional<std::string> PathOfOpenFile(uint32_t handle) {
    uint32_t object = LookupHandle(handle);
    std::lock_guard lock(gFilesMutex);
    auto it = gFiles.find(object);
    if (it == gFiles.end()) return std::nullopt;
    return it->second.path;
}

// Resolves an OBJECT_ATTRIBUTES (with its optional root directory handle) to a disc path.
// A root of 0 or 0xFFFFFFFD (the object namespace root) means the name is absolute.
std::optional<std::string> ResolveAttributes(const uint8_t* base, uint32_t attributes, std::string& shown) {
    shown = ObjectName(base, attributes);
    uint32_t root = attributes ? LoadBE32(base, attributes) : 0;
    if (root && root != 0xFFFFFFFD) {
        std::optional<std::string> rootPath = PathOfOpenFile(root);
        if (!rootPath) return std::nullopt;
        return Canonical(*rootPath + "\\" + NormalizeGuestPath(shown));
    }
    return DiscPath(shown);
}

uint32_t Attributes(const Disc::Node& node) { return node.attributes; }
uint64_t AllocationSize(const Disc::Node& node) { return (node.size + kSector - 1) / kSector * kSector; }

void WriteIoStatus(uint8_t* base, uint32_t ioStatus, uint32_t status, uint32_t information) {
    if (!ioStatus) return;
    StoreBE32(base, ioStatus, status);
    StoreBE32(base, ioStatus + 4, information);
}

// FILE_NETWORK_OPEN_INFORMATION: 4 times, allocation size, end of file, attributes, pad.
void WriteNetworkOpenInformation(uint8_t* base, uint32_t info, const Disc::Node& node) {
    for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + i * 8, gDisc->Timestamp());
    StoreBE64(base, info + 0x20, AllocationSize(node));
    StoreBE64(base, info + 0x28, node.size);
    StoreBE32(base, info + 0x30, Attributes(node));
    StoreBE32(base, info + 0x34, 0);
}

// Signals the caller's event and queues its APC, as when asynchronous I/O completes.
void CompleteIo(uint8_t* base, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, uint32_t ioStatus) {
    if (event)
        if (uint32_t object = LookupHandle(event)) SignalObject(object, 1);
    if (apcRoutine) QueueUserApc(CurrentThread(), apcRoutine, apcContext, ioStatus, 0);
}

uint32_t OpenPath(uint8_t* base, const char* caller, uint32_t handlePtr, uint32_t attributes, uint32_t ioStatus,
                  uint32_t disposition, uint32_t options) {
    if (handlePtr) StoreBE32(base, handlePtr, 0);
    std::string shown;
    std::optional<std::string> path = ResolveAttributes(base, attributes, shown);
    uint32_t status = kStatusSuccess;
    std::optional<uint32_t> node;
    const char* outcome = nullptr;
    if (!path) {
        status = kStatusObjectPathNotFound;
        outcome = "not on the disc (no other device is served yet)";
    } else if (!(node = gDisc->Find(*path))) {
        size_t cut = path->rfind('\\');
        bool parentExists = gDisc->Find(cut == std::string::npos ? "" : path->substr(0, cut)).has_value();
        status = parentExists ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
        if (disposition == kFileCreate || disposition == kFileOpenIf || disposition == 0 || disposition > 3)
            status = kStatusMediaWriteProtected;  // it would create a file on the disc
        outcome = "not found";
    } else if (disposition == kFileCreate) {
        status = kStatusObjectNameCollision;
        outcome = "exists (create-only)";
    } else if (disposition != kFileOpen && disposition != kFileOpenIf) {
        status = kStatusMediaWriteProtected;
        outcome = "can't overwrite the disc";
    } else if ((options & kFileDirectoryFile) && !gDisc->At(*node).directory) {
        status = kStatusNotADirectory;
        outcome = "not a directory";
    } else if ((options & kFileNonDirectoryFile) && gDisc->At(*node).directory) {
        status = kStatusFileIsADirectory;
        outcome = "is a directory";
    }

    size_t count = ++gOpens;
    if (status != kStatusSuccess) {
        Log("  t%u %s \"%s\": %s (%08X)\n", CurrentThreadNumber(), caller, shown.c_str(), outcome, status);
        WriteIoStatus(base, ioStatus, status, 0);
        return status;
    }
    const Disc::Node& found = gDisc->At(*node);
    if (count <= kLoggedOpens)
        Log("  t%u %s \"%s\": %s, %llu bytes\n", CurrentThreadNumber(), caller, shown.c_str(),
            found.directory ? "directory" : "file", found.size);

    uint32_t object = NewObject(kFileObjectSize, kFileObject, 1, ForgetFile);
    if (!object) {
        WriteIoStatus(base, ioStatus, kStatusNoMemory, 0);
        return kStatusNoMemory;
    }
    {
        std::lock_guard lock(gFilesMutex);
        gFiles[object] = OpenFile{*node, *path};
    }
    if (handlePtr) StoreBE32(base, handlePtr, CreateHandle(object));
    DereferenceObject(object);
    WriteIoStatus(base, ioStatus, kStatusSuccess, kFileOpened);
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

// Reads `length` bytes at `offset` into guest memory, a page list or one buffer.
int64_t ReadFromDisc(uint8_t* base, uint32_t node, uint64_t offset, uint32_t buffer, uint32_t segments,
                     uint32_t length) {
    if (!segments) return gDisc->Read(node, offset, base + buffer, length);
    int64_t total = 0;
    for (uint32_t page = 0; total < length; ++page) {
        uint32_t chunk = std::min<uint32_t>(0x1000, length - uint32_t(total));
        int64_t got = gDisc->Read(node, offset + total, base + LoadBE32(base, segments + page * 4), chunk);
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
    uint32_t node = 0;
    uint64_t offset = 0;
    bool directory = false;
    if (!WithFile(handle, [&](OpenFile& f) {
            node = f.node;
            offset = f.position;
        })) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    directory = gDisc->At(node).directory;
    if (offsetPtr) {
        uint64_t requested = LoadBE64(base, offsetPtr);
        if (requested != kUseFilePointer && requested != ~0ull) offset = requested;
    }
    uint32_t status;
    int64_t got = 0;
    if (directory) {
        status = kStatusInvalidDeviceRequest;
    } else {
        got = ReadFromDisc(base, node, offset, scatter ? 0 : buffer, scatter ? buffer : 0, length);
        status = got < 0 ? kUnexpectedIoError : (got == 0 && length > 0) ? kStatusEndOfFile : kStatusSuccess;
        if (got > 0) WithFile(handle, [&](OpenFile& f) { f.position = offset + uint64_t(got); });
    }
    WriteIoStatus(base, ioStatus, status, got > 0 ? uint32_t(got) : 0);
    CompleteIo(base, event, apc, apcContext, ioStatus);
    ctx.r3.u64 = status;
}

}  // namespace

void MountDisc(const Disc* disc) { gDisc = disc; }

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
    std::optional<std::string> path = ResolveAttributes(base, ctx.r3.u32, shown);
    std::optional<uint32_t> node = path ? gDisc->Find(*path) : std::nullopt;
    if (!node) {
        Log("  t%u NtQueryFullAttributesFile \"%s\": not found\n", CurrentThreadNumber(), shown.c_str());
        ctx.r3.u64 = path ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
        return;
    }
    WriteNetworkOpenInformation(base, ctx.r4.u32, gDisc->At(*node));
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

// NTSTATUS NtWriteFile(...): the disc is read-only.
PPC_FUNC(__imp__NtWriteFile) {
    Trace(ctx, "NtWriteFile", true);
    std::optional<std::string> path = PathOfOpenFile(ctx.r3.u32);
    Log("  t%u NtWriteFile to \"%s\": the disc is read-only\n", CurrentThreadNumber(), path ? path->c_str() : "?");
    WriteIoStatus(base, ctx.r7.u32, kStatusMediaWriteProtected, 0);
    ctx.r3.u64 = path ? kStatusMediaWriteProtected : kStatusInvalidHandle;
}

// NTSTATUS NtFlushBuffersFile(HANDLE file, IO_STATUS_BLOCK* ioStatus)
PPC_FUNC(__imp__NtFlushBuffersFile) {
    Trace(ctx, "NtFlushBuffersFile", true);
    WriteIoStatus(base, ctx.r4.u32, kStatusSuccess, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                                 u32 length, u32 infoClass)
PPC_FUNC(__imp__NtQueryInformationFile) {
    Trace(ctx, "NtQueryInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    OpenFile file;
    if (!WithFile(ctx.r3.u32, [&](OpenFile& f) { file = f; })) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    const Disc::Node& node = gDisc->At(file.node);
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
        for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + i * 8, gDisc->Timestamp());
        StoreBE32(base, info + 0x20, Attributes(node));
        break;
    case kFileStandardInformation:  // allocation size, end of file, links, delete pending, directory
        StoreBE64(base, info, AllocationSize(node));
        StoreBE64(base, info + 8, node.size);
        StoreBE32(base, info + 16, 1);
        base[info + 21] = node.directory ? 1 : 0;
        break;
    case kFileInternalInformation: StoreBE64(base, info, file.node); break;
    case kFilePositionInformation: StoreBE64(base, info, file.position); break;
    case kFileEndOfFileInformation: StoreBE64(base, info, node.size); break;
    case kFileNetworkOpenInformation: WriteNetworkOpenInformation(base, info, node); break;
    }
    WriteIoStatus(base, ioStatus, kStatusSuccess, size);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                               u32 length, u32 infoClass)
PPC_FUNC(__imp__NtSetInformationFile) {
    Trace(ctx, "NtSetInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    uint32_t status = kStatusSuccess;
    bool found = WithFile(ctx.r3.u32, [&](OpenFile& f) {
        switch (infoClass) {
        case kFilePositionInformation:
            if (length < 8) status = kStatusInfoLengthMismatch;
            else f.position = LoadBE64(base, info);
            break;
        case kFileRenameInformation:
        case kFileDispositionInformation:
        case kFileAllocationInformation:
        case kFileEndOfFileInformation:
            status = kStatusMediaWriteProtected;
            break;
        default:
            status = kStatusInvalidInfoClass;
            break;
        }
    });
    if (!found) status = kStatusInvalidHandle;
    if (status == kStatusInvalidInfoClass)
        Log("  t%u NtSetInformationFile: class %u isn't supported\n", CurrentThreadNumber(), infoClass);
    WriteIoStatus(base, ioStatus, status, 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//                                       u32 length, u32 infoClass)
PPC_FUNC(__imp__NtQueryVolumeInformationFile) {
    Trace(ctx, "NtQueryVolumeInformationFile", true);
    uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    if (!PathOfOpenFile(ctx.r3.u32)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
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
        StoreBE32(base, info + 8, 0x53334E31);
        break;
    case kFsSizeInformation:  // total units, available units, sectors per unit, bytes per sector
        StoreBE64(base, info, (gDisc->TotalBytes() + kSector - 1) / kSector);
        StoreBE32(base, info + 16, 1);
        StoreBE32(base, info + 20, kSector);
        break;
    case kFsDeviceInformation:  // FILE_DEVICE_CD_ROM, removable and read-only
        StoreBE32(base, info, 0x02);
        StoreBE32(base, info + 4, 0x03);
        break;
    case kFsAttributeInformation:  // attributes, longest name, file system name
        StoreBE32(base, info + 4, 255);
        StoreBE32(base, info + 8, 4);
        std::memcpy(base + info + 12, "GDFX", 4);
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
    bool found = WithFile(ctx.r3.u32, [&](OpenFile& f) {
        const Disc::Node& dir = gDisc->At(f.node);
        if (!dir.directory) {
            status = kStatusInvalidParameter;
            return;
        }
        if (!f.enumerating || restart) {
            f.enumerating = true;
            f.mask = mask.empty() ? "*" : mask;
            f.cursor = 0;
            f.matched = 0;
        }
        while (f.cursor < dir.children.size() &&
               !MatchesMask(NormalizeGuestPath(gDisc->At(dir.children[f.cursor]).name), f.mask))
            ++f.cursor;
        if (f.cursor >= dir.children.size()) {
            status = f.matched ? kStatusNoMoreFiles : kStatusNoSuchFile;
            return;
        }
        const Disc::Node& entry = gDisc->At(dir.children[f.cursor]);
        uint32_t nameLength = uint32_t(entry.name.size());
        if (0x40 + nameLength > length) {
            status = kStatusBufferOverflow;
            return;
        }
        StoreBE32(base, info + 0x00, 0);
        StoreBE32(base, info + 0x04, uint32_t(f.cursor));
        for (uint32_t i = 0; i < 4; ++i) StoreBE64(base, info + 0x08 + i * 8, gDisc->Timestamp());
        StoreBE64(base, info + 0x28, entry.size);
        StoreBE64(base, info + 0x30, AllocationSize(entry));
        StoreBE32(base, info + 0x38, Attributes(entry));
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
