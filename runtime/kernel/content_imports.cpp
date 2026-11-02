// XAM's content: storage devices and content packages (saves, for now).
//
// There's one storage device, the hard drive. A content package is a folder under
// %APPDATA%\Skate3Native\saves: <XUID>\<title ID>\<content type>\<file name>, which
// XamContentCreateEx mounts as a device under the root name the title chooses
// (save:\ ...), so its files are ordinary file I/O (file_imports.cpp). A package's display
// name is kept beside it, in <file name>.title (UTF-16, as the title gave it).

#include "config.h"
#include "kernel/kernel.h"
#include "kernel/xam.h"
#include "memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint32_t kHardDrive = 1;           // the device ID handed out
constexpr uint32_t kDeviceTypeHardDrive = 1;
// Sizes that also read right as 32-bit numbers (titles compare the low half): 8 GB free
// would look like none.
constexpr uint64_t kDeviceBytes = 0xE0000000;      // 3.5 GB
constexpr uint64_t kDeviceFreeBytes = 0x7FFF0000;  // just under 2 GB
constexpr uint32_t kUserNone = 0xFF;         // content that belongs to the console, not a profile

// XCONTENT_DATA: device ID, content type, display name (128 UTF-16), file name (42 chars).
constexpr uint32_t kContentDataSize = 0x134;
constexpr uint32_t kDisplayName = 0x08;
constexpr uint32_t kDisplayNameBytes = 256;
constexpr uint32_t kFileName = 0x108;
constexpr uint32_t kFileNameBytes = 42;

// XamContentCreateEx's creation dispositions (the low bits of its flags), and what it reports.
constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kOpenAlways = 4, kTruncateExisting = 5;
constexpr uint32_t kContentCreated = 1, kContentOpened = 2;

std::string Hex(uint64_t value, int digits) {
    char text[17];
    std::snprintf(text, sizeof text, "%0*llX", digits, value);
    return text;
}

std::filesystem::path TypeFolder(uint32_t user, uint32_t contentType) {
    uint64_t owner = user == kUserNone ? 0 : kLocalXuid;
    return SavesDir() / Hex(owner, 16) / Hex(TitleId(), 8) / Hex(contentType, 8);
}

// The file name in an XCONTENT_DATA, made safe as one folder name.
std::string FileName(const uint8_t* base, uint32_t data) {
    std::string name(reinterpret_cast<const char*>(base + data + kFileName),
                     strnlen(reinterpret_cast<const char*>(base + data + kFileName), kFileNameBytes));
    for (char& c : name)
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return name;
}

std::filesystem::path PackageFolder(const uint8_t* base, uint32_t user, uint32_t data) {
    return TypeFolder(user, LoadBE32(base, data + 4)) / FileName(base, data);
}

std::filesystem::path TitleFile(const std::filesystem::path& package) {
    std::filesystem::path file = package;
    file += ".title";
    return file;
}

}  // namespace
}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// u32 XamShowDeviceSelectorUI(u32 user, u32 contentType, u32 contentFlags, u64 bytesNeeded,
//         u32* deviceId, XOVERLAPPED* overlapped)
// Picks the hard drive, as a console with one storage device does without asking.
PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
    Trace(ctx, "XamShowDeviceSelectorUI", true);
    if (ctx.r7.u32) StoreBE32(base, ctx.r7.u32, kHardDrive);
    ctx.r3.u64 = CompleteOverlapped(ctx.r8.u32, kErrorSuccess);
}

// u32 XamContentGetDeviceState(u32 deviceId, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentGetDeviceState) {
    Trace(ctx, "XamContentGetDeviceState", true);
    uint32_t result = ctx.r3.u32 == kHardDrive ? kErrorSuccess : kErrorDeviceNotConnected;
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, result);
}

// u32 XamContentGetDeviceData(u32 deviceId, XDEVICE_DATA* data)
// XDEVICE_DATA: device ID, type, total bytes, free bytes, friendly name (27 UTF-16).
PPC_FUNC(__imp__XamContentGetDeviceData) {
    Trace(ctx, "XamContentGetDeviceData", true);
    uint32_t data = ctx.r4.u32;
    if (ctx.r3.u32 != kHardDrive || !data) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    std::memset(base + data, 0, 0x50);
    StoreBE32(base, data, kHardDrive);
    StoreBE32(base, data + 4, kDeviceTypeHardDrive);
    StoreBE64(base, data + 8, kDeviceBytes);
    StoreBE64(base, data + 0x10, kDeviceFreeBytes);
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; ++i) base[data + 0x18 + i * 2 + 1] = uint8_t(name[i]);
    ctx.r3.u64 = kErrorSuccess;
}

