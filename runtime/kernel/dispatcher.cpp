// Kernel objects, handles and waits.
//
// Every waitable object is a dispatcher header in guest memory, and its signal state there
// is the real state: title code that initializes or reads objects inline sees the same
// thing the kernel functions do. One dispatcher lock guards all state changes and one
// condition variable wakes waiters, which re-check their objects.
//
// Objects the runtime creates (Nt*Create*, threads) live in system memory and are
// reference counted; handles name them. Objects inside title memory (KEVENTs and the like
// passed to Ke* functions) belong to the title.

#include "kernel/kernel.h"
#include "memory.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint32_t kHeaderSignalState = 0x04;
constexpr uint32_t kHeaderWaitList = 0x08;
constexpr uint32_t kMutantOwner = 0x18;
constexpr uint32_t kMutantSize = 0x20;
constexpr uint32_t kSemaphoreLimit = 0x10;
constexpr uint32_t kSemaphoreSize = 0x14;
constexpr uint32_t kEventSize = 0x10;

constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
constexpr uint32_t kDuplicateCloseSource = 0x1;

// ---- Object pool ----

struct ObjectInfo {
    uint32_t refs = 0;
    uint32_t size = 0;
    std::string name;
};

std::mutex gObjectsMutex;
std::unordered_map<uint32_t, ObjectInfo> gObjects;          // pool objects by address
std::map<uint32_t, std::vector<uint32_t>> gFreeObjects;     // size -> freed addresses
std::unordered_map<std::string, uint32_t> gNamedObjects;    // name -> object

uint32_t PoolSize(uint32_t size) { return (size + 15) & ~15u; }

// ---- Handles ----

std::mutex gHandlesMutex;
std::unordered_map<uint32_t, uint32_t> gHandles;  // handle -> object
uint32_t gNextHandle = 0xF8000004;

// ---- Dispatcher ----

std::mutex gDispatcher;
std::condition_variable gWake;

struct Apc {
    uint32_t routine, r3, r4, r5;
};
std::unordered_map<uint32_t, std::deque<Apc>> gApcs;  // by KTHREAD, under gDispatcher

int32_t State(const uint8_t* base, uint32_t object) { return int32_t(LoadBE32(base, object + kHeaderSignalState)); }
void SetState(uint8_t* base, uint32_t object, int32_t state) {
    StoreBE32(base, object + kHeaderSignalState, uint32_t(state));
}

bool IsSignaled(const uint8_t* base, uint32_t object, uint32_t thread) {
    if (base[object] == kMutantObject && LoadBE32(base, object + kMutantOwner) == thread) return true;
    return State(base, object) > 0;
}

// What a satisfied wait does to the object.
void Acquire(uint8_t* base, uint32_t object, uint32_t thread) {
    switch (base[object]) {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
        SetState(base, object, 0);
        break;
    case kSemaphoreObject:
        SetState(base, object, State(base, object) - 1);
        break;
    case kMutantObject:
        SetState(base, object, State(base, object) - 1);
        StoreBE32(base, object + kMutantOwner, thread);
        break;
    default:  // notification events and timers, threads: stay signaled
        break;
    }
}

// Runs the calling thread's queued user APCs. Returns whether there were any.
bool DeliverApcs(PPCContext& ctx, uint8_t* base, uint32_t thread) {
    bool any = false;
    for (;;) {
        Apc apc;
        {
            std::lock_guard lock(gDispatcher);
            auto it = gApcs.find(thread);
            if (it == gApcs.end() || it->second.empty()) return any;
            apc = it->second.front();
            it->second.pop_front();
        }
        any = true;
        CallGuest(ctx, base, apc.routine, apc.r3, apc.r4, apc.r5);
    }
}

