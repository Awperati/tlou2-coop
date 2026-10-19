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

## 4. Live checks for the owner (deployed 2026-10-20 05:56, both PCs, build 676f843)

Applied in dbe37e2 (wiring), 49806e9 (gating by her own state), 4b63c04 (ropes), 676f843 (facing lead). Do these one at
a time, on PC B, with PC A's game in front (her input only works then). After each one, tell the session the time.
It reads both logs with `tools/logstats.py`.

1. **Jump** (Space) standing, then while walking. PC B logs `jump: own player jumped (state)`, PC A logs
   `clonetraversal: Jump`, and Dina jumps.
2. **Ledge**: jump up to a ledge you can hang from. PC A logs `clonetraversal: Edge`, and she grabs it, or teleports
   up within about 2 s.
3. **Let go**: while both hang, drop off (Crouch/B). PC A logs `clonetraversal: LedgeDrop`, and she lets go too.
4. **Ladder**: take a ladder. PC A logs `clonetraversal: Ladder`. If she doesn't take it, the ladder button is not Y.
5. **Dodge** (L1/LB on a pad, the dodge key on keyboard). PC A logs `clonetraversal: Evade`, and she dodges.
6. **Look around while standing** (mouse). Dina should now turn with less delay than before, smooth and without an A-pose.
   If she overshoots when you stop, send `facelead 0.08` (or `0` to turn the lead off) in PC A's cmd.txt.
