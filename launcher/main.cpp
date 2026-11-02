// Skate 3 Native launcher: pick your game copy, set options, start the runtime.

#include "config.h"
#include "game_source.h"
#include "net_adapters.h"
#include "stfs.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace s3n;

namespace {

// ---- File dialogs ------------------------------------------------------------
// SDL may run dialog callbacks on another thread, so results are queued here and
// picked up by the UI loop.

enum class DialogKind : intptr_t { GameIso, GameFolder, SaveFile };

struct DialogResult {
    DialogKind kind;
    std::string path;
};

std::mutex gDialogMutex;
std::vector<DialogResult> gDialogResults;

void SDLCALL OnDialogDone(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;  // error or cancelled
    std::lock_guard lock(gDialogMutex);
    gDialogResults.push_back({static_cast<DialogKind>(reinterpret_cast<intptr_t>(userdata)), files[0]});
}

void* DialogTag(DialogKind kind) { return reinterpret_cast<void*>(static_cast<intptr_t>(kind)); }

const SDL_DialogFileFilter kIsoFilters[] = {{"Xbox 360 disc image", "iso"}, {"All files", "*"}};
const SDL_DialogFileFilter kSaveFilters[] = {{"Xbox 360 save package", "*"}};

// ---- Look ----------------------------------------------------------------------

const ImVec4 kAccent{0.95f, 0.62f, 0.18f, 1.0f};
const ImVec4 kGood{0.45f, 0.82f, 0.45f, 1.0f};
const ImVec4 kBad{0.96f, 0.42f, 0.36f, 1.0f};
const ImVec4 kMuted{0.60f, 0.62f, 0.67f, 1.0f};

void ApplyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = {18, 16};
    s.FramePadding = {10, 6};
    s.ItemSpacing = {10, 8};
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 6;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.SelectableTextAlign = {0.0f, 0.5f};

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = {0.07f, 0.075f, 0.09f, 1};
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0.10f, 0.105f, 0.125f, 1};
    c[ImGuiCol_Border] = {0.20f, 0.21f, 0.25f, 1};
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_FrameBg] = {0.13f, 0.14f, 0.17f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.17f, 0.18f, 0.22f, 1};
    c[ImGuiCol_FrameBgActive] = {0.20f, 0.21f, 0.26f, 1};
    c[ImGuiCol_Button] = {0.16f, 0.17f, 0.21f, 1};
    c[ImGuiCol_ButtonHovered] = {0.22f, 0.23f, 0.28f, 1};
    c[ImGuiCol_ButtonActive] = {0.26f, 0.27f, 0.33f, 1};
    c[ImGuiCol_Header] = {kAccent.x, kAccent.y, kAccent.z, 0.22f};
    c[ImGuiCol_HeaderHovered] = {kAccent.x, kAccent.y, kAccent.z, 0.30f};
    c[ImGuiCol_HeaderActive] = {kAccent.x, kAccent.y, kAccent.z, 0.40f};
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = {1.0f, 0.72f, 0.30f, 1};
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_TextSelectedBg] = {kAccent.x, kAccent.y, kAccent.z, 0.35f};
    s.ScaleAllSizes(scale);
}

// ---- Small widgets -------------------------------------------------------------

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Label in a fixed left column; the next widget goes to its right.
void Label(const char* text) {
    float em = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(em * 11);
    ImGui::SetNextItemWidth(em * 16);
}

template <class E> void EnumCombo(const char* id, E& value, std::initializer_list<const char*> labels) {
    int i = static_cast<int>(value);
    if (ImGui::Combo(id, &i, labels.begin(), static_cast<int>(labels.size()))) value = static_cast<E>(i);
}

void OptionCombo(const char* id, int& value, std::initializer_list<std::pair<int, const char*>> options) {
    const char* preview = "Custom";
    for (const auto& [v, name] : options)
        if (v == value) preview = name;
    if (ImGui::BeginCombo(id, preview)) {
        for (const auto& [v, name] : options)
            if (ImGui::Selectable(name, v == value)) value = v;
        ImGui::EndCombo();
    }
}

std::string Hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

const char* PadTypeName(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One / Series";
    case SDL_GAMEPAD_TYPE_PS3: return "PlayStation 3";
    case SDL_GAMEPAD_TYPE_PS4: return "PlayStation 4";
    case SDL_GAMEPAD_TYPE_PS5: return "PlayStation 5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    default: return "Generic controller";
    }
}

