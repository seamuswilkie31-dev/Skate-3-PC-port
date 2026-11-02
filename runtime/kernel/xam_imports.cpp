// XAM: the system services library titles link against (xam.xex). Notifications,
// enumerations, and the helpers the other XAM files share (xam.h).

#include "kernel/xam.h"

#include "kernel/kernel.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace s3n::kernel {
namespace {

std::atomic<uint32_t> gTitleId{0};
constexpr uint32_t kEnumeratorHeader = 0x10;  // the dispatcher header; a private area follows

struct Enumerator {
    uint32_t itemSize = 0;
    std::vector<uint8_t> items;
    size_t next = 0;  // the next item to hand out
};
std::mutex gEnumeratorsMutex;
std::unordered_map<uint32_t, Enumerator> gEnumerators;  // by object

void ForgetEnumerator(uint32_t object) {
    std::lock_guard lock(gEnumeratorsMutex);
    gEnumerators.erase(object);
}

// A notification listener: the areas it wants (bit n for area n, the top bits of an ID),
// and what's queued for it. Its object is signaled while the queue isn't empty.
struct Listener {
    uint64_t areas = 0;
    std::deque<std::pair<uint32_t, uint32_t>> queue;  // ID, parameter
};
std::mutex gListenersMutex;
std::unordered_map<uint32_t, Listener> gListeners;  // by object

void ForgetListener(uint32_t object) {
    std::lock_guard lock(gListenersMutex);
    gListeners.erase(object);
}

}  // namespace

void Notify(uint32_t id, uint32_t parameter) {
    std::lock_guard lock(gListenersMutex);
    for (auto& [object, listener] : gListeners) {
        if (!(listener.areas >> (id >> 25 & 0x3F) & 1)) continue;
        listener.queue.emplace_back(id, parameter);
        SignalObject(object, 1);
    }
}

void NotifyAfter(uint32_t id, uint32_t parameter, uint32_t milliseconds) {
    std::thread([=] {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        Notify(id, parameter);
    }).detach();
}

void SetTitleId(uint32_t titleId) { gTitleId = titleId; }
uint32_t TitleId() { return gTitleId; }

// XOVERLAPPED: InternalLow (the result), InternalHigh (length), InternalContext, event,
// completion routine, its context, extended error.
uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length) {
    if (!overlapped) return result;
    uint8_t* base = Memory().base();
    StoreBE32(base, overlapped, result);
    StoreBE32(base, overlapped + 4, length);
    StoreBE32(base, overlapped + 0x18, result);
    if (uint32_t event = LoadBE32(base, overlapped + 0xC))
        if (uint32_t object = LookupHandle(event)) SignalObject(object, 1);
    // A completion routine runs as an APC on the calling thread: (result, length, overlapped).
    if (uint32_t routine = LoadBE32(base, overlapped + 0x10))
        QueueUserApc(CurrentThread(), routine, result, length, overlapped);
    return kErrorIoPending;
}

