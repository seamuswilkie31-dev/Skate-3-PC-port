#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <type_traits>

namespace s3n {
namespace {

template <class E> struct EnumNames;
template <> struct EnumNames<WindowMode> { static constexpr const auto& names = kWindowModeNames; };
template <> struct EnumNames<ButtonPrompts> { static constexpr const auto& names = kButtonPromptNames; };
template <> struct EnumNames<NetMode> { static constexpr const auto& names = kNetModeNames; };

// One list of every setting, used for both loading and saving.
template <class C, class F> void VisitConfig(C& c, F&& f) {
    f("game", "source", c.game.source);

    f("display", "window_mode", c.display.windowMode);
    f("display", "width", c.display.width);
    f("display", "height", c.display.height);
    f("display", "render_scale", c.display.renderScale);
    f("display", "vsync", c.display.vsync);
    f("display", "fps_cap", c.display.fpsCap);
    f("display", "msaa", c.display.msaa);
    f("display", "ray_tracing", c.display.rayTracing);

    f("input", "button_prompts", c.input.prompts);
    f("input", "nintendo_layout", c.input.nintendoLayout);
    f("input", "left_deadzone", c.input.leftDeadzone);
    f("input", "right_deadzone", c.input.rightDeadzone);
    f("input", "trigger_threshold", c.input.triggerThreshold);
    f("input", "vibration", c.input.vibration);
    f("input", "vibration_strength", c.input.vibrationStrength);

    f("saves", "imported", c.saves.imported);
    f("saves", "unlock_all", c.saves.unlockAll);

    f("net", "mode", c.net.mode);
    f("net", "player_name", c.net.playerName);
    f("net", "bind_address", c.net.bindAddress);
    f("net", "host_address", c.net.hostAddress);
    f("net", "port", c.net.port);

    f("modifiers", "gravity", c.mods.gravity);
    f("modifiers", "pop_height", c.mods.popHeight);
    f("modifiers", "top_speed", c.mods.topSpeed);
    f("modifiers", "flip_speed", c.mods.flipSpeed);
    f("modifiers", "spin_speed", c.mods.spinSpeed);
}

std::string Format(bool v) { return v ? "true" : "false"; }
std::string Format(int v) { return std::to_string(v); }
std::string Format(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
std::string Format(const std::string& v) { return v; }
template <class E> requires std::is_enum_v<E> std::string Format(E v) {
    return EnumNames<E>::names[static_cast<size_t>(v)];
}

void Parse(const std::string& s, bool& v) { v = (s == "true" || s == "1" || s == "yes"); }
void Parse(const std::string& s, int& v) { v = std::atoi(s.c_str()); }
void Parse(const std::string& s, float& v) { v = std::strtof(s.c_str(), nullptr); }
void Parse(const std::string& s, std::string& v) { v = s; }
template <class E> requires std::is_enum_v<E> void Parse(const std::string& s, E& v) {
    const auto& names = EnumNames<E>::names;
    for (size_t i = 0; i < names.size(); ++i)
        if (s == names[i]) v = static_cast<E>(i);
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

}  // namespace

std::filesystem::path PathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path UserDir() {
#ifdef _WIN32
    wchar_t* appData = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&appData, &length, L"APPDATA") == 0 && appData) {
        std::filesystem::path dir = std::filesystem::path(appData) / "Skate3Native";
        std::free(appData);
        return dir;
    }
#else
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local/share/Skate3Native";
#endif
    return std::filesystem::current_path() / "user";
}

std::filesystem::path ConfigPath() { return UserDir() / "config.ini"; }
std::filesystem::path SavesDir() { return UserDir() / "saves"; }

bool LoadConfig(const std::filesystem::path& path, Config& config) {
    std::ifstream in(path);
    if (!in) return false;
    std::map<std::string, std::string> values;  // "section.key" -> value
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos)
            values[section + "." + Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    VisitConfig(config, [&](const char* sec, const char* key, auto& value) {
        auto it = values.find(std::string(sec) + "." + key);
        if (it != values.end()) Parse(it->second, value);
    });
    return true;
}

bool SaveConfig(const std::filesystem::path& path, const Config& config) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "; Skate 3 Native settings. Written by the launcher, read by the runtime.\n";
    std::string section;
    VisitConfig(config, [&](const char* sec, const char* key, const auto& value) {
        if (section != sec) {
            out << (section.empty() ? "" : "\n") << '[' << sec << "]\n";
            section = sec;
        }
        out << key << " = " << Format(value) << '\n';
    });
    return static_cast<bool>(out);
}

}  // namespace s3n
