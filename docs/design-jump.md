# Clone jump: design (2026-10-19, not built)

The owner: "it doesnt detect jumping yet". Crouch and prone already work (stance sync).

## Why it doesn't work today

- The clone's button mask 1F0 already forwards A (0x20, jump/vault) from the partner's raw pad.
- The partner on PC B plays with mouse and keyboard. A keyboard jump never shows up as a raw pad bit.
  Aim and fire had the same gap, which the state flags (bits 1 and 2) now cover.
- A pad partner's A press reaches her today. It is not verified live.

## What to send (PC B → PC A)

- A new state flag, bit 3: "jumped". It is set on the rising edge of a jump and held for 300 ms, so it
  survives a lost packet.
- Detection on the sender: a jump has no `player-is-jumping?` native. Use her own motion instead.
  - Vertical speed above +2.0 m/s, from a standing or walking state, counts as a jump.
  - Also usable: `get-player-jump-or-fall-top-pos-y` (native 0xE64350) gives the jump apex.
- Also send where it lands: the partner's position once vertical speed returns to ~0. This goes as a
  `WorldEvent` (kWorldJump: start x y z, land x y z).

## How the clone triggers it (PC A)

Two options, simplest first.

1. **Tap A.** On the jump flag's rising edge, hold A (0x20) on her raw pad for 100 ms through the existing
   raw-copy hook. The game picks the jump, vault or climb from her own surroundings.
2. **Force a jump to the landing spot.** Call `player-force-jump-at` (native 0xC9A170) with the partner's
   landing position, under `SetPlayerOverride(player 1)`.
   - The native gets its player from GetPlayer (0xDA8A80). Our override already redirects that to
     player 1, as the `props` check verified.
   - The game then plays its own jump animation toward the exact spot the partner reached.

Recommendation: start with option 1 because it is cheap. Move to option 2 if the logs show context misses.

## Context risks

- **Wrong context.** A is contextual: the same press is a jump, a vault, a climb, or nothing.
  - If she is 0.3 m off the partner's spot, she may face no ledge and do nothing.
  - Or she may hop in place while the partner climbs.
  - Then the drift snap moves her up after 2 s. That is visible but recoverable.
- **Turn and jump timing.** A needs her facing the ledge. FaceForce applies only while standing, and a
  jump comes mid-walk, so her own walk facing decides it.
- **Double jump.** The pad partner's A already reaches her. With the flag as well, the press could count
  twice. Take the flag path only when the flag arrives without the A bit.
- **Climbs and long traversal.** A ledge climb is a scripted move (traversal action pack). Our plot
  detection watches the companion, not the clone, so it won't interfere.
  - If a climb fails, the drift snap is the fallback.
  - The snap uses the partner's position, which may be up on the ledge. That works.
- **Death jumps.** `get-player-acc-clamber-vault-to-death?` exists. A forced jump-at must never target a
  spot below a drop that kills her. Use option 2 only for landings within 2 m in height.

## What to log

- `clonejump: partner jumped at <pos>, landed <pos>; clone pressed A / forced jump-at; clone in air <0/1>
  within 300 ms; height gain <m>`.
- Success means the clone's vertical speed passes +1.5 m/s within 300 ms of the event, and she ends up
  within 0.5 m of the landing spot.
