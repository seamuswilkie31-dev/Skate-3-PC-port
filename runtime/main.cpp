// s3native: runs Skate 3's recompiled code natively.
//
//   s3native [--xex <default.xex>] [--log <file>] [--stall <seconds>] [--time-limit <seconds>]
//
// Phase 2 runtime: loads the game, starts its entry point on a guest main thread, and
// logs every kernel call. Imports we haven't implemented are logged stubs returning 0.

#include "kernel/kernel.h"
#include "loader.h"
#include "memory.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace s3n;

// The few processor-block (r13) and thread-object fields Xbox code reads directly.
constexpr uint32_t kPcrTls = 0x000;
constexpr uint32_t kPcrSelf = 0x030;
constexpr uint32_t kPcrStackBase = 0x070;  // high end
constexpr uint32_t kPcrStackLimit = 0x074;  // low end
constexpr uint32_t kPcrCurrentThread = 0x100;
constexpr uint32_t kThreadStackBase = 0x05C;
constexpr uint32_t kThreadStackLimit = 0x060;
constexpr uint32_t kThreadTls = 0x068;

struct GuestThread {
    uint32_t pcr = 0;
    uint32_t thread = 0;
    uint32_t tls = 0;  // static TLS data; the dynamic slots (KeTlsAlloc) follow it
    uint32_t stackBase = 0;  // high end
    uint32_t stackLimit = 0;
};

bool CreateGuestThread(GuestMemory& memory, const kernel::TlsLayout& layout, const XexInfo& xex, uint32_t stackSize,
                       GuestThread& t) {
    uint8_t* base = memory.base();
    t.tls = memory.AllocSystem(layout.staticSize + layout.slotCount * 4);
    t.stackLimit = memory.AllocSystem(stackSize, 0x10000);
    t.thread = memory.AllocSystem(0x400);
    t.pcr = memory.AllocSystem(0x1000, 0x1000);
    if (!t.tls || !t.stackLimit || !t.thread || !t.pcr) return false;
    t.stackBase = t.stackLimit + stackSize;
    if (layout.staticSize && xex.tlsRawDataSize)
        std::memcpy(base + t.tls, base + xex.tlsRawDataAddress, xex.tlsRawDataSize);

    StoreBE32(base, t.pcr + kPcrTls, t.tls);
    StoreBE32(base, t.pcr + kPcrSelf, t.pcr);
    StoreBE32(base, t.pcr + kPcrStackBase, t.stackBase);
    StoreBE32(base, t.pcr + kPcrStackLimit, t.stackLimit);
    StoreBE32(base, t.pcr + kPcrCurrentThread, t.thread);
    StoreBE32(base, t.thread + kThreadStackBase, t.stackBase);
    StoreBE32(base, t.thread + kThreadStackLimit, t.stackLimit);
    StoreBE32(base, t.thread + kThreadTls, t.tls);
    return true;
}

struct MainThreadArgs {
    GuestMemory* memory;
    GuestThread thread;
    uint32_t entry;
};

DWORD WINAPI RunMainThread(void* param) {
    auto* args = static_cast<MainThreadArgs*>(param);
    uint8_t* base = args->memory->base();
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = args->thread.stackBase - 0x100;  // leave room for the caller's linkage area
    ctx.r13.u64 = args->thread.pcr;
    ctx.r3.u64 = 0;
    kernel::SetMainContext(&ctx);
    kernel::Log("starting the entry point 0x%08X\n\n", args->entry);
    (PPC_LOOKUP_FUNC(base, args->entry))(ctx, base);
    kernel::Finish("the entry point returned", 0);
}

std::string DefaultWorkPath(const char* leaf) {
    const char* local = std::getenv("LOCALAPPDATA");
    return std::string(local ? local : ".") + "\\Skate3Native\\" + leaf;
}

}  // namespace

int main(int argc, char** argv) {
    std::string xexPath = DefaultWorkPath("game\\default.xex");
    std::string logPath = DefaultWorkPath("s3native.log");
    kernel::Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string key = argv[i];
        if (key == "--xex") xexPath = argv[i + 1];
        else if (key == "--log") logPath = argv[i + 1];
        else if (key == "--stall") options.stallSeconds = uint32_t(std::atoi(argv[i + 1]));
        else if (key == "--time-limit") options.timeLimitSeconds = uint32_t(std::atoi(argv[i + 1]));
        // --config (from the launcher) is accepted and not used yet.
    }

    options.log = std::fopen(logPath.c_str(), "w");
    GuestMemory memory;
    if (!memory.Reserve()) {
        std::fprintf(stderr, "s3native: can't reserve the 4 GB guest address space\n");
        return 1;
    }
    kernel::Init(memory, options);

    LoadedGame game;
    std::string error;
    if (!LoadGame(memory, xexPath, game, error)) {
        kernel::Log("s3native: %s\n", error.c_str());
        return 1;
    }
    kernel::Log("s3native: loaded %s\n", xexPath.c_str());
    kernel::Log("  title %08X version %u, entry 0x%08X\n", game.xex.titleId, game.xex.version, game.entryPoint);
    kernel::Log("  %zu recompiled functions in the indirect-call table\n", game.functionsMapped);
    kernel::Log("  %zu kernel/XAM function imports, %zu imported variables:", game.functionImports, game.variables.size());
    for (const std::string& v : game.variables) kernel::Log(" %s", v.c_str());
    kernel::Log("\n");

    // Static TLS (the XEX's data size, exactly) is followed by the dynamic slots.
    kernel::TlsLayout tls;
    tls.slotCount = game.xex.tlsSlotCount ? game.xex.tlsSlotCount : 1024;
    tls.staticSize = game.xex.tlsSlotCount ? game.xex.tlsDataSize : 0;
    kernel::SetTlsLayout(tls);

    MainThreadArgs args{&memory, {}, game.entryPoint};
    uint32_t stackSize = game.xex.defaultStackSize > 0x100000 ? game.xex.defaultStackSize : 0x100000;
    if (!CreateGuestThread(memory, tls, game.xex, stackSize, args.thread)) {
        kernel::Log("s3native: out of system memory for the main thread\n");
        return 1;
    }
    kernel::Log("  main thread: pcr=%08X tls=%08X (%u static bytes, %u slots) stack=%08X-%08X\n", args.thread.pcr,
                args.thread.tls, tls.staticSize, tls.slotCount, args.thread.stackLimit, args.thread.stackBase);

    // Recompiled code recurses on the host stack; give it plenty.
    HANDLE thread = CreateThread(nullptr, 256u << 20, RunMainThread, &args, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        kernel::Log("s3native: can't start the main thread\n");
        return 1;
    }
    WaitForSingleObject(thread, INFINITE);
    return 0;
}
