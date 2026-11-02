#pragma once

// What the XAM import files share: the local profile (user_imports.cpp), and the
// asynchronous-call and enumeration helpers (xam_imports.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace s3n::kernel {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorFileNotFound = 0x2;
constexpr uint32_t kErrorNoMoreFiles = 0x12;
constexpr uint32_t kErrorInsufficientBuffer = 0x7A;
constexpr uint32_t kErrorAlreadyExists = 0xB7;
constexpr uint32_t kErrorIoPending = 0x3E5;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorNoSuchUser = 0x525;

// The one profile: offline, signed in on user 0. Offline XUIDs start with 0xE000; fixed for
// now, until an imported save's profile chooses it.
constexpr uint64_t kLocalXuid = 0xE000000000000001ull;
bool SignedIn(uint32_t user);
const std::string& PlayerName();  // the launcher's player name, at most 15 characters

// Notifications (XNotifyGetNext): queued for every listener that asked for the ID's area,
// then or (NotifyAfter) a little later. A system dialog posts XN_SYS_UI 1 when it shows and
// 0 when it has closed. Titles pause their own UI in between, and some only start listening
// once the dialog's request has completed, so the 0 comes as the dialog would finish closing.
constexpr uint32_t kXnSysUi = 0x00000009;
constexpr uint32_t kDialogClosingMilliseconds = 500;
void Notify(uint32_t id, uint32_t parameter);
void NotifyAfter(uint32_t id, uint32_t parameter, uint32_t milliseconds);

// The running title's ID (from its XEX), for the paths of its content.
void SetTitleId(uint32_t titleId);
uint32_t TitleId();

// XAM's asynchronous calls finish at once: the XOVERLAPPED (if any) gets the result, its
// extended error and a length, its event is signaled, and its completion routine is queued
// as an APC. Returns what the call returns:
// ERROR_IO_PENDING with an overlapped (it has finished by then), the result without.
uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

// An enumeration for XamEnumerate: a handle over `items`, `itemSize` bytes each, already in
// guest byte order, with a zeroed private area of `privateSize` bytes for the title
// (XamGetPrivateEnumStructureFromHandle). Closed with NtClose.
uint32_t NewEnumerator(uint32_t itemSize, std::vector<uint8_t> items, uint32_t privateSize = 0);

}  // namespace s3n::kernel
