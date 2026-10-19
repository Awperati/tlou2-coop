// Linux build of coop_relay only (deploy/relay/Dockerfile): the session code that defines NowMs is Windows-only.
#include "core/session.h"
#include <chrono>

namespace coop {
SteadyMs NowMs() {
    return static_cast<SteadyMs>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace coop
