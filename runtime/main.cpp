// s3native: runs Skate 3's recompiled code natively.
//
//   s3native [--xex <default.xex>] [--log <file>] [--stall <seconds>] [--time-limit <seconds>]
//
// Loads the game, starts its entry point on a guest main thread, and logs every kernel
// call. Guest threads run on host threads; imports we haven't implemented are logged
// stubs returning 0. The run ends through kernel::Finish, which prints a report.

#include "kernel/kernel.h"
#include "loader.h"
#include "memory.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace s3n;

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
        else if (key == "--watch") options.watchAddress = uint32_t(std::strtoul(argv[i + 1], nullptr, 16));
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
    for (const auto& [name, address] : game.variables) {
        kernel::Log(" %s", name.c_str());
        kernel::DefineVariable(name, address);
    }
    kernel::Log("\n");

    // Static TLS (the XEX's data size, exactly) is followed by the dynamic slots.
    kernel::TlsLayout tls;
    tls.slotCount = game.xex.tlsSlotCount ? game.xex.tlsSlotCount : 1024;
    tls.staticSize = game.xex.tlsSlotCount ? game.xex.tlsDataSize : 0;
    tls.templateAddress = game.xex.tlsRawDataAddress;
    tls.templateSize = tls.staticSize ? game.xex.tlsRawDataSize : 0;
    kernel::SetTlsLayout(tls);
    kernel::SetDefaultStackSize(game.xex.defaultStackSize);
    kernel::Log("  TLS: %u static bytes, %u slots; default stack %u KB\n", tls.staticSize, tls.slotCount,
                game.xex.defaultStackSize / 1024);

    kernel::ThreadStart start;
    start.entry = game.entryPoint;
    start.stackSize = game.xex.defaultStackSize > 0x100000 ? game.xex.defaultStackSize : 0x100000;
    start.main = true;
    uint32_t mainThread = kernel::CreateGuestThread(start, nullptr);
    if (!mainThread) {
        kernel::Log("s3native: can't create the main thread\n");
        return 1;
    }
    kernel::DereferenceObject(mainThread);
    for (;;) Sleep(INFINITE);  // kernel::Finish ends the process
}