uint32_t NewEnumerator(uint32_t itemSize, std::vector<uint8_t> items, uint32_t privateSize) {
    uint32_t object = NewObject(kEnumeratorHeader + privateSize, kNotificationEvent, 1, ForgetEnumerator);
    if (!object) return 0;
    {
        std::lock_guard lock(gEnumeratorsMutex);
        gEnumerators[object] = Enumerator{itemSize, std::move(items), 0};
    }
    uint32_t handle = CreateHandle(object);
    DereferenceObject(object);
    return handle;
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// HANDLE XamNotifyCreateListener(u64 areas, u32 version)
// A listener is waitable: it's signaled while notifications are queued for it.
PPC_FUNC(__imp__XamNotifyCreateListener) {
    Trace(ctx, "XamNotifyCreateListener", true);
    uint64_t areas = ctx.r3.u64;
    uint32_t listener = NewObject(0x10, kNotificationEvent, 0, ForgetListener);
    if (!listener) {
        ctx.r3.u64 = 0;
        return;
    }
    {
        std::lock_guard lock(gListenersMutex);
        gListeners[listener].areas = areas;
    }
    ctx.r3.u64 = CreateHandle(listener);
    DereferenceObject(listener);
}

// BOOL XNotifyGetNext(HANDLE listener, u32 id, u32* idOut, u32* parameter)
// The oldest notification queued for the listener (of that ID, unless ID is 0).
PPC_FUNC(__imp__XNotifyGetNext) {
    Trace(ctx, "XNotifyGetNext", true);
    uint32_t object = LookupHandle(ctx.r3.u32), wanted = ctx.r4.u32, idOut = ctx.r5.u32, parameterOut = ctx.r6.u32;
    std::lock_guard lock(gListenersMutex);
    auto it = gListeners.find(object);
    if (it == gListeners.end()) {
        ctx.r3.u64 = 0;
        return;
    }
    auto& queue = it->second.queue;
    auto match = std::find_if(queue.begin(), queue.end(), [wanted](auto& n) { return !wanted || n.first == wanted; });
    if (match == queue.end()) {
        ctx.r3.u64 = 0;
        return;
    }
    if (idOut) StoreBE32(base, idOut, match->first);
    if (parameterOut) StoreBE32(base, parameterOut, match->second);
    queue.erase(match);
    if (queue.empty()) SignalObject(object, 0);
    ctx.r3.u64 = 1;
}

// u32 XamEnumerate(HANDLE enumerator, u32 flags, void* buffer, u32 size, u32* returned,
//         XOVERLAPPED* overlapped)
// Hands out as many of the enumeration's items as fit, then ERROR_NO_MORE_FILES.
PPC_FUNC(__imp__XamEnumerate) {
    Trace(ctx, "XamEnumerate", true);
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32, returned = ctx.r7.u32, overlapped = ctx.r8.u32;
    uint32_t object = LookupHandle(ctx.r3.u32), result = kErrorNoMoreFiles, count = 0;
    {
        std::lock_guard lock(gEnumeratorsMutex);
        auto it = gEnumerators.find(object);
        if (it != gEnumerators.end() && it->second.itemSize) {
            Enumerator& e = it->second;
            size_t left = (e.items.size() / e.itemSize) - e.next;
            count = uint32_t(std::min<size_t>(left, size / e.itemSize));
            if (left && !count) {
                result = kErrorInsufficientBuffer;
            } else if (count) {
                std::memcpy(base + buffer, e.items.data() + e.next * e.itemSize, size_t(count) * e.itemSize);
                e.next += count;
                result = kErrorSuccess;
            }
        }
    }
    if (returned) StoreBE32(base, returned, count);
    ctx.r3.u64 = CompleteOverlapped(overlapped, result, count);
}

// u32 XamCreateEnumeratorHandle(u32 user, u32 app, u32 openMessage, u32 closeMessage,
//         u32 privateSize, u32 count, u32 flags, HANDLE* enumerator)
// An enumeration another XAM app would fill (the marketplace's offers, say), with a private
// area for the title's request. None of those apps exist, so it's empty.
PPC_FUNC(__imp__XamCreateEnumeratorHandle) {
    Trace(ctx, "XamCreateEnumeratorHandle", true);
    uint32_t privateSize = std::min(ctx.r7.u32, 0x10000u), out = ctx.r10.u32;
    uint32_t handle = NewEnumerator(1, {}, privateSize);
    if (out) StoreBE32(base, out, handle);
    ctx.r3.u64 = handle ? kErrorSuccess : 0x8;  // ERROR_NOT_ENOUGH_MEMORY
}

// u32 XamGetPrivateEnumStructureFromHandle(HANDLE enumerator, void** privateArea)
PPC_FUNC(__imp__XamGetPrivateEnumStructureFromHandle) {
    Trace(ctx, "XamGetPrivateEnumStructureFromHandle", true);
    uint32_t object = LookupHandle(ctx.r3.u32);
    bool known;
    {
        std::lock_guard lock(gEnumeratorsMutex);
        known = gEnumerators.count(object) != 0;
    }
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, known ? object + kEnumeratorHeader : 0);
    ctx.r3.u64 = known ? kErrorSuccess : 0x6;  // ERROR_INVALID_HANDLE
}

// Messages to XAM's apps (the dashboard, Live, the marketplace): none of them run here, so
// a request finishes at once, successfully, with its buffer untouched. Each app and message
// pair is logged the first time, to show what the title asks for.
namespace {

std::mutex gMessagesMutex;
std::unordered_map<uint64_t, uint32_t> gMessages;  // app << 32 | message -> times

void NoteMessage(const char* call, uint32_t app, uint32_t message, uint32_t size) {
    std::lock_guard lock(gMessagesMutex);
    if (gMessages[uint64_t(app) << 32 | message]++ == 0)
        Log("  t%u %s: app %02X, message %08X, %u-byte buffer (answered at once)\n", CurrentThreadNumber(), call, app,
            message, size);
}

}  // namespace

// u32 XMsgStartIORequest(u32 app, u32 message, XOVERLAPPED* overlapped, void* buffer, u32 size)
PPC_FUNC(__imp__XMsgStartIORequest) {
    Trace(ctx, "XMsgStartIORequest", true);
    NoteMessage("XMsgStartIORequest", ctx.r3.u32, ctx.r4.u32, ctx.r7.u32);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, kErrorSuccess);
}

// u32 XMsgStartIORequestEx(u32 app, u32 message, XOVERLAPPED* overlapped, void* buffer,
//         u32 size, void* extra)
PPC_FUNC(__imp__XMsgStartIORequestEx) {
    Trace(ctx, "XMsgStartIORequestEx", true);
    NoteMessage("XMsgStartIORequestEx", ctx.r3.u32, ctx.r4.u32, ctx.r7.u32);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, kErrorSuccess);
}

// u32 XMsgInProcessCall(u32 app, u32 message, void* param1, void* param2)
PPC_FUNC(__imp__XMsgInProcessCall) {
    Trace(ctx, "XMsgInProcessCall", true);
    NoteMessage("XMsgInProcessCall", ctx.r3.u32, ctx.r4.u32, 0);
    ctx.r3.u64 = kErrorSuccess;
}

// u32 XMsgCancelIORequest(XOVERLAPPED* overlapped, BOOL wait): everything has finished.
PPC_FUNC(__imp__XMsgCancelIORequest) {
    Trace(ctx, "XMsgCancelIORequest", true);
    ctx.r3.u64 = kErrorSuccess;
}