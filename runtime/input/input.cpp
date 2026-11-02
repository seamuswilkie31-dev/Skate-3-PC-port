#include "input/input.h"

#include "kernel/kernel.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace s3n::input {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point gStart = Clock::now();

constexpr uint32_t kUsers = 4;
constexpr float kMouseGain = 0.03f;   // stick deflection per pixel of movement
constexpr float kMouseDecay = 0.6f;   // what's left of the deflection after a poll without movement
constexpr double kPressSeconds = 0.2;

Config::Input gSettings;
bool gInitialized = false;

// The window's thread.
std::array<SDL_Gamepad*, kUsers> gPads{};
std::array<bool, SDL_SCANCODE_COUNT> gKeys{};
bool gMouseLeft = false, gMouseRight = false, gMouseMiddle = false;
float gMouseDX = 0.0f, gMouseDY = 0.0f;  // movement since the last poll
float gRightX = 0.0f, gRightY = 0.0f;    // the mouse's right stick
std::array<uint16_t, kUsers> gRumbleApplied[2]{};

// Shared with the title's threads.
std::mutex gMutex;
std::array<Gamepad, kUsers> gState{};
std::array<bool, kUsers> gConnected{};
std::array<uint32_t, kUsers> gPackets{};
std::array<uint16_t, kUsers> gRumble[2]{};  // left (low-frequency) and right motors

struct Press {
    uint16_t button;
    bool leftTrigger, rightTrigger;
    double at;
};
std::vector<Press> gScript;
std::atomic<uint64_t> gStateReads{0};

std::string Describe() {
    std::lock_guard lock(gMutex);
    uint32_t pads = 0;
    for (uint32_t u = 0; u < kUsers; ++u) pads += gPads[u] != nullptr;
    return "input: " + std::to_string(pads) + " pads, keyboard and mouse on user 0; " +
           std::to_string(gStateReads.load()) + " state reads, " + std::to_string(gScript.size()) +
           " scripted presses\n";
}

int16_t ToStick(float value) { return int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f); }

// A stick with a radial dead zone, rescaled so it still reaches full deflection.
void Stick(float x, float y, float deadzone, int16_t& outX, int16_t& outY) {
    float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone || magnitude == 0.0f) {
        outX = outY = 0;
        return;
    }
    float scale = std::min((magnitude - deadzone) / (1.0f - deadzone), 1.0f) / magnitude;
    outX = ToStick(x * scale);
    outY = ToStick(y * scale);
}

uint8_t Trigger(float value, float threshold) {
    if (value <= threshold) return 0;
    return uint8_t(std::clamp((value - threshold) / (1.0f - threshold), 0.0f, 1.0f) * 255.0f);
}

Gamepad ReadPad(SDL_Gamepad* pad) {
    Gamepad g;
    // Face buttons by position (A at the bottom), unless a Switch pad should go by label.
    SDL_GamepadType type = SDL_GetGamepadType(pad);
    bool nintendo = gSettings.nintendoLayout && (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
                                                 type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
    struct Map {
        SDL_GamepadButton from;
        uint16_t to;
    };
    const Map kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, nintendo ? kB : kA},  {SDL_GAMEPAD_BUTTON_EAST, nintendo ? kA : kB},
        {SDL_GAMEPAD_BUTTON_WEST, nintendo ? kY : kX},   {SDL_GAMEPAD_BUTTON_NORTH, nintendo ? kX : kY},
        {SDL_GAMEPAD_BUTTON_START, kStart},              {SDL_GAMEPAD_BUTTON_BACK, kBack},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb},     {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp},           {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight},
    };
    for (const Map& m : kButtons)
        if (SDL_GetGamepadButton(pad, m.from)) g.buttons |= m.to;
    auto axis = [pad](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0f; };
    // SDL's y axes point down; XInput's up.
    Stick(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY), gSettings.leftDeadzone, g.thumbLX, g.thumbLY);
    Stick(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY), gSettings.rightDeadzone, g.thumbRX,
          g.thumbRY);
    g.leftTrigger = Trigger(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), gSettings.triggerThreshold);
    g.rightTrigger = Trigger(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), gSettings.triggerThreshold);
    return g;
}

