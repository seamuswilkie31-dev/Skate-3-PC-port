// s3native: runs Skate 3's recompiled code natively.
//
//   s3native [--game <iso or folder>] [--xex <default.xex>] [--log <file>]
//            [--stall <seconds>] [--time-limit <seconds>] [--watch <hex address>]
//            [--watch-reads <hex address>] [--no-window] [--dump-frames <interval>]
//            [--capture-frame <frame>] [--press <button:seconds,...>]
//
// Loads the game, mounts its disc (--game, or the launcher's choice in config.ini),
// opens the game window, starts the entry point on a guest main thread, and logs every
// kernel call. Guest threads run on host threads; imports we haven't implemented are
// logged stubs returning 0. The run ends through kernel::Finish, which prints a report.
// --dump-frames n writes every nth frame to <work>\frames as a BMP; --capture-frame n logs every
// clear, resolve and draw of frame n; --press presses pad buttons on a schedule (input.h).

#include "apu/xma.h"
#include "config.h"
#include "disc.h"
#include "gpu/gpu.h"
#include "input/input.h"
#include "kernel/kernel.h"
#include "kernel/xam.h"
#include "loader.h"
#include "memory.h"
#include "render/presenter.h"
#include "render/shader_cache.h"
#include "render/targets.h"

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
    std::string gameSource, presses;
    bool window = true;
    kernel::Options options;
    for (int i = 1; i < argc; i += 2) {
        std::string key = argv[i];
        if (key == "--no-window") {
            window = false;
            --i;  // a flag without a value
            continue;
        }
        if (i + 1 >= argc) break;
        if (key == "--game") gameSource = argv[i + 1];
        else if (key == "--xex") xexPath = argv[i + 1];
        else if (key == "--log") logPath = argv[i + 1];
        else if (key == "--stall") options.stallSeconds = uint32_t(std::atoi(argv[i + 1]));
        else if (key == "--time-limit") options.timeLimitSeconds = uint32_t(std::atoi(argv[i + 1]));
        else if (key == "--press") presses = argv[i + 1];
        else if (key == "--capture-frame") render::CaptureFrame(std::strtoull(argv[i + 1], nullptr, 10));
        else if (key == "--dump-frames") render::DumpFrames(uint32_t(std::atoi(argv[i + 1])), DefaultWorkPath("frames"));
        else if (key == "--watch") options.watchAddress = uint32_t(std::strtoul(argv[i + 1], nullptr, 16));
        else if (key == "--watch-reads") {
            options.watchAddress = uint32_t(std::strtoul(argv[i + 1], nullptr, 16));
            options.watchReads = true;
        }
        // --config (from the launcher) is accepted and not used yet.
    }

    options.log = std::fopen(logPath.c_str(), "w");
    GuestMemory memory;
    std::string error;
    if (!memory.Reserve(error)) {
        std::fprintf(stderr, "s3native: can't set up guest memory: %s\n", error.c_str());
        return 1;
    }
    kernel::Init(memory, options);
    if (!gpu::Init(memory)) {
        kernel::Log("s3native: can't map the GPU registers\n");
        return 1;
    }
    // The XMA audio decoder's registers: plain memory, since the decoder is driven through
    // the XMA* functions instead (apu/xma.cpp).
    constexpr uint32_t kAudioRegisters = 0x7FEA0000;
    if (!memory.Commit(kAudioRegisters, 0x10000)) {
        kernel::Log("s3native: can't map the audio registers\n");
        return 1;
    }
    apu::InitXma(memory);

    LoadedGame game;
    if (!LoadGame(memory, xexPath, game, error)) {
        kernel::Log("s3native: %s\n", error.c_str());
        return 1;
    }
    kernel::Log("s3native: loaded %s\n", xexPath.c_str());
    kernel::Log("  title %08X version %u, entry 0x%08X\n", game.xex.titleId, game.xex.version, game.entryPoint);
    kernel::SetTitleId(game.xex.titleId);
    kernel::Log("  %zu recompiled functions in the indirect-call table\n", game.functionsMapped);
    kernel::Log("  %zu kernel/XAM function imports, %zu imported variables:", game.functionImports, game.variables.size());
    for (const auto& [name, address] : game.variables) {
        kernel::Log(" %s", name.c_str());
        kernel::DefineVariable(name, address);
    }
    kernel::Log("\n");

    // The launcher's settings; the disc is --game, or the ISO/folder chosen there.
    Config config;
    LoadConfig(ConfigPath(), config);
    if (gameSource.empty()) gameSource = config.game.source;
    if (gameSource.empty()) {
        kernel::Log("s3native: no game disc; choose your ISO in the launcher or pass --game <iso or folder>\n");
        return 1;
    }
    static Disc disc;
    if (!disc.Mount(PathFromUtf8(gameSource), error)) {
        kernel::Log("s3native: can't mount %s: %s\n", gameSource.c_str(), error.c_str());
        return 1;
    }
    kernel::MountDisc(&disc);
    kernel::Log("  disc: %s (%s, %zu files, %.2f GiB)\n", gameSource.c_str(), disc.Layout().c_str(), disc.FileCount(),
                disc.TotalBytes() / double(1ull << 30));

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

    // The translated shaders (scripts/shaders.ps1). Without them the game still runs, but
    // the renderer can't tell what it draws.
    if (size_t shaders = render::LoadShaderCache(DefaultWorkPath("shaders\\shader_cache.bin"), error))
        kernel::Log("  shaders: %zu translated shaders loaded\n", shaders);
    else
        kernel::Log("  shaders: %s\n", error.c_str());

    // The window belongs to this (the process's main) thread, so it's opened before the
    // title starts and its loop runs here.
    if (window && !render::Init(error)) {
        kernel::Log("s3native: can't open the game window: %s\n", error.c_str());
        return 1;
    }
    if (window) input::Init(config.input);
    if (!presses.empty() && !input::ScriptPresses(presses, error)) {
        kernel::Log("s3native: %s\n", error.c_str());
        return 1;
    }

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
    if (!window) {
        for (;;) Sleep(INFINITE);  // kernel::Finish ends the process
    }
    render::Run();
    kernel::Finish("the window was closed", 0);
}