// u32 XamContentCreateEx(u32 user, char* rootName, XCONTENT_DATA* data, u32 flags,
//         u32* disposition, u32* licenseMask, u32 cacheSize, u64 contentSize,
//         XOVERLAPPED* overlapped)
// Opens or creates the package's folder and mounts it as rootName:\. The ninth argument
// is on the stack.
PPC_FUNC(__imp__XamContentCreateEx) {
    Trace(ctx, "XamContentCreateEx", true);
    uint32_t user = ctx.r3.u32, data = ctx.r5.u32, flags = ctx.r6.u32;
    uint32_t dispositionOut = ctx.r7.u32, licenseOut = ctx.r8.u32;
    uint32_t overlapped = LoadBE32(base, ctx.r1.u32 + 0x54);
    std::string root(reinterpret_cast<const char*>(base + ctx.r4.u32));
    std::filesystem::path folder = PackageFolder(base, user, data);
    std::error_code error;
    bool exists = std::filesystem::is_directory(folder, error);
    uint32_t disposition = flags & 0xF, result = kErrorSuccess, outcome = kContentOpened;
    if ((disposition == kOpenExisting || disposition == kTruncateExisting) && !exists) {
        result = kErrorFileNotFound;
    } else if (disposition == kCreateNew && exists) {
        result = kErrorAlreadyExists;
    } else {
        if (exists && (disposition == kCreateAlways || disposition == kTruncateExisting)) {
            std::filesystem::remove_all(folder, error);
            exists = false;
        }
        if (!exists) {
            std::filesystem::create_directories(folder, error);
            std::ofstream title(TitleFile(folder), std::ios::binary);
            title.write(reinterpret_cast<const char*>(base + data + kDisplayName), kDisplayNameBytes);
            outcome = kContentCreated;
        }
        MountFolder(root, folder);
    }
    Log("  t%u XamContentCreateEx %s: \"%s\" as %s: (%s)\n", CurrentThreadNumber(),
        result == kErrorSuccess ? (outcome == kContentCreated ? "created" : "opened") : "failed",
        FileName(base, data).c_str(), root.c_str(), PathToUtf8(folder).c_str());
    if (dispositionOut) StoreBE32(base, dispositionOut, result == kErrorSuccess ? outcome : 0);
    if (licenseOut) StoreBE32(base, licenseOut, 0);
    ctx.r3.u64 = CompleteOverlapped(overlapped, result);
}

// u32 XamContentClose(char* rootName, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentClose) {
    Trace(ctx, "XamContentClose", true);
    bool mounted = UnmountFolder(reinterpret_cast<const char*>(base + ctx.r3.u32));
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, mounted ? kErrorSuccess : kErrorFileNotFound);
}

// u32 XamContentFlush(char* rootName, XOVERLAPPED* overlapped): writes are already on disk.
PPC_FUNC(__imp__XamContentFlush) {
    Trace(ctx, "XamContentFlush", true);
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, kErrorSuccess);
}

// u32 XamContentDelete(u32 user, XCONTENT_DATA* data, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentDelete) {
    Trace(ctx, "XamContentDelete", true);
    std::filesystem::path folder = PackageFolder(base, ctx.r3.u32, ctx.r4.u32);
    std::error_code error;
    bool removed = std::filesystem::remove_all(folder, error) > 0;
    std::filesystem::remove(TitleFile(folder), error);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, removed ? kErrorSuccess : kErrorFileNotFound);
}

// u32 XamContentCreateEnumerator(u32 user, u32 deviceId, u32 contentType, u32 flags,
//         u32 count, u32* bufferSize, HANDLE* enumerator)
// The packages of that type, as XCONTENT_DATA.
PPC_FUNC(__imp__XamContentCreateEnumerator) {
    Trace(ctx, "XamContentCreateEnumerator", true);
    uint32_t user = ctx.r3.u32, device = ctx.r4.u32, contentType = ctx.r5.u32, count = ctx.r7.u32;
    uint32_t bufferSize = ctx.r8.u32, out = ctx.r9.u32;
    std::vector<uint8_t> items;
    std::error_code error;
    if (device == 0 || device == kHardDrive) {
        for (const auto& entry : std::filesystem::directory_iterator(TypeFolder(user, contentType), error)) {
            if (!entry.is_directory()) continue;
            std::string name = PathToUtf8(entry.path().filename());
            if (name.size() >= kFileNameBytes) continue;
            size_t at = items.size();
            items.resize(at + kContentDataSize);
            uint8_t* item = items.data() + at;
            StoreBE32(item, 0, kHardDrive);
            StoreBE32(item, 4, contentType);
            std::ifstream title(TitleFile(entry.path()), std::ios::binary);
            title.read(reinterpret_cast<char*>(item + kDisplayName), kDisplayNameBytes);
            std::memcpy(item + kFileName, name.data(), name.size());
        }
    }
    Log("  t%u XamContentCreateEnumerator: %zu packages of type %08X\n", CurrentThreadNumber(),
        items.size() / kContentDataSize, contentType);
    if (bufferSize) StoreBE32(base, bufferSize, std::max(count, 1u) * kContentDataSize);
    uint32_t handle = NewEnumerator(kContentDataSize, std::move(items));
    if (out) StoreBE32(base, out, handle);
    ctx.r3.u64 = handle ? kErrorSuccess : 0x8;  // ERROR_NOT_ENOUGH_MEMORY
}

// u32 XamContentGetCreator(u32 user, XCONTENT_DATA* data, BOOL* isCreator, XUID* creator,
//         XOVERLAPPED* overlapped): the local profile made everything.
PPC_FUNC(__imp__XamContentGetCreator) {
    Trace(ctx, "XamContentGetCreator", true);
    if (ctx.r5.u32) StoreBE32(base, ctx.r5.u32, 1);
    if (ctx.r6.u32) StoreBE64(base, ctx.r6.u32, kLocalXuid);
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, kErrorSuccess);
}

// u32 XamContentSetThumbnail(char* rootName, void* image, u32 size, XOVERLAPPED* overlapped)
// Accepted; there's no dashboard to show it.
PPC_FUNC(__imp__XamContentSetThumbnail) {
    Trace(ctx, "XamContentSetThumbnail", true);
    ctx.r3.u64 = CompleteOverlapped(ctx.r6.u32, kErrorSuccess);
}

// u32 XamContentGetLicenseMask(u32* mask, XOVERLAPPED* overlapped): no licensed content.
PPC_FUNC(__imp__XamContentGetLicenseMask) {
    Trace(ctx, "XamContentGetLicenseMask", true);
    if (ctx.r3.u32) StoreBE32(base, ctx.r3.u32, 0);
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, kErrorSuccess);
}
