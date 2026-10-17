#include "core/teleport.h"
#include <cmath>

namespace coop {

bool TeleportController::Update(const Inputs& in) {
    if (cooldown_ > 0) cooldown_ -= in.dt;
    if (!in.holdPressed) needRelease_ = false;

    bool far = false;
    if (in.objective && in.objective->isTransition) {
        far = Distance(in.selfPos, in.objective->pos) > in.objective->radius;
        reason_ = "Partner is at the exit";
    } else if (in.anchorValid) {
        far = Distance(in.selfPos, in.anchorPos) > cfg_.farDistance;
        reason_ = "Too far from partner";
    }

    if (!far) { state_ = PromptState::Hidden; held_ = 0; return false; }
    if (in.dead) { state_ = PromptState::Blocked; reason_ = "Dead"; held_ = 0; return false; }
    if (cfg_.blockInCombat && in.inCombat) { state_ = PromptState::Blocked; reason_ = "In combat"; held_ = 0; return false; }
    if (cooldown_ > 0) { state_ = PromptState::Blocked; reason_ = "Cooling down"; held_ = 0; return false; }

    if (!in.holdPressed || needRelease_) { state_ = PromptState::Available; held_ = 0; return false; }

    state_ = PromptState::Charging;
    held_ += in.dt;
    if (held_ < cfg_.holdSeconds) return false;

    held_ = 0;
    cooldown_ = cfg_.cooldownSeconds;
    needRelease_ = true;   // holding through the cooldown must not re-fire
    state_ = PromptState::Hidden;
    return true;
}

TeleportTo ComputeTeleportTarget(const Vec3& anchorPos, float anchorYaw, const Objective* obj,
                                 uint8_t slot, const TeleportConfig& cfg) {
    TeleportTo t;
    t.yaw = anchorYaw;
    // Spread players on a small arc behind the anchor so they don't stack.
    float side = (slot % 2 ? 1.f : -1.f) * 0.8f * static_cast<float>((slot + 1) / 2);
    float c = std::cos(anchorYaw), s = std::sin(anchorYaw);
    Vec3 base = anchorPos;
    // An objective without a known spot comes as 0,0,0 (objective position isn't mapped yet):
    // using it sent the partner to the world origin, out of the map (2026-10-17).
    bool objKnown = obj && (obj->pos.x != 0.f || obj->pos.y != 0.f || obj->pos.z != 0.f);
    if (obj && obj->isTransition && objKnown) { base = obj->pos; t.objectiveId = obj->id; }
    // Engine convention (to confirm in RE): yaw 0 faces +Z, X is right, Y is up.
    t.pos.x = base.x - s * cfg.behindOffset + c * side;
    t.pos.y = base.y;
    t.pos.z = base.z - c * cfg.behindOffset - s * side;
    return t;
}

void TransitionGate::Begin(const Objective& obj) {
    obj_ = obj;
    active_ = true;
    waited_ = 0;
    teleported_.clear();
}

void TransitionGate::ReportPosition(uint8_t slot, const Vec3& pos) { positions_[slot] = pos; }

std::vector<uint8_t> TransitionGate::Stragglers() const {
    std::vector<uint8_t> out;
    for (auto& [slot, pos] : positions_) {
        auto t = teleported_.find(slot);
        if (t != teleported_.end() && t->second) continue;
        if (Distance(pos, obj_.pos) > obj_.radius) out.push_back(slot);
    }
    return out;
}

bool TransitionGate::Ready(float dt) {
    if (!active_) return false;
    waited_ += dt;
    if (Stragglers().empty()) { active_ = false; return true; }
    if (cfg_.transitionForceSeconds > 0 && waited_ >= cfg_.transitionForceSeconds) { active_ = false; return true; }
    return false;
}

} // namespace coop