const char* ButtonName(SDL_Gamepad* pad, SDL_GamepadButton button) {
    switch (SDL_GetGamepadButtonLabel(pad, button)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
    default: break;
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L-stick click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R-stick click";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L bumper";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R bumper";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Share / Capture";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    default: return "Extra button";
    }
}

std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// ---- Launcher ----------------------------------------------------------------

enum Page { kGame, kDisplay, kControls, kSaves, kMultiplayer, kModifiers, kPageCount };
const char* const kPageNames[kPageCount] = {"Game", "Display", "Controls", "Saves", "Multiplayer", "Modifiers"};

class Launcher {
public:
    explicit Launcher(SDL_Window* window, ImFont* headingFont) : window_(window), headingFont_(headingFont) {
        LoadConfig(ConfigPath(), config_);
        RefreshResolutions();
        RefreshPads();
        adapters_ = ListNetAdapters();
        if (!config_.game.source.empty()) Verify(false);
        if (!config_.saves.imported.empty()) {
            StfsInfo info;
            if (ReadStfsInfo(PathFromUtf8(config_.saves.imported), info)) save_ = info;
        }
    }

    ~Launcher() {
        if (testPad_) SDL_CloseGamepad(testPad_);
        SaveConfig(ConfigPath(), config_);
    }

    void RefreshPads() {
        pads_.clear();
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            pads_.assign(ids, ids + count);
            SDL_free(ids);
        }
        if (testPad_ && std::find(pads_.begin(), pads_.end(), SDL_GetGamepadID(testPad_)) == pads_.end()) {
            SDL_CloseGamepad(testPad_);
            testPad_ = nullptr;
        }
        if (!testPad_ && !pads_.empty()) testPad_ = SDL_OpenGamepad(pads_.front());
    }

    void PollDialogs() {
        std::vector<DialogResult> results;
        {
            std::lock_guard lock(gDialogMutex);
            results.swap(gDialogResults);
        }
        for (const DialogResult& r : results) {
            if (r.kind == DialogKind::SaveFile) {
                ImportSave(r.path);
            } else {
                config_.game.source = r.path;
                Verify(true);
            }
        }
    }

    void Frame() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        float em = ImGui::GetFontSize();

        ImGui::BeginChild("nav", ImVec2(em * 10, 0));
        ImGui::PushFont(headingFont_);
        ImGui::TextColored(kAccent, "SKATE 3");
        ImGui::PopFont();
        ImGui::TextColored(kMuted, "Native port");
        ImGui::Dummy(ImVec2(0, em));
        for (int i = 0; i < kPageCount; ++i)
            if (ImGui::Selectable(kPageNames[i], page_ == i, 0, ImVec2(0, em * 1.9f))) page_ = i;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - em * 1.6f);
        ImGui::TextColored(kMuted, "v0.1  |  phase 1");
        ImGui::EndChild();

        ImGui::SameLine(0, em * 1.5f);
        ImGui::BeginGroup();
        ImGui::BeginChild("page", ImVec2(0, -em * 3.2f));
        switch (page_) {
        case kGame: GamePage(); break;
        case kDisplay: DisplayPage(); break;
        case kControls: ControlsPage(); break;
        case kSaves: SavesPage(); break;
        case kMultiplayer: MultiplayerPage(); break;
        case kModifiers: ModifiersPage(); break;
        }
        ImGui::EndChild();
        ImGui::Separator();
        Footer();
        ImGui::EndGroup();
        ImGui::End();
    }

