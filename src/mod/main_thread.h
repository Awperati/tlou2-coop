// Runs work on the game's window thread, between frames.
// Engine calls (events, teleports) are not safe from our own thread.
// A WH_GETMESSAGE hook on that thread drains a queue each time it pumps messages.
#pragma once
#include <functional>

namespace coop {

bool MainThreadInit();      // finds the game window; call again until true
void MainThreadShutdown();
void RunOnMainThread(std::function<void()> fn);

} // namespace coop