Gamepad ReadKeyboardAndMouse() {
    Gamepad g;
    struct Map {
        SDL_Scancode from;
        uint16_t to;
    };
    static constexpr Map kKeys[] = {
        {SDL_SCANCODE_SPACE, kA},      {SDL_SCANCODE_E, kB},            {SDL_SCANCODE_Q, kX},
        {SDL_SCANCODE_F, kY},          {SDL_SCANCODE_RETURN, kStart},   {SDL_SCANCODE_TAB, kBack},
        {SDL_SCANCODE_Z, kLeftShoulder}, {SDL_SCANCODE_C, kRightShoulder}, {SDL_SCANCODE_LCTRL, kLeftThumb},
        {SDL_SCANCODE_UP, kDpadUp},    {SDL_SCANCODE_DOWN, kDpadDown},  {SDL_SCANCODE_LEFT, kDpadLeft},
        {SDL_SCANCODE_RIGHT, kDpadRight},
    };
    for (const Map& m : kKeys)
        if (gKeys[m.from]) g.buttons |= m.to;
    if (gMouseMiddle) g.buttons |= kRightThumb;
    float x = float(gKeys[SDL_SCANCODE_D]) - float(gKeys[SDL_SCANCODE_A]);
    float y = float(gKeys[SDL_SCANCODE_W]) - float(gKeys[SDL_SCANCODE_S]);
    if (x && y) x *= 0.7071f, y *= 0.7071f;
    g.thumbLX = ToStick(x);
    g.thumbLY = ToStick(y);
    // The mouse's movement deflects the right stick, which springs back when it stops.
    gRightX = std::clamp(gRightX * kMouseDecay + gMouseDX * kMouseGain, -1.0f, 1.0f);
    gRightY = std::clamp(gRightY * kMouseDecay - gMouseDY * kMouseGain, -1.0f, 1.0f);
    if (std::abs(gRightX) < 0.02f) gRightX = 0.0f;
    if (std::abs(gRightY) < 0.02f) gRightY = 0.0f;
    gMouseDX = gMouseDY = 0.0f;
    g.thumbRX = ToStick(gRightX);
    g.thumbRY = ToStick(gRightY);
    g.leftTrigger = gMouseRight ? 255 : 0;
    g.rightTrigger = gMouseLeft ? 255 : 0;
    return g;
}

// Both at once on user 0: buttons combined, the stronger trigger and stick.
void Merge(Gamepad& into, const Gamepad& from) {
    into.buttons |= from.buttons;
    into.leftTrigger = std::max(into.leftTrigger, from.leftTrigger);
    into.rightTrigger = std::max(into.rightTrigger, from.rightTrigger);
    auto stronger = [](int16_t& x, int16_t& y, int16_t fx, int16_t fy) {
        if (int32_t(fx) * fx + int32_t(fy) * fy > int32_t(x) * x + int32_t(y) * y) x = fx, y = fy;
    };
    stronger(into.thumbLX, into.thumbLY, from.thumbLX, from.thumbLY);
    stronger(into.thumbRX, into.thumbRY, from.thumbRX, from.thumbRY);
}

void ReleaseAll() {
    gKeys.fill(false);
    gMouseLeft = gMouseRight = gMouseMiddle = false;
}

}  // namespace

void Init(const Config::Input& settings) {
    gSettings = settings;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        kernel::Log("  input: no pads: SDL_InitSubSystem: %s\n", SDL_GetError());
    gInitialized = true;
    kernel::AddReportSection(Describe);
}

