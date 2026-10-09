// Dying Light style catch-up teleport.
//  * TeleportController runs on each player's PC: decides when the
//    "Hold [T] to teleport" prompt shows and turns a held button into a request.
//  * TransitionGate runs on the host: holds a scene transition until every
//    player is inside the objective radius, has teleported, or the wait expires.
#pragma once
#include "core/protocol.h"
#include <map>
#include <optional>

namespace coop {

struct TeleportConfig {
    float holdSeconds = 1.5f;        // how long the button must be held
    float farDistance = 35.f;        // metres from the anchor before the prompt shows
    float cooldownSeconds = 5.f;
    bool blockInCombat = false;      // Dying Light allows teleporting mid-fight
    float transitionForceSeconds = 0.f;  // 0 = wait forever for stragglers
    float behindOffset = 1.5f;       // drop-in point behind the anchor
};

enum class PromptState { Hidden, Available, Charging, Blocked };

class TeleportController {
public:
    explicit TeleportController(TeleportConfig cfg = {}) : cfg_(cfg) {}

    struct Inputs {
        float dt = 0;
        Vec3 selfPos;
        Vec3 anchorPos;            // the player who is at the objective (usually host)
        bool anchorValid = false;
        const Objective* objective = nullptr;
        bool holdPressed = false;
        bool inCombat = false;
        bool dead = false;
    };

    // Returns true on the frame the hold completes; caller sends TeleportRequest.
    bool Update(const Inputs& in);

    PromptState state() const { return state_; }
    float progress() const { return cfg_.holdSeconds > 0 ? held_ / cfg_.holdSeconds : 1.f; }
    const char* reason() const { return reason_; }
    void setConfig(const TeleportConfig& c) { cfg_ = c; }

private:
    TeleportConfig cfg_;
    PromptState state_ = PromptState::Hidden;
    float held_ = 0;
    float cooldown_ = 0;
    bool needRelease_ = false;
    const char* reason_ = "";
};

// Host-side: picks where a teleporting player lands.
TeleportTo ComputeTeleportTarget(const Vec3& anchorPos, float anchorYaw, const Objective* obj,
                                 uint8_t slot, const TeleportConfig& cfg);

class TransitionGate {
public:
    explicit TransitionGate(TeleportConfig cfg = {}) : cfg_(cfg) {}

    // Host calls when the game wants to start a scene transition.
    void Begin(const Objective& obj);
    void Cancel() { active_ = false; }
    bool active() const { return active_; }

    void ReportPosition(uint8_t slot, const Vec3& pos);
    void MarkTeleported(uint8_t slot) { teleported_[slot] = true; }
    void ForgetPlayer(uint8_t slot) { positions_.erase(slot); teleported_.erase(slot); }

    // True once the transition may proceed. Call every frame while active.
    bool Ready(float dt);
    // Slots still outside the radius; they get the teleport prompt.
    std::vector<uint8_t> Stragglers() const;
    const Objective& objective() const { return obj_; }

private:
    TeleportConfig cfg_;
    Objective obj_;
    bool active_ = false;
    float waited_ = 0;
    std::map<uint8_t, Vec3> positions_;
    std::map<uint8_t, bool> teleported_;
};

} // namespace coop
