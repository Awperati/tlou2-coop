// Accessible prompt layer that works before any renderer hook exists:
//  * a click-through, always-on-top window over the game (borderless/windowed),
//  * spoken prompts through Windows SAPI,
//  * short sound cues.
#pragma once
#include <string>

namespace coop {

struct OverlayOptions {
    bool speak = true;
    bool sounds = true;
};

void OverlayStart(const OverlayOptions& opt);
void OverlayStop();

// Prompt line in the centre (teleport). progress < 0 hides the bar.
void OverlaySetPrompt(const std::wstring& text, float progress);
void OverlayClearPrompt();
// Small status line in the corner (connection, ping).
void OverlaySetStatus(const std::wstring& text);
// One-off notice: shown for a few seconds and spoken.
void OverlayNotify(const std::wstring& text);
void OverlayCue(int kind);   // 0 prompt shown, 1 teleport fired, 2 player joined, 3 player left

} // namespace coop