void HandleEvent(const SDL_Event& event) {
    if (!gInitialized) return;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        for (SDL_Gamepad*& pad : gPads) {
            if (pad) continue;
            pad = SDL_OpenGamepad(event.gdevice.which);
            if (pad) kernel::Log("  input: %s connected\n", SDL_GetGamepadName(pad));
            break;
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (SDL_Gamepad*& pad : gPads) {
            if (pad && SDL_GetGamepadID(pad) == event.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < SDL_SCANCODE_COUNT) gKeys[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
            if (SDL_Window* window = SDL_GetWindowFromID(event.key.windowID))
                SDL_SetWindowRelativeMouseMode(window, false);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
            window && SDL_GetWindowRelativeMouseMode(window)) {
            gMouseDX += event.motion.xrel;
            gMouseDY += event.motion.yrel;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
        if (down && window && !SDL_GetWindowRelativeMouseMode(window)) {  // the first click captures
            SDL_SetWindowRelativeMouseMode(window, true);
            break;
        }
        if (event.button.button == SDL_BUTTON_LEFT) gMouseLeft = down;
        if (event.button.button == SDL_BUTTON_RIGHT) gMouseRight = down;
        if (event.button.button == SDL_BUTTON_MIDDLE) gMouseMiddle = down;
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ReleaseAll();
        if (SDL_Window* window = SDL_GetWindowFromID(event.window.windowID))
            SDL_SetWindowRelativeMouseMode(window, false);
        break;
    default:
        break;
    }
}

void Poll() {
    if (!gInitialized) return;
    std::array<Gamepad, kUsers> state{};
    std::array<bool, kUsers> connected{};
    for (uint32_t u = 0; u < kUsers; ++u) {
        if (!gPads[u]) continue;
        state[u] = ReadPad(gPads[u]);
        connected[u] = true;
    }
    Merge(state[0], ReadKeyboardAndMouse());
    connected[0] = true;

    std::array<uint16_t, kUsers> rumble[2];
    {
        std::lock_guard lock(gMutex);
        for (uint32_t u = 0; u < kUsers; ++u) {
            if (std::memcmp(&state[u], &gState[u], sizeof(Gamepad)) != 0) ++gPackets[u];
            gState[u] = state[u];
            gConnected[u] = connected[u];
        }
        rumble[0] = gRumble[0];
        rumble[1] = gRumble[1];
    }
    // Vibration the title asked for, on the pads (scaled by the launcher's strength).
    for (uint32_t u = 0; u < kUsers; ++u) {
        if (!gPads[u] || (rumble[0][u] == gRumbleApplied[0][u] && rumble[1][u] == gRumbleApplied[1][u])) continue;
        gRumbleApplied[0][u] = rumble[0][u];
        gRumbleApplied[1][u] = rumble[1][u];
        float strength = gSettings.vibration ? gSettings.vibrationStrength : 0.0f;
        SDL_RumbleGamepad(gPads[u], uint16_t(rumble[0][u] * strength), uint16_t(rumble[1][u] * strength),
                          rumble[0][u] || rumble[1][u] ? 60000 : 0);
    }
}

bool ScriptPresses(const std::string& spec, std::string& error) {
    struct Name {
        const char* name;
        uint16_t button;
        bool left, right;
    };
    static constexpr Name kNames[] = {
        {"a", kA}, {"b", kB}, {"x", kX}, {"y", kY}, {"start", kStart}, {"back", kBack},
        {"lb", kLeftShoulder}, {"rb", kRightShoulder}, {"ls", kLeftThumb}, {"rs", kRightThumb},
        {"up", kDpadUp}, {"down", kDpadDown}, {"left", kDpadLeft}, {"right", kDpadRight},
        {"lt", 0, true, false}, {"rt", 0, false, true},
    };
    std::stringstream items(spec);
    std::string item;
    while (std::getline(items, item, ',')) {
        size_t colon = item.find(':');
        std::string name = item.substr(0, colon);
        const Name* found = nullptr;
        for (const Name& n : kNames)
            if (name == n.name) found = &n;
        if (!found || colon == std::string::npos) {
            error = "--press: \"" + item + "\" isn't button:seconds";
            return false;
        }
        gScript.push_back({found->button, found->left, found->right, std::atof(item.c_str() + colon + 1)});
    }
    return true;
}

bool Connected(uint32_t user) {
    if (user >= kUsers) return false;
    if (user == 0) return true;
    std::lock_guard lock(gMutex);
    return gConnected[user];
}

bool State(uint32_t user, Gamepad& pad, uint32_t& packet) {
    if (!Connected(user)) return false;
    ++gStateReads;
    {
        std::lock_guard lock(gMutex);
        pad = gState[user];
        packet = gPackets[user] * 2;
    }
    if (user == 0 && !gScript.empty()) {
        double now = std::chrono::duration<double>(Clock::now() - gStart).count();
        for (const Press& p : gScript) {
            if (now < p.at || now >= p.at + kPressSeconds) continue;
            pad.buttons |= p.button;
            if (p.leftTrigger) pad.leftTrigger = 255;
            if (p.rightTrigger) pad.rightTrigger = 255;
            packet |= 1;  // a different state, a different packet number
        }
    }
    return true;
}

void SetVibration(uint32_t user, uint16_t left, uint16_t right) {
    if (user >= kUsers) return;
    std::lock_guard lock(gMutex);
    gRumble[0][user] = left;
    gRumble[1][user] = right;
}

}  // namespace s3n::input
