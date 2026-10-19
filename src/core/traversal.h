// Traversal copying (2026-10-20, static RE; not yet live): the partner's player state (state machine name id,
// FNV-1a of the name, read with sub_14133D6B0 on the sender) tells the clone when they jump, climb a ledge,
// take a ladder, slide or dodge. Entering one of these states presses the button that starts it on the clone,
// in her own surroundings; the drift snap covers a miss. Game-free so it can be unit-tested.
// Buttons are the raw pad bits, in the game's default pad layout (PlayStation glyphs in its joypad script:
// cross, circle, triangle, square, l1 -> A 0x20 jump/climb, B 0x80 crouch, Y 0x10 interact, LB 0x100 dodge).
// Checked offline 2026-10-20: dc1/joypad.bin names only the glyphs (actions are hashed), so the action->button
// pairs are the game's published default layout. Slide (mud slopes) and SqueezeThrough start by themselves when
// she walks in, so they follow; the ladder press (Y) is the one pairing still to see live.
#pragma once
#include <atomic>
#include <cstdint>

namespace coop {

enum class Traversal : uint8_t { None, Jump, Climb, Ladder, Slide, Dodge, Squeeze, Drop, Follow };

// Clone-state gating (her own state, read on her PC): `only` = press only while she is in one of these
// (0 = any), `skip` = never while she is in one of these (she is already doing it, or it would mean something
// else there: B while standing is a crouch, A while hanging is a climb).
struct TraversalState {
    uint64_t id; Traversal kind; uint64_t button; unsigned ms; const char* name;
    uint64_t only[2]; uint64_t skip[3];
};

namespace tstate {
constexpr uint64_t Jump = 0x15A477E9F9A165ADull, Fall = 0x1999418517F28100ull, Edge = 0x6503256C2DE2745Cull,
                   Ladder = 0x16948826BF750B89ull, Shimmy = 0x6A1BC0BFDD271822ull, Drop = 0x30D1FD450727A3CBull;
}

// Follow: states the clone reaches by walking after the partner (her stick drive) or the drift snap.
// Ropes and ziplines (exe strings, 2026-10-20): a rope is grabbed out of a jump (ClimbToRope::CanJump / DoJump,
// JumpToRope, AutoClimbDownToRope), so the partner's Jump press already carries her onto it when one is in
// reach; ClimbDownToRopeViaPrompt and the zipline (PlayerZiplineController) start from a prompt or a jump too.
// So FreeRope and Zipline stay follow-only: no press of their own, which could only misfire.
// LedgeDrop: hanging on a ledge, B lets go (only pressed while she hangs too).
inline constexpr TraversalState kTraversalStates[] = {
    {tstate::Jump,          Traversal::Jump,    0x20,  150, "Jump",           {0, 0}, {tstate::Jump, tstate::Fall, tstate::Edge}},
    {tstate::Edge,          Traversal::Climb,   0x20,  150, "Edge",           {0, 0}, {tstate::Edge, tstate::Shimmy, tstate::Ladder}},
    {tstate::Ladder,        Traversal::Ladder,  0x10,  150, "Ladder",         {0, 0}, {tstate::Ladder, tstate::Edge, tstate::Shimmy}},
    {0xAD64C266D7665E26ull, Traversal::Dodge,   0x100, 150, "Evade",          {0, 0}, {tstate::Jump, tstate::Edge, tstate::Ladder}},
    {tstate::Drop,          Traversal::Drop,    0x80,  150, "LedgeDrop",      {tstate::Edge, tstate::Shimmy}, {0, 0, 0}},
    {0xEAF5B9CD31C0B966ull, Traversal::Slide,   0,     0,   "Slide",          {0, 0}, {0, 0, 0}},
    {0xDD860669F287283Cull, Traversal::Squeeze, 0,     0,   "SqueezeThrough", {0, 0}, {0, 0, 0}},
    {tstate::Shimmy,        Traversal::Follow,  0,     0,   "WallShimmy",     {0, 0}, {0, 0, 0}},
    {0x9ACB4182871CE5A3ull, Traversal::Follow,  0,     0,   "FreeRope",       {0, 0}, {0, 0, 0}},
    {0x3B3CDD9BE9F96F46ull, Traversal::Follow,  0,     0,   "Zipline",        {0, 0}, {0, 0, 0}},
    {0xCF0D3D2518907C4Dull, Traversal::Follow,  0,     0,   "Swim",           {0, 0}, {0, 0, 0}},
};

inline bool TraversalAllowed(const TraversalState& t, uint64_t cloneState) {
    for (uint64_t s : t.skip) if (s && s == cloneState) return false;
    if (!t.only[0] && !t.only[1]) return true;
    for (uint64_t s : t.only) if (s && s == cloneState) return true;
    return false;
}

inline const TraversalState* FindTraversal(uint64_t id) {
    for (const auto& s : kTraversalStates) if (s.id == id) return &s;
    return nullptr;
}

// Receiver side: feed the partner's state each packet. On entering a pressable traversal state, Update
// returns it once and Held() gives the button to hold until its time is up, while her own state allows it
// (TraversalAllowed; checked each frame, so a press stops as soon as she gets there herself). A state the
// partner was already in (packet repeats, reconnects) does not press again.
// Threads (review 10-20): Update runs on the mod's network thread (SetCloneMove), Held on a game job thread
// (TickCloneDrive). The press (which state, until when) is one atomic word, so Held never pairs a new state
// with the old deadline or the other way round (that tear could press the previous button for a frame).
struct TraversalCopy {
    const TraversalState* Update(unsigned long long now, uint64_t state) {   // one writer thread
        const TraversalState* hit = nullptr;
        if (state != prev_) {
            const TraversalState* t = FindTraversal(state);
            if (t && t->button) { hit = t; press_.store(Pack(t, now + t->ms), std::memory_order_release); }
        }
        prev_ = state;
        return hit;
    }
    uint64_t Held(unsigned long long now, uint64_t cloneState = 0) const {   // any thread
        uint64_t w = press_.load(std::memory_order_acquire);
        const TraversalState* t = Active(w);
        if (!t || now >= (w & kUntilMask)) return 0;
        return TraversalAllowed(*t, cloneState) ? t->button : 0;
    }
    // The word: table index + 1 in the top 8 bits (0 = none), the deadline (ms) in the low 56.
    static constexpr uint64_t kUntilMask = (1ull << 56) - 1;
    static uint64_t Pack(const TraversalState* t, unsigned long long until) {
        uint64_t idx = static_cast<uint64_t>(t - kTraversalStates) + 1;
        return (idx << 56) | (until & kUntilMask);
    }
    static const TraversalState* Active(uint64_t w) {
        uint64_t idx = w >> 56;
        constexpr uint64_t n = sizeof(kTraversalStates) / sizeof(kTraversalStates[0]);
        return idx && idx <= n ? &kTraversalStates[idx - 1] : nullptr;
    }
private:
    uint64_t prev_ = 0;
    std::atomic<uint64_t> press_{0};
};

} // namespace coop
