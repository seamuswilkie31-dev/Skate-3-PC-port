#pragma once

// Controllers for the game: up to four pads through SDL3 (Xbox, PlayStation and Switch
// pads alike), with the keyboard and mouse as one more pad merged into user 0. The
// window's thread feeds it (each event, then a poll every loop); the title reads it on its
// own threads through XamInputGetState (kernel/input_imports.cpp).
//
// Keyboard and mouse: WASD is the left stick and mouse movement the right one (for the
// flick-it tricks); Space A, E B, Q X, F Y, Enter Start, Tab Back, Z and C the bumpers, the
// mouse buttons the triggers (right: left trigger), the arrows the D-pad, Left Ctrl and the
// middle button the stick clicks. Clicking in the window captures the mouse; Escape frees it.

#include "config.h"

#include <cstdint>
#include <string>

union SDL_Event;

namespace s3n::input {

// XInput's gamepad: buttons (below), triggers 0-255, sticks -32768-32767 with y up.
struct Gamepad {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// The window's thread: opens the pads (after the window, which initialises SDL).
void Init(const Config::Input& settings);
void HandleEvent(const SDL_Event& event);
void Poll();

// --press: scripted presses, as "button:seconds[,...]" (e.g. "start:30,a:36"), each held
// for a fifth of a second that long after start-up, on user 0. Buttons: a b x y start back
// lb rb ls rs up down left right lt rt. Works without a window, for checking runs.
bool ScriptPresses(const std::string& spec, std::string& error);

// The title's threads. User 0 is always connected (keyboard and mouse at least).
bool Connected(uint32_t user);
bool State(uint32_t user, Gamepad& pad, uint32_t& packet);
void SetVibration(uint32_t user, uint16_t left, uint16_t right);

}  // namespace s3n::input
