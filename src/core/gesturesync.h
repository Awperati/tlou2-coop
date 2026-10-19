// Idle gesture sync (owner 10-20: "make sure the idle wiping nose animations line up").
// Each PC sends the gesture its own player plays on gesture layer 1 (PlayerState.gesture, a string
// id; 0 = none). Seen live on PC A over ~25 min: five different idle gestures (d8ad71f5..,
// d38b965a.., d846c30d.., a0a75315.., 49859335..), one about every 20 s, each 2.5-3.6 s. The clone
// never starts one herself, so the receiver starts the partner's on her.
// Game-free so it can be unit-tested; the bridge feeds it every 100 ms.
#pragma once
#include <cstdint>

namespace coop {

struct GestureSync {
    uint64_t played = 0;   // the partner gesture last started on the clone (0 after the partner's ends)

    // partner: the partner's current gesture; partnerMoving: their moving flag; cloneNow: what the
    // clone plays on layer 1 right now. Returns the gesture to start on the clone, or 0.
    // - Starts each partner gesture once, when it begins (or when we first see it).
    // - Only while the partner stands: idle gestures play standing; a moving partner's layer-1
    //   gesture (door push, pickup reach) is driven on the clone by her own move instead.
    // - Not if she already plays that very gesture.
    // - The same id again starts again once the partner's gesture ended in between (sent as 0).
    uint64_t Update(uint64_t partner, bool partnerMoving, uint64_t cloneNow, bool enabled = true) {
        if (!partner) { played = 0; return 0; }
        if (partner == played) return 0;
        if (!enabled || partnerMoving) return 0;   // not marked played: starts if they stop while it still runs
        played = partner;
        return cloneNow == partner ? 0 : partner;
    }
};

} // namespace coop
