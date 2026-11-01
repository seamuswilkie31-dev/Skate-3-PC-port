#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

// Settings shared by the launcher (writes them) and the runtime (reads them).
namespace s3n {

enum class WindowMode { Windowed, Borderless, Fullscreen };
enum class ButtonPrompts { Auto, Xbox, PlayStation, Nintendo };
enum class NetMode { Off, Host, Join };

inline constexpr std::array<const char*, 3> kWindowModeNames{"windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, 4> kButtonPromptNames{"auto", "xbox", "playstation", "nintendo"};
inline constexpr std::array<const char*, 3> kNetModeNames{"off", "host", "join"};

struct Config {
    struct Game {
        std::string source;  // ISO file or extracted folder, UTF-8
    } game;

    struct Display {
        WindowMode windowMode = WindowMode::Borderless;
        int width = 0;  // 0 = desktop resolution
        int height = 0;
        float renderScale = 1.0f;
        bool vsync = true;
        int fpsCap = 60;  // 30 = original, 0 = unlimited
        int msaa = 4;
        bool rayTracing = false;  // experimental; ignored until the renderer supports it
    } display;

    struct Input {
        ButtonPrompts prompts = ButtonPrompts::Auto;
        bool nintendoLayout = false;  // swap A/B and X/Y by position on Switch pads
        float leftDeadzone = 0.15f;
        float rightDeadzone = 0.15f;
        float triggerThreshold = 0.10f;
        bool vibration = true;
        float vibrationStrength = 1.0f;
    } input;

    struct Saves {
        std::string imported;  // imported Xbox 360 save package, UTF-8
        bool unlockAll = false;
    } saves;

    struct Net {
        NetMode mode = NetMode::Off;
        std::string playerName = "Skater";
        std::string bindAddress;  // IPv4 of the Radmin VPN adapter
        std::string hostAddress;
        int port = 47360;
    } net;

    struct Modifiers {
        float gravity = 1.0f;
        float popHeight = 1.0f;
        float topSpeed = 1.0f;
        float flipSpeed = 1.0f;
        float spinSpeed = 1.0f;
    } mods;
};

std::filesystem::path UserDir();     // %APPDATA%/Skate3Native
std::filesystem::path ConfigPath();  // UserDir()/config.ini
std::filesystem::path SavesDir();    // UserDir()/saves

bool LoadConfig(const std::filesystem::path& path, Config& config);
bool SaveConfig(const std::filesystem::path& path, const Config& config);

std::filesystem::path PathFromUtf8(std::string_view utf8);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace s3n