// Nt*Create* for every object type: makes the object (or opens the existing one with
// the same name), stores a handle at *handlePtr. `init` fills in a new object.
template <typename Init>
uint32_t CreateNamed(uint8_t* base, uint32_t handlePtr, uint32_t attributes, uint32_t size, uint8_t type,
                     int32_t state, Init init) {
    std::string name = ObjectName(base, attributes);
    uint32_t object = 0;
    bool existed = false;
    if (!name.empty()) {
        std::lock_guard lock(gObjectsMutex);
        if (auto it = gNamedObjects.find(name); it != gNamedObjects.end()) {
            object = it->second;
            existed = true;
            if (base[object] != type) return kStatusObjectTypeMismatch;
            ++gObjects[object].refs;
        }
    }
    if (!object) {
        object = NewObject(size, type, state);
        if (!object) return kStatusNoMemory;
        init(object);
        if (!name.empty()) {
            std::lock_guard lock(gObjectsMutex);
            gNamedObjects[name] = object;
            gObjects[object].name = name;
        }
    }
    StoreBE32(base, handlePtr, CreateHandle(object));
    DereferenceObject(object);
    return existed ? kStatusObjectNameExists : kStatusSuccess;
}

// Looks up a handle and checks its object's type; 0 if either is wrong.
uint32_t ObjectOfType(const uint8_t* base, uint32_t handle, std::initializer_list<uint8_t> types) {
    uint32_t object = LookupHandle(handle);
    if (!object) return 0;
    for (uint8_t type : types)
        if (base[object] == type) return object;
    return 0;
}

}  // namespace

std::string ObjectName(const uint8_t* base, uint32_t attributes) {
    if (!attributes) return {};
    uint32_t string = LoadBE32(base, attributes + 4);
    if (!string) return {};
    uint16_t length = uint16_t(base[string] << 8 | base[string + 1]);
    uint32_t buffer = LoadBE32(base, string + 4);
    if (!buffer || !length) return {};
    return std::string(reinterpret_cast<const char*>(base + buffer), length);
}

uint32_t NewObject(uint32_t size, uint8_t type, int32_t signalState) {
    uint8_t* base = Memory().base();
    size = PoolSize(size);
    uint32_t object = 0;
    {
        std::lock_guard lock(gObjectsMutex);
        if (auto it = gFreeObjects.find(size); it != gFreeObjects.end() && !it->second.empty()) {
            object = it->second.back();
            it->second.pop_back();
        }
    }
    if (!object) object = Memory().AllocSystem(size);
    if (!object) return 0;
    std::memset(base + object, 0, size);
    base[object] = type;
    base[object + 2] = uint8_t(size / 4);
    SetState(base, object, signalState);
    StoreBE32(base, object + kHeaderWaitList, object + kHeaderWaitList);  // empty list: points at itself
    StoreBE32(base, object + kHeaderWaitList + 4, object + kHeaderWaitList);
    std::lock_guard lock(gObjectsMutex);
    gObjects[object] = ObjectInfo{1, size, {}};
    return object;
}

void ReferenceObject(uint32_t object) {
    std::lock_guard lock(gObjectsMutex);
    if (auto it = gObjects.find(object); it != gObjects.end()) ++it->second.refs;
}

void DereferenceObject(uint32_t object) {
    std::lock_guard lock(gObjectsMutex);
    auto it = gObjects.find(object);
    if (it == gObjects.end() || --it->second.refs > 0) return;
    if (!it->second.name.empty()) gNamedObjects.erase(it->second.name);
    gFreeObjects[it->second.size].push_back(object);
    gObjects.erase(it);
}

uint32_t CreateHandle(uint32_t object) {
    ReferenceObject(object);
    std::lock_guard lock(gHandlesMutex);
    uint32_t handle = gNextHandle;
    gNextHandle += 4;
    gHandles[handle] = object;
    return handle;
}

uint32_t LookupHandle(uint32_t handle) {
    if (handle == kCurrentThreadHandle) return CurrentThread();
    std::lock_guard lock(gHandlesMutex);
    auto it = gHandles.find(handle);
    return it == gHandles.end() ? 0 : it->second;
}

