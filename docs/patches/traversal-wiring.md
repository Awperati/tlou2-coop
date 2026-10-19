# Traversal wiring: ready to apply (2026-10-20)

The core is committed and tested: `src/core/protocol.h` (`kState*`, `JumpFromState`) and
`src/core/traversal.h` (`TraversalCopy`). This note wires them into the mod. It waits for the fidget-sync session,
which is editing protocol.cpp, game_bridge.cpp/.h and dllmain.cpp. Apply it after that session commits.
Anchors are the code as of commit 0cfaa9b plus that session's gesture field.

## 1. Protocol: send the sender's state id

`src/core/protocol.h`, `struct PlayerState`, after the `gesture` field:
```cpp
    // The sender's player state machine state (name id, FNV-1a; kState* / traversal.h), 0 = unknown (2026-10-20).
    uint64_t state = 0;
```
`src/core/protocol.cpp`, `Write(Writer&, const PlayerState&)`, after `w.u64(s.gesture);`:
```cpp
    w.u64(s.state);
```
`Read(Reader&, PlayerState&)`, after the gesture read:
```cpp
    s.state = (r.ok() && r.remaining() >= 8) ? r.u64() : 0;
```
`tests/tests.cpp`, in the PlayerState round-trip test: set `s.state = 0x15A477E9F9A165ADull`, then check that it survives,
and that an older sender (8 bytes shorter) reads `state == 0`.

## 2. Sender: read the state, flag jumps from it

`src/mod/game_bridge.cpp`, next to `PlayerStanceOf` (a static helper):
```cpp
// The player's state machine state name id (sub_14133D6B0(player, out) -> pointer to it; FNV-1a of "Jump", "Fall"...).
static uint64_t PlayerStateOf(uintptr_t p) {
    __try {
        uint8_t buf[16] = {};
        auto r = reinterpret_cast<uint64_t* (*)(uintptr_t, void*)>(BuildRva(0x133D6B0))(p, buf);
        return r ? *r : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
```
`ReadLocalPlayer`, replace the jump block (`bool jumpedNow = jumpDet_.Update(...)`) with:
```cpp
        s.state = PlayerStateOf(p);
        bool byState = jumpState_.Update(now, s.state);
        bool bySpeed = jumpDet_.Update(now, s.pos.y, s.pos.x, s.pos.z);   // fallback when the state is unreadable
        bool jumpedNow = s.state ? byState : bySpeed;
        if (jumpedNow) s.flags |= kJumped;
        if (jumpedNow && !jumpSentPrev_) Log("jump: own player jumped (%s) at y %.2f, telling the other PC", s.state ? "state" : "speed", s.pos.y);
        jumpSentPrev_ = jumpedNow;
        if (cloneAnimLog_ && s.state != lastOwnState_) { lastOwnState_ = s.state; Log("animstate: own player state %016llx", static_cast<unsigned long long>(s.state)); }
```
`ReadLocalPlayer` must run on the game fiber for the call. If it doesn't, read the state in the fiber tick, store it
in a member, and copy that member into `s.state` here.
`game_bridge.h` members: `JumpFromState jumpState_; uint64_t lastOwnState_ = 0; TraversalCopy traversal_;`, and
`#include "core/traversal.h"`.

## 3. Receiver: traversal presses

`SetCloneMove` gains the state. In dllmain.cpp, pass `s.state` and add a `uint64_t state` parameter. Then, after the
kJumped block:
```cpp
    if (state) {   // a new sender: the state carries jumps too, so skip the kJumped tap (no double press)
        if (auto* t = traversal_.Update(GetTickCount64(), state)) Log("clonetraversal: %s -> button %llx", t->name, static_cast<unsigned long long>(t->button));
        cloneJumpUntil_ = 0;
    }
```
`TickCloneDrive`, next to `if (GetTickCount64() < cloneJumpUntil_) held |= 0x20;`:
```cpp
        if (uint64_t tb = traversal_.Held(GetTickCount64()); tb && !(cloneHeld_.load() & tb)) held |= tb;
```
Then extend `cloneButtonMask_` with 0x100 (LB, dodge) if it isn't already set. The default 1F0 already has it.

## 4. Live checks (owner, one at a time, PC A's game in front)

1. Jump on PC B: PC B logs `jump: own player jumped (state)`, PC A logs `clonetraversal: Jump`, and Dina jumps.
2. Climb a ledge on PC B: `clonetraversal: Edge`, and she climbs (or snaps up within 2 s).
3. Take a ladder on PC B: `clonetraversal: Ladder`. Is Y the right button? If she doesn't take it, try A.
4. Dodge (L1/LB) on PC B: `clonetraversal: Evade`, and she dodges.