private:
    void Heading(const char* title, const char* subtitle) {
        ImGui::PushFont(headingFont_);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        Hint(subtitle);
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
    }

    void SetStatus(std::string text, bool error = false) {
        status_ = std::move(text);
        statusIsError_ = error;
    }

    void Save() {
        if (SaveConfig(ConfigPath(), config_))
            SetStatus("Settings saved.");
        else
            SetStatus("Couldn't write " + PathToUtf8(ConfigPath()), true);
    }

    void Verify(bool saveIfGood) {
        game_ = InspectGame(PathFromUtf8(config_.game.source));
        gameChecked_ = true;
        if (game_.isSkate3 && saveIfGood) Save();
    }

    // ---- Game ----

    void GamePage() {
        float em = ImGui::GetFontSize();
        Heading("Game",
                "Point the launcher at your own copy of Skate 3 for Xbox 360: a disc image (.iso) or an extracted "
                "game folder. The port reads your files where they are; nothing is copied or uploaded.");

        ImGui::SetNextItemWidth(-em * 15.5f);
        if (ImGui::InputTextWithHint("##source", "C:\\Games\\Skate 3.iso", &config_.game.source,
                                     ImGuiInputTextFlags_EnterReturnsTrue))
            Verify(true);
        ImGui::SameLine();
        if (ImGui::Button("Browse ISO..."))
            SDL_ShowOpenFileDialog(OnDialogDone, DialogTag(DialogKind::GameIso), window_, kIsoFilters, 2, nullptr,
                                   false);
        ImGui::SameLine();
        if (ImGui::Button("Folder...")) SDL_ShowOpenFolderDialog(OnDialogDone, DialogTag(DialogKind::GameFolder), window_, nullptr, false);
        ImGui::SameLine();
        if (ImGui::Button("Verify")) Verify(true);

        ImGui::Dummy(ImVec2(0, em * 0.5f));
        ImGui::BeginChild("result", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        if (!gameChecked_) {
            Hint("Not verified yet.");
        } else if (!game_.readable) {
            ImGui::TextColored(kBad, "%s", game_.error.c_str());
        } else {
            if (game_.isSkate3)
                ImGui::TextColored(kGood, "Skate 3 found. This copy is ready for the port.");
            else
                ImGui::TextColored(kBad, "%s", game_.error.c_str());
            ImGui::Spacing();
            const XexInfo& x = game_.xex;
            if (ImGui::BeginTable("gameinfo", 2, ImGuiTableFlags_SizingFixedFit)) {
                auto row = [](const char* key, const std::string& value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kMuted, "%s", key);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(value.c_str());
                };
                char buf[64];
                row("Source", game_.layout);
                row("Title ID", Hex32(x.titleId));
                row("Media ID", Hex32(x.mediaId) +
                                    (x.mediaId == kSkate3RetailMediaId ? "   retail disc (USA / Europe)" : ""));
                row("Version", FormatXexVersion(x.version));
                std::snprintf(buf, sizeof buf, "%u of %u", x.discNumber, x.discCount);
                row("Disc", buf);
                std::snprintf(buf, sizeof buf, "%zu files, %.2f GB", game_.fileCount,
                              double(game_.totalBytes) / (1024.0 * 1024.0 * 1024.0));
                row("Contents", buf);
                row("Executable", (x.originalPeName.empty() ? std::string("default.xex") : x.originalPeName) +
                                      (x.encryption ? ", encrypted" : ", plain") + ", " +
                                      XexCompressionName(x.compression) + " compressed");
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    // ---- Display ----

    void RefreshResolutions() {
        resolutions_.clear();
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        int count = 0;
        if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
            for (int i = 0; i < count; ++i) {
                std::pair<int, int> r{modes[i]->w, modes[i]->h};
                if (std::find(resolutions_.begin(), resolutions_.end(), r) == resolutions_.end())
                    resolutions_.push_back(r);
            }
            SDL_free(modes);
        }
        if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display))
            desktop_ = {desktop->w, desktop->h};
    }

    void DisplayPage() {
        Config::Display& d = config_.display;
        Heading("Display", "Rendering is native Vulkan: no emulator in between.");

        Label("Renderer");
        ImGui::TextUnformatted("Vulkan");

        Label("Window mode");
        EnumCombo("##windowmode", d.windowMode, {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"});

        Label("Resolution");
        char current[48];
        if (d.width)
            std::snprintf(current, sizeof current, "%d x %d", d.width, d.height);
        else
            std::snprintf(current, sizeof current, "Desktop (%d x %d)", desktop_.first, desktop_.second);
        if (ImGui::BeginCombo("##resolution", current)) {
            char label[48];
            std::snprintf(label, sizeof label, "Desktop (%d x %d)", desktop_.first, desktop_.second);
            if (ImGui::Selectable(label, d.width == 0)) d.width = d.height = 0;
            for (const auto& [w, h] : resolutions_) {
                std::snprintf(label, sizeof label, "%d x %d", w, h);
                if (ImGui::Selectable(label, d.width == w && d.height == h)) {
                    d.width = w;
                    d.height = h;
                }
            }
            ImGui::EndCombo();
        }

        Label("Render scale");
        ImGui::SliderFloat("##renderscale", &d.renderScale, 0.5f, 2.0f, "%.2fx");

        Label("Anti-aliasing");
        OptionCombo("##msaa", d.msaa, {{0, "Off"}, {2, "2x MSAA"}, {4, "4x MSAA"}, {8, "8x MSAA"}});

        Label("VSync");
        ImGui::Checkbox("##vsync", &d.vsync);

        Label("Frame rate cap");
        OptionCombo("##fps", d.fpsCap,
                    {{30, "30 fps (original)"}, {60, "60 fps"}, {120, "120 fps"}, {240, "240 fps"}, {0, "Unlimited"}});
        ImGui::Indent(ImGui::GetFontSize() * 11);
        Hint("The Xbox 360 game runs at 30 fps. It times itself by the clock, so a higher cap gives more frames, "
             "not a faster game; how many you get depends on your PC. With VSync on, your display's refresh rate "
             "caps it too.");
        ImGui::Unindent(ImGui::GetFontSize() * 11);

        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
        Label("Ray tracing");
        ImGui::BeginDisabled();
        ImGui::Checkbox("Experimental, not available yet", &d.rayTracing);
        ImGui::EndDisabled();
        ImGui::Indent(ImGui::GetFontSize() * 11);
        Hint("Skate 3 shipped without any ray tracing data. RT shadows and ambient occlusion may come later as an "
             "add-on to the Vulkan renderer; this switch unlocks when they exist.");
        ImGui::Unindent(ImGui::GetFontSize() * 11);
    }

    // ---- Controls ----

    void DrawPadTester() {
        float em = ImGui::GetFontSize();
        float r = em * 2.6f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        auto axis = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(testPad_, a) / 32767.0f, -1.0f, 1.0f); };
        auto stick = [&](SDL_GamepadAxis ax, SDL_GamepadAxis ay, float deadzone) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 c{p.x + r, p.y + r};
            draw->AddCircleFilled(c, r, ImGui::GetColorU32(ImGuiCol_FrameBg));
            draw->AddCircle(c, r * deadzone, ImGui::GetColorU32(ImGuiCol_Border), 0, 1.5f);
            draw->AddCircleFilled(ImVec2(c.x + axis(ax) * r, c.y + axis(ay) * r), r * 0.17f, ImGui::GetColorU32(kAccent));
            ImGui::Dummy(ImVec2(r * 2, r * 2));
        };
        stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, config_.input.leftDeadzone);
        ImGui::SameLine(0, em);
        stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, config_.input.rightDeadzone);
        ImGui::SameLine(0, em * 1.5f);

        ImGui::BeginGroup();
        ImGui::ProgressBar(std::max(0.0f, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), ImVec2(em * 10, 0), "Left trigger");
        ImGui::ProgressBar(std::max(0.0f, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)), ImVec2(em * 10, 0), "Right trigger");
        std::string pressed;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            auto button = static_cast<SDL_GamepadButton>(b);
            if (SDL_GetGamepadButton(testPad_, button)) pressed += std::string(pressed.empty() ? "" : ", ") + ButtonName(testPad_, button);
        }
        ImGui::TextColored(pressed.empty() ? kMuted : kAccent, "%s", pressed.empty() ? "Press any button" : pressed.c_str());
        if (ImGui::Button("Test vibration")) {
            auto strength = static_cast<Uint16>(0xFFFF * std::clamp(config_.input.vibrationStrength, 0.0f, 1.0f));
            SDL_RumbleGamepad(testPad_, strength, strength, 400);
        }
        ImGui::EndGroup();
    }

    void ControlsPage() {
        Config::Input& in = config_.input;
        Heading("Controls",
                "Xbox One / Series, PlayStation 4 / 5 and Nintendo Switch Pro / Joy-Con controllers all work, over "
                "USB or Bluetooth.");

        ImGui::SeparatorText("Connected controllers");
        if (pads_.empty()) Hint("No controllers connected. Plug one in or pair it; it shows up here right away.");
        for (SDL_JoystickID id : pads_) {
            const char* name = SDL_GetGamepadNameForID(id);
            std::string label = std::string(name ? name : "Controller") + "   (" + PadTypeName(SDL_GetGamepadTypeForID(id)) + ")";
            ImGui::PushID(static_cast<int>(id));
            bool selected = testPad_ && SDL_GetGamepadID(testPad_) == id;
            if (ImGui::Selectable(label.c_str(), selected) && !selected) {
                if (testPad_) SDL_CloseGamepad(testPad_);
                testPad_ = SDL_OpenGamepad(id);
            }
            ImGui::PopID();
        }
        if (testPad_) {
            ImGui::Spacing();
            DrawPadTester();
        }

        ImGui::SeparatorText("Settings");
        Label("Button prompts");
        EnumCombo("##prompts", in.prompts, {"Match my controller", "Xbox", "PlayStation", "Nintendo"});
        Label("Nintendo layout");
        ImGui::Checkbox("Swap A/B and X/Y to match button positions", &in.nintendoLayout);
        Label("Left stick deadzone");
        ImGui::SliderFloat("##ldz", &in.leftDeadzone, 0.0f, 0.5f, "%.2f");
        Label("Right stick deadzone");
        ImGui::SliderFloat("##rdz", &in.rightDeadzone, 0.0f, 0.5f, "%.2f");
        Label("Trigger threshold");
        ImGui::SliderFloat("##trig", &in.triggerThreshold, 0.0f, 0.5f, "%.2f");
        Label("Vibration");
        ImGui::Checkbox("##vibration", &in.vibration);
        Label("Vibration strength");
        ImGui::BeginDisabled(!in.vibration);
        ImGui::SliderFloat("##vibstrength", &in.vibrationStrength, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::Indent(ImGui::GetFontSize() * 11);
        Hint("On-screen button icons follow the prompt setting once the port can swap the game's prompt textures "
             "(roadmap phase 8).");
        ImGui::Unindent(ImGui::GetFontSize() * 11);
    }

    // ---- Saves ----

    void ImportSave(const std::string& utf8) {
        fs::path src = PathFromUtf8(utf8);
        StfsInfo info;
        std::string error;
        saveOk_ = false;
        if (!ReadStfsInfo(src, info, &error)) {
            saveMessage_ = error;
            return;
        }
        if (info.titleId != kSkate3TitleId) {
            saveMessage_ = "That save belongs to " + (info.titleName.empty() ? std::string("another game") : info.titleName) +
                           ", not Skate 3.";
            return;
        }
        if (info.contentType != kStfsSavedGame) {
            saveMessage_ = "That Skate 3 package isn't a saved game.";
            return;
        }
        std::error_code ec;
        fs::path dst = SavesDir() / "imported" / src.filename();
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            saveMessage_ = "Couldn't copy the save: " + ec.message();
            return;
        }
        config_.saves.imported = PathToUtf8(dst);
        save_ = info;
        saveOk_ = true;
        saveMessage_ = "Imported. The original file was left untouched.";
        Save();
    }

    void BackupSaves() {
        std::error_code ec;
        fs::create_directories(SavesDir(), ec);
        fs::path dst = UserDir() / "backups" / Timestamp();
        fs::create_directories(dst, ec);
        fs::copy(SavesDir(), dst, fs::copy_options::recursive, ec);
        if (ec)
            SetStatus("Backup failed: " + ec.message(), true);
        else
            SetStatus("Saves backed up to " + PathToUtf8(dst));
    }

    void SavesPage() {
        Config::Saves& s = config_.saves;
        Heading("Saves", "Your progress lives in your user folder, separate from the game files.");

        Label("Save folder");
        ImGui::TextUnformatted(PathToUtf8(SavesDir()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open")) {
            std::error_code ec;
            fs::create_directories(SavesDir(), ec);
            std::string url = "file:///" + PathToUtf8(SavesDir());
            std::replace(url.begin(), url.end(), '\\', '/');
            SDL_OpenURL(url.c_str());
        }

        ImGui::SeparatorText("Your skater from Xbox 360");
        Hint("Copy your Skate 3 save from the console to a USB drive (System Settings > Storage), pull the save "
             "package off the drive with an Xbox 360 USB file tool, then import it here.");
        if (ImGui::Button("Import Xbox 360 save..."))
            SDL_ShowOpenFileDialog(OnDialogDone, DialogTag(DialogKind::SaveFile), window_, kSaveFilters, 1, nullptr, false);
        if (save_ && !s.imported.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kGood, "Using: %s", save_->displayName.empty() ? "Skate 3 save" : save_->displayName.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Forget")) {
                s.imported.clear();
                save_.reset();
                saveMessage_.clear();
            }
        }
        if (!saveMessage_.empty()) ImGui::TextColored(saveOk_ ? kMuted : kBad, "%s", saveMessage_.c_str());

        ImGui::SeparatorText("Progress");
        ImGui::Checkbox("Start with 100% completion", &s.unlockAll);
        Hint("The port unlocks everything itself once the save format is mapped (roadmap phase 5). No one else's "
             "save file is downloaded or shipped.");

        ImGui::SeparatorText("Backups");
        if (ImGui::Button("Back up saves now")) BackupSaves();
    }

    // ---- Multiplayer ----

    void MultiplayerPage() {
        Config::Net& n = config_.net;
        float em = ImGui::GetFontSize();
        Heading("Multiplayer", "Skate with friends over Radmin VPN, which puts everyone on one virtual LAN.");

        auto radmin = std::find_if(adapters_.begin(), adapters_.end(), [](const NetAdapter& a) { return a.radmin; });
        if (radmin != adapters_.end())
            ImGui::TextColored(kGood, "Radmin VPN connected: %s", radmin->ipv4.c_str());
        else
            ImGui::TextColored(kBad, "Radmin VPN not detected.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) adapters_ = ListNetAdapters();
        if (radmin == adapters_.end())
            Hint("Install Radmin VPN, create or join a network with your friends, then press Refresh.");
        if (n.bindAddress.empty() && radmin != adapters_.end()) n.bindAddress = radmin->ipv4;

        ImGui::SeparatorText("Session");
        Label("Mode");
        int mode = static_cast<int>(n.mode);
        ImGui::RadioButton("Off", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Host", &mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Join", &mode, 2);
        n.mode = static_cast<NetMode>(mode);

        Label("Player name");
        ImGui::InputText("##playername", &n.playerName);

        Label("Network adapter");
        std::string current = n.bindAddress.empty() ? std::string("None") : n.bindAddress + " (not connected)";
        for (const NetAdapter& a : adapters_)
            if (a.ipv4 == n.bindAddress) current = a.name + "  " + a.ipv4;
        if (ImGui::BeginCombo("##adapter", current.c_str())) {
            for (const NetAdapter& a : adapters_) {
                std::string label = a.name + "  " + a.ipv4 + (a.radmin ? "  (Radmin)" : "");
                if (ImGui::Selectable(label.c_str(), a.ipv4 == n.bindAddress)) n.bindAddress = a.ipv4;
            }
            ImGui::EndCombo();
        }

        if (n.mode == NetMode::Join) {
            Label("Host address");
            ImGui::InputTextWithHint("##host", "26.x.x.x", &n.hostAddress);
        }
        if (n.mode != NetMode::Off) {
            Label("Port");
            if (ImGui::InputInt("##port", &n.port, 0)) n.port = std::clamp(n.port, 1024, 65535);
        }
        if (n.mode == NetMode::Host && !n.bindAddress.empty()) {
            std::string address = n.bindAddress + ":" + std::to_string(n.port);
            ImGui::Dummy(ImVec2(0, em * 0.25f));
            ImGui::TextUnformatted("Friends join with");
            ImGui::SameLine();
            ImGui::TextColored(kAccent, "%s", address.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) SDL_SetClipboardText(address.c_str());
        }
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        Hint("Online play arrives in roadmap phase 7. These settings are saved now so the runtime can use them.");
    }

    // ---- Modifiers ----

    void ModifiersPage() {
        Config::Modifiers& m = config_.mods;
        Heading("Player modifiers", "Tune how your skater handles. 1.00x is the original game.");
        auto slider = [](const char* label, const char* id, float* v, float lo, float hi) {
            Label(label);
            ImGui::SliderFloat(id, v, lo, hi, "%.2fx");
        };
        slider("Gravity", "##gravity", &m.gravity, 0.25f, 2.0f);
        slider("Pop height", "##pop", &m.popHeight, 0.5f, 3.0f);
        slider("Top speed", "##speed", &m.topSpeed, 0.5f, 2.0f);
        slider("Flip speed", "##flip", &m.flipSpeed, 0.5f, 2.0f);
        slider("Spin speed", "##spin", &m.spinSpeed, 0.5f, 2.0f);
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        if (ImGui::Button("Reset to original")) m = Config::Modifiers{};
        ImGui::Spacing();
        Hint("Saved now; they take effect once the runtime has located the game's physics tuning (roadmap phase 6).");
    }

    // ---- Footer ----

    void Launch() {
        Save();
        if (!game_.isSkate3) {
            SetStatus("Verify your Skate 3 copy on the Game page first.", true);
            page_ = kGame;
            return;
        }
        const char* base = SDL_GetBasePath();
        fs::path exe = PathFromUtf8(base ? base : "") / "s3native.exe";
        std::error_code ec;
        if (!fs::exists(exe, ec)) {
            SetStatus("The game runtime (s3native.exe) isn't built yet; it starts in roadmap phase 2.", true);
            return;
        }
        std::string exeUtf8 = PathToUtf8(exe);
        std::string configUtf8 = PathToUtf8(ConfigPath());
        const char* args[] = {exeUtf8.c_str(), "--config", configUtf8.c_str(), nullptr};
        if (SDL_Process* process = SDL_CreateProcess(args, false)) {
            SDL_DestroyProcess(process);  // the game keeps running on its own
            SetStatus("Game started.");
        } else {
            SetStatus(std::string("Couldn't start the game: ") + SDL_GetError(), true);
        }
    }

    void Footer() {
        float em = ImGui::GetFontSize();
        float startX = ImGui::GetCursorPosX();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        ImGui::AlignTextToFramePadding();
        if (!status_.empty())
            ImGui::TextColored(statusIsError_ ? kBad : kMuted, "%s", status_.c_str());
        else if (game_.isSkate3)
            ImGui::TextColored(kGood, "Ready");
        else
            ImGui::TextColored(kMuted, "Choose your game on the Game page.");

        float saveWidth = em * 5.5f, launchWidth = em * 8.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(startX + avail - saveWidth - launchWidth - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Save", ImVec2(saveWidth, 0))) Save();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.70f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.54f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
        if (ImGui::Button("Launch", ImVec2(launchWidth, 0))) Launch();
        ImGui::PopStyleColor(4);
    }

    SDL_Window* window_;
    ImFont* headingFont_;
    Config config_;
    GameCheck game_;
    bool gameChecked_ = false;
    std::optional<StfsInfo> save_;
    std::string saveMessage_;
    bool saveOk_ = false;
    std::vector<NetAdapter> adapters_;
    std::vector<std::pair<int, int>> resolutions_;
    std::pair<int, int> desktop_{0, 0};
    std::vector<SDL_JoystickID> pads_;
    SDL_Gamepad* testPad_ = nullptr;
    int page_ = kGame;
    std::string status_;
    bool statusIsError_ = false;
};

ImFont* LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    const char* regular = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";
    if (fs::exists(regular, ec)) {
        io.Fonts->AddFontFromFileTTF(regular, 17.0f * scale);
    } else {
        ImFontConfig config;
        config.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&config);
    }
    return fs::exists(bold, ec) ? io.Fonts->AddFontFromFileTTF(bold, 26.0f * scale) : nullptr;
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Skate 3 Native", SDL_GetError(), nullptr);
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) scale = 1.0f;

    SDL_Window* window = SDL_CreateWindow("Skate 3 Native - Launcher", int(1020 * scale), int(680 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Skate 3 Native", SDL_GetError(), window);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, int(820 * scale), int(540 * scale));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ApplyStyle(scale);
    ImFont* headingFont = LoadFonts(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        Launcher launcher(window, headingFont);
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
                case SDL_EVENT_GAMEPAD_ADDED:
                case SDL_EVENT_GAMEPAD_REMOVED: launcher.RefreshPads(); break;
                default: break;
                }
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(16);
                continue;
            }
            launcher.PollDialogs();

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            launcher.Frame();
            ImGui::Render();

            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColorFloat(renderer, 0.07f, 0.075f, 0.09f, 1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