bool CloseHandle(uint32_t handle) {
    uint32_t object;
    {
        std::lock_guard lock(gHandlesMutex);
        auto it = gHandles.find(handle);
        if (it == gHandles.end()) return false;
        object = it->second;
        gHandles.erase(it);
    }
    DereferenceObject(object);
    return true;
}

int32_t SignalObject(uint32_t object, int32_t state) {
    uint8_t* base = Memory().base();
    int32_t previous;
    {
        std::lock_guard lock(gDispatcher);
        previous = State(base, object);
        SetState(base, object, state);
    }
    if (state > 0) gWake.notify_all();
    return previous;
}

void QueueUserApc(uint32_t thread, uint32_t routine, uint32_t r3, uint32_t r4, uint32_t r5) {
    {
        std::lock_guard lock(gDispatcher);
        gApcs[thread].push_back({routine, r3, r4, r5});
    }
    gWake.notify_all();
}

uint32_t WaitForObjects(PPCContext& ctx, uint8_t* base, const char* caller, const uint32_t* objects, uint32_t count,
                        bool waitAll, bool alertable, const Deadline& deadline) {
    uint32_t thread = CurrentThread();
    SetWaitState(caller, count ? objects[0] : 0);
    struct ClearWaitState {
        ~ClearWaitState() { SetWaitState(nullptr, 0); }
    } clear;
    std::unique_lock lock(gDispatcher);
    for (;;) {
        if (count && waitAll) {
            bool all = true;
            for (uint32_t i = 0; i < count && all; ++i) all = IsSignaled(base, objects[i], thread);
            if (all) {
                for (uint32_t i = 0; i < count; ++i) Acquire(base, objects[i], thread);
                return kStatusSuccess;
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (IsSignaled(base, objects[i], thread)) {
                    Acquire(base, objects[i], thread);
                    return kStatusSuccess + i;  // STATUS_WAIT_0 + i
                }
            }
        }
        if (alertable) {
            auto it = gApcs.find(thread);
            if (it != gApcs.end() && !it->second.empty()) {
                lock.unlock();
                DeliverApcs(ctx, base, thread);
                return kStatusUserApc;
            }
        }
        if (deadline) {
            if (std::chrono::steady_clock::now() >= *deadline) return kStatusTimeout;
            gWake.wait_until(lock, *deadline);
        } else {
            gWake.wait(lock);
        }
    }
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

namespace {

// Events, semaphores and mutants are set by handle or by pointer.
uint32_t SetEventObject(uint32_t event) { return uint32_t(SignalObject(event, 1)); }
uint32_t ResetEventObject(uint32_t event) {
    uint8_t* base = Memory().base();
    std::lock_guard lock(gDispatcher);
    int32_t previous = State(base, event);
    SetState(base, event, 0);
    return uint32_t(previous);
}

}  // namespace

// ---- Events ----

// NTSTATUS NtCreateEvent(HANDLE* out, OBJECT_ATTRIBUTES* attributes, u32 type, BOOL initialState)
// type 0: notification (manual reset), 1: synchronization (auto reset).
PPC_FUNC(__imp__NtCreateEvent) {
    Trace(ctx, "NtCreateEvent", true);
    uint8_t type = ctx.r5.u32 ? kSynchronizationEvent : kNotificationEvent;
    ctx.r3.u64 = CreateNamed(base, ctx.r3.u32, ctx.r4.u32, kEventSize, type, ctx.r6.u32 ? 1 : 0, [](uint32_t) {});
}

// NTSTATUS NtSetEvent(HANDLE event, s32* previousState)
PPC_FUNC(__imp__NtSetEvent) {
    Trace(ctx, "NtSetEvent", true);
    uint32_t event = ObjectOfType(base, ctx.r3.u32, {kNotificationEvent, kSynchronizationEvent});
    if (!event) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t previous = SetEventObject(event);
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtClearEvent(HANDLE event)
PPC_FUNC(__imp__NtClearEvent) {
    Trace(ctx, "NtClearEvent", true);
    uint32_t event = ObjectOfType(base, ctx.r3.u32, {kNotificationEvent, kSynchronizationEvent});
    if (!event) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    ResetEventObject(event);
    ctx.r3.u64 = kStatusSuccess;
}

// s32 KeSetEvent(KEVENT* event, s32 increment, BOOL wait): returns the previous state.
PPC_FUNC(__imp__KeSetEvent) {
    Trace(ctx, "KeSetEvent", true);
    ctx.r3.u64 = SetEventObject(ctx.r3.u32);
}

// s32 KeResetEvent(KEVENT* event): returns the previous state.
PPC_FUNC(__imp__KeResetEvent) {
    Trace(ctx, "KeResetEvent", true);
    ctx.r3.u64 = ResetEventObject(ctx.r3.u32);
}

// ---- Semaphores ----

// NTSTATUS NtCreateSemaphore(HANDLE* out, OBJECT_ATTRIBUTES* attributes, s32 count, s32 limit)
PPC_FUNC(__imp__NtCreateSemaphore) {
    Trace(ctx, "NtCreateSemaphore", true);
    int32_t limit = ctx.r6.s32;
    ctx.r3.u64 = CreateNamed(base, ctx.r3.u32, ctx.r4.u32, kSemaphoreSize, kSemaphoreObject, ctx.r5.s32,
                             [&](uint32_t object) { StoreBE32(base, object + kSemaphoreLimit, uint32_t(limit)); });
}

// NTSTATUS NtReleaseSemaphore(HANDLE semaphore, s32 count, s32* previousCount)
PPC_FUNC(__imp__NtReleaseSemaphore) {
    Trace(ctx, "NtReleaseSemaphore", true);
    uint32_t semaphore = ObjectOfType(base, ctx.r3.u32, {kSemaphoreObject});
    int32_t count = ctx.r4.s32;
    if (!semaphore || count <= 0) {
        ctx.r3.u64 = semaphore ? kStatusInvalidParameter : kStatusInvalidHandle;
        return;
    }
    int32_t previous;
    {
        std::lock_guard lock(gDispatcher);
        previous = State(base, semaphore);
        int32_t limit = int32_t(LoadBE32(base, semaphore + kSemaphoreLimit));
        if (int64_t(previous) + count > limit) {
            ctx.r3.u64 = kStatusSemaphoreLimitExceeded;
            return;
        }
        SetState(base, semaphore, previous + count);
    }
    gWake.notify_all();
    if (ctx.r5.u32) StoreBE32(base, ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = kStatusSuccess;
}

// ---- Mutants ----
// Signal state 1 is free; each acquisition by the owner takes it one lower.

// NTSTATUS NtCreateMutant(HANDLE* out, OBJECT_ATTRIBUTES* attributes, BOOL initialOwner)
PPC_FUNC(__imp__NtCreateMutant) {
    Trace(ctx, "NtCreateMutant", true);
    bool owned = ctx.r5.u32 != 0;
    uint32_t thread = CurrentThread();
    ctx.r3.u64 = CreateNamed(base, ctx.r3.u32, ctx.r4.u32, kMutantSize, kMutantObject, owned ? 0 : 1,
                             [&](uint32_t object) {
                                 if (owned) StoreBE32(base, object + kMutantOwner, thread);
                             });
}

// NTSTATUS NtReleaseMutant(HANDLE mutant, s32* previousCount)
PPC_FUNC(__imp__NtReleaseMutant) {
    Trace(ctx, "NtReleaseMutant", true);
    uint32_t mutant = ObjectOfType(base, ctx.r3.u32, {kMutantObject});
    if (!mutant) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    int32_t previous;
    {
        std::lock_guard lock(gDispatcher);
        if (LoadBE32(base, mutant + kMutantOwner) != CurrentThread()) {
            ctx.r3.u64 = kStatusMutantNotOwned;
            return;
        }
        previous = State(base, mutant);
        SetState(base, mutant, previous + 1);
        if (previous + 1 > 0) StoreBE32(base, mutant + kMutantOwner, 0);
    }
    gWake.notify_all();
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = kStatusSuccess;
}

// ---- Waits ----

// NTSTATUS NtWaitForSingleObjectEx(HANDLE handle, u32 waitMode, BOOL alertable, s64* timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    Trace(ctx, "NtWaitForSingleObjectEx", true);
    uint32_t object = LookupHandle(ctx.r3.u32);
    if (!object) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    ReferenceObject(object);  // keeps it alive if another thread closes the handle meanwhile
    ctx.r3.u64 = WaitForObjects(ctx, base, "NtWaitForSingleObjectEx", &object, 1, true, ctx.r5.u32 != 0,
                                GuestTimeout(base, ctx.r6.u32));
    DereferenceObject(object);
}

// NTSTATUS KeWaitForSingleObject(void* object, u32 reason, u32 mode, BOOL alertable, s64* timeout)
PPC_FUNC(__imp__KeWaitForSingleObject) {
    Trace(ctx, "KeWaitForSingleObject", true);
    uint32_t object = ctx.r3.u32;
    ctx.r3.u64 = WaitForObjects(ctx, base, "KeWaitForSingleObject", &object, 1, true, ctx.r6.u32 != 0,
                                GuestTimeout(base, ctx.r7.u32));
}

// NTSTATUS KeWaitForMultipleObjects(u32 count, void** objects, u32 waitType, u32 reason,
//                                   u32 mode, BOOL alertable, s64* timeout, void* waitBlocks)
// waitType 0: all, 1: any.
PPC_FUNC(__imp__KeWaitForMultipleObjects) {
    Trace(ctx, "KeWaitForMultipleObjects", true);
    uint32_t count = ctx.r3.u32;
    if (count == 0 || count > 64) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; ++i) objects[i] = LoadBE32(base, ctx.r4.u32 + i * 4);
    ctx.r3.u64 = WaitForObjects(ctx, base, "KeWaitForMultipleObjects", objects, count, ctx.r5.u32 == 0,
                                ctx.r8.u32 != 0, GuestTimeout(base, ctx.r9.u32));
}

// ---- Object manager ----

// NTSTATUS ObReferenceObjectByHandle(HANDLE handle, void* objectType, void** object)
PPC_FUNC(__imp__ObReferenceObjectByHandle) {
    Trace(ctx, "ObReferenceObjectByHandle", true);
    uint32_t object = LookupHandle(ctx.r3.u32);
    if (!object) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    ReferenceObject(object);
    if (ctx.r5.u32) StoreBE32(base, ctx.r5.u32, object);
    ctx.r3.u64 = kStatusSuccess;
}

// void ObReferenceObject(void* object)
PPC_FUNC(__imp__ObReferenceObject) {
    Trace(ctx, "ObReferenceObject", true);
    ReferenceObject(ctx.r3.u32);
}

// void ObDereferenceObject(void* object)
PPC_FUNC(__imp__ObDereferenceObject) {
    Trace(ctx, "ObDereferenceObject", true);
    DereferenceObject(ctx.r3.u32);
}

// NTSTATUS NtClose(HANDLE handle)
PPC_FUNC(__imp__NtClose) {
    Trace(ctx, "NtClose", true);
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? kStatusSuccess : kStatusInvalidHandle;
}

// NTSTATUS NtDuplicateObject(HANDLE source, HANDLE* target, u32 options)
PPC_FUNC(__imp__NtDuplicateObject) {
    Trace(ctx, "NtDuplicateObject", true);
    uint32_t source = ctx.r3.u32;
    uint32_t object = LookupHandle(source);
    if (!object) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, CreateHandle(object));
    if (ctx.r5.u32 & kDuplicateCloseSource) CloseHandle(source);
    ctx.r3.u64 = kStatusSuccess;
}
