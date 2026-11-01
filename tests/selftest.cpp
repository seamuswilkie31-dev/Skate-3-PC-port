// Checks for the shared code: config round-trip, and game inspection on a real image.
//   s3n_selftest [iso-or-folder]

#include "config.h"
#include "game_source.h"

#include <cstdio>
#include <filesystem>

using namespace s3n;

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        failures += !ok;
    };

    Config a;
    a.game.source = "D:\\Games\\Skate 3 (USA).iso";
    a.display.windowMode = WindowMode::Fullscreen;
    a.display.fpsCap = 120;
    a.display.renderScale = 1.5f;
    a.input.prompts = ButtonPrompts::PlayStation;
    a.saves.unlockAll = true;
    a.net.mode = NetMode::Join;
    a.net.hostAddress = "26.1.2.3";
    a.mods.popHeight = 2.25f;
    auto path = std::filesystem::temp_directory_path() / "s3n_selftest.ini";
    Config b;
    check(SaveConfig(path, a) && LoadConfig(path, b), "config writes and reads back");
    check(b.game.source == a.game.source && b.display.windowMode == WindowMode::Fullscreen &&
              b.display.fpsCap == 120 && b.display.renderScale == 1.5f && b.input.prompts == ButtonPrompts::PlayStation &&
              b.saves.unlockAll && b.net.mode == NetMode::Join && b.net.hostAddress == "26.1.2.3" &&
              b.mods.popHeight == 2.25f,
          "config values survive the round-trip");
    std::filesystem::remove(path);

    if (argc > 1) {
        GameCheck g = InspectGame(PathFromUtf8(argv[1]));
        check(g.readable, "game image is readable");
        if (!g.error.empty()) std::printf("      note: %s\n", g.error.c_str());
        if (g.readable) {
            const XexInfo& x = g.xex;
            std::printf("      layout %s, %zu files, %.2f GiB\n", g.layout.c_str(), g.fileCount,
                        double(g.totalBytes) / (1 << 30));
            std::printf("      title %08X  media %08X  version %s  disc %u/%u  %s, %s\n", x.titleId, x.mediaId,
                        FormatXexVersion(x.version).c_str(), x.discNumber, x.discCount,
                        x.encryption ? "encrypted" : "plain", XexCompressionName(x.compression));
            std::printf("      %s  load 0x%08X  size 0x%08X  entry 0x%08X\n", x.originalPeName.c_str(),
                        x.loadAddress, x.imageSize, x.entryPoint);
            std::printf("      Skate 3: %s\n", g.isSkate3 ? "yes" : "no");
        }
    }
    return failures ? 1 : 0;
}
