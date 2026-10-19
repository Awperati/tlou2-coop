// Traversal copying (2026-10-20, static RE; not yet live): the partner's player state (state machine name id,
// FNV-1a of the name, read with sub_14133D6B0 on the sender) tells the clone when they jump, climb a ledge,
// take a ladder, slide or dodge. Entering one of these states presses the button that starts it on the clone,
// in her own surroundings; the drift snap covers a miss. Game-free so it can be unit-tested.
// Buttons are the raw pad bits (A 0x20 jump/climb, B 0x80 crouch, Y 0x10 interact, LB 0x100 dodge).
#pragma once
#include <cstdint>

namespace coop {

enum class Traversal : uint8_t { None, Jump, Climb, Ladder, Slide, Dodge, Squeeze, Follow };

struct TraversalState { uint64_t id; Traversal kind; uint64_t button; unsigned ms; const char* name; };

// Follow: states the clone reaches through the drift snap only (no single button starts them).
inline constexpr TraversalState kTraversalStates[] = {
    {0x15A477E9F9A165ADull, Traversal::Jump,    0x20,  150, "Jump"},
    {0x6503256C2DE2745Cull, Traversal::Climb,   0x20,  150, "Edge"},
    {0x16948826BF750B89ull, Traversal::Ladder,  0x10,  150, "Ladder"},
    {0xEAF5B9CD31C0B966ull, Traversal::Slide,   0x80,  150, "Slide"},
    {0xAD64C266D7665E26ull, Traversal::Dodge,   0x100, 150, "Evade"},
    {0xDD860669F287283Cull, Traversal::Squeeze, 0x10,  150, "SqueezeThrough"},
    {0x6A1BC0BFDD271822ull, Traversal::Follow,  0,     0,   "WallShimmy"},
    {0x30D1FD450727A3CBull, Traversal::Follow,  0,     0,   "LedgeDrop"},
    {0x9ACB4182871CE5A3ull, Traversal::Follow,  0,     0,   "FreeRope"},
    {0x3B3CDD9BE9F96F46ull, Traversal::Follow,  0,     0,   "Zipline"},
    {0xCF0D3D2518907C4Dull, Traversal::Follow,  0,     0,   "Swim"},
};

inline const TraversalState* FindTraversal(uint64_t id) {
    for (const auto& s : kTraversalStates) if (s.id == id) return &s;
    return nullptr;
}

// Receiver side: feed the partner's state each packet. On entering a pressable traversal state, Update
// returns it once and Held() gives the button to hold until its time is up. A state the partner was
// already in (packet repeats, reconnects) does not press again.
struct TraversalCopy {
    const TraversalState* Update(unsigned long long now, uint64_t state) {
        const TraversalState* hit = nullptr;
        if (state != prev_) {
            const TraversalState* t = FindTraversal(state);
            if (t && t->button) { hit = t; button_ = t->button; until_ = now + t->ms; }
        }
        prev_ = state;
        return hit;
    }
    uint64_t Held(unsigned long long now) const { return now < until_ ? button_ : 0; }
private:
    uint64_t prev_ = 0, button_ = 0; unsigned long long until_ = 0;
};

} // namespace coop
