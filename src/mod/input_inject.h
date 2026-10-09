// Synthetic controller input from inside the game.
// The game ignores injected keyboard/mouse (raw input with no device), but it
// polls XInput through xinput1_4.dll. We patch XInputGetState and
// XInputGetCapabilities there and merge scripted buttons into pad 0.
#pragma once
#include <string>

namespace coop {

bool InputInjectInstall();
void InputInjectRemove();

// One script line, e.g. "press a 150", "press start", "stick l 0 32767 800",
// "trigger r 255 300", "wait 500". Steps run in order. Returns false if unknown.
bool InputScriptAdd(const std::string& line);
bool InputScriptBusy();
void InputLogStats();

} // namespace coop
