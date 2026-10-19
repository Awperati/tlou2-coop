# TLOU2 LAN co-op mod: handoff (2026-10-19, 14:15)

Repo: `C:\Users\User\TLOU2 Co-op` (GitHub Awperati/tlou2-coop, private). Written at commit `8605e8a` + this file.
Game: `E:\Games\The Last of Us - Part II Remastered` (Steam, v1.0.10402.1014, Watchtower save "Capitol Hill").
PC A = host (192.168.0.237, owner plays with a controller). PC B = partner (192.168.0.238, keyboard and mouse,
SSH `ssh -i ~/.ssh/tlou2_pcb sharn@192.168.0.238`).
Full findings log: [ROADMAP.md](ROADMAP.md). Player guide: [PLAYING.md](PLAYING.md). Jump design: [design-jump.md](design-jump.md).

## 1. Where it stands

| Area | Status (owner-checked unless marked) |
|---|---|
| Session, discovery, relay (M8), overlay, auto-rejoin, hot reload | Done. Leak-free over 5 reloads |
| Clone mode | The partner is a real second Player (player 1) on each PC, in their look |
| Clone walking | Owner 13:2x: "both their movements feel great and work perfectly". The partner's world stick goes straight into her engine world stick |
| Catch-up | Steering pull on, no sprint. Same-start test: p95 drift 1.24 m with pull vs 2.38 m without, 0 snaps |
| Position | "Position matches correctly based on either side" (owner) |
| Facing while walking | Her own locomotion. Matches the partner's body within ~0.1 rad (log) |
| Facing while standing | Snaps to the partner's CAMERA yaw (teleport to the same spot). Owner: "still snaps to the right orientation". Choppy, no turn animation |
| Smooth standing turn | Built (FaceForce) but OFF: it turned each PC's own player too. See 4.1 |
| Stance | Crouch and prone confirmed by the owner |
| Body | 8 meshes and 3 cloth props match PC B's own Dina. Owner: torso and arms back (14:0x) |
| Hood | Owner: "Ellie hood is great" |
| Aim | Seen on video: she aims where PC B aims. Shots untested live |
| Jump | Built, not yet seen working. See 4.2 |
| Pickups | Log-only. Received pickups now log their class. Live name check still open |
| Enemies, checkpoints, pause, plot moments | As in ROADMAP (M6/M7) |

Not bugs (owner confirmed):
- Walking backwards on a sudden S/back press is the game's own keyboard behaviour. Ellie on PC A does it too with a keyboard.
- Dina teleports when PC A's game is behind another window. Her input is focus-gated, so only drift snaps move her. Keep PC A's game in front.

## 2. Job structure (how this work is run)

- **This session** builds, deploys and tests. It drives both PCs itself: PC A locally, PC B over SSH.
  It runs scripted input with the virtual pad commands (`stick`, `press`, `trigger`).
- **Watchdog "Roblox Game 2 - Opus Fast"** (session local_6c52f15b-…) is the owner's relay and reviewer.
  - Report every step to it with SendMessage: what changed, the evidence, the commit, and one ask for the owner.
  - It forwards the owner's words (often from their phone) and approves or redirects.
  - The app pauses cross-session sends after 10 without a user message.
- **"Godot game fundamentals scope"** (local_80a3796e-…) is the technical director when it's open.
- **Owner checks** go in the wrap-up box as ONE short question with two buttons. The owner answers by eye.
  Ask only for what logs can't show: animation quality, what a screen looks like, real keyboard input.
- **Loop per change:**
  1. Read logs, then IDA if needed.
  2. Make the change.
  3. Unit test (`build.cmd` runs coop_tests, 187 checks).
  4. Deploy (`deploy.cmd`; PC B pulls coop.dll from `tools/share.py` within about a minute, so keep that running).
  5. Scripted test with metrics: drift p95, snaps, yaw lines.
  6. Commit with the numbers in the message.
  7. Report to the watchdog.
  8. Ask the owner for one eye check.

## 3. Rules (from the owner, also in memory)

- Don't stop at "reply X". Carry on, and test and verify yourself (logs, memory reads, recordings).
- **Banned:** focus tricks, bringing the game to the front by any method (also minimising windows), hidden launches,
  patching the unfocused-input gate, getting around permission denials.
  - `tools/live/fg.ps1` was refused by the permission classifier on 10-19. Read focus from the drive log instead
    (`game in front 0/1`).
- No coop.ini edits on PC B without the owner's OK. No game files, recordings or `shots/` in git.
- Launching and relaunching both games after crashes is allowed.
- Never touch the `_RUNE CrackFix` folder.

## 4. Open work, in priority order (proposed jobs)

### 4.1 Smooth standing turn without the leak (FaceForce)
- **Mechanism (works):** native `player-force-facing/f` sets a global flag (0x40FC87E) and a direction (0x40FC880).
  The movement controller update `sub_140C31230(ctl, req)` → `sub_140C31340` uses them as desired facing ([ctl+0x1B0]).
  ctl+280 is the player.
  - The hook sets both only around player 1's update (`FaceCtlHookInstall`, fiber_hook.cpp).
  - She turns through the game's own turn, smoothly, with no A-pose (owner 13:0x).
- **Leak:** each PC's own player 0 ends up with exactly her forced yaw (13:39: PC A body -3.09 = its clone's target;
  PC B body 1.56 = its clone's target).
  - Ruled out: a cross-thread race (updates serialized, 0 give-ups), nesting (0), GetPlayer inside her update.
  - Ruled out: a write to player 0 or its controller during her update. A `facediff` memory diff over 0x5800 + 0x800 bytes showed 0 dwords changed.
- **Static RE (10-20 night, PC B off, so no live run):**
  - Only sub_140C31340 reads the flag and dir (rva 0xC31A36 / 0xC31A44). The native sub_140DB63B0 is the only other writer.
  - The forced dir goes only into her own state: [ctl+0x1B0], her stored facing [ctl+0x890] + [ctl+0x8E0]=1
    (sub_140C34C80), and her player's key/value store (player+0x50B8, key 0xF8EDCB8F4C46F387, player-local dir).
    sub_141F819E0 / sub_141F80BF0 are pure maths.
  - Controller: vtable 0x142CC3CC0, size 0x950, desired-facing getter slot +0x150 (sub_140C2EB10).
    Per-frame update sub_140C37110 (slot +0x158), enter sub_140C35ED0. Each controller sits at player+0x14ADC0.
  - PC B log 13:31-13:40 (force on, lock on, 0 give-ups, 0 nested): its own yaw followed the clone's yaw each time
    (-1.21, 0.18, 1.56). With the force off it did not. 13:1x: its own player kept her yaw while walking,
    as if player 0 itself saw the force.
- **Next (one live run, both standing):**
  1. `faceforce on`, then `facewatch 3000 glob`. The report lists every instruction that touched the flag/dir,
     with the `rdi` (= controller in C31340) and how often the flag was set then.
     A hit at 0xC31A36 with player 0's controller and "set" > 0 means player 0 reads the force after all.
  2. `facewatch 3000 ctl`: every reader of her [ctl+0x1B0] / [ctl+0x8E0]. Anything outside sub_140C31340 /
     her own anim is the leak path. `faceptrs` prints both players' pointers to name the `rdi` values.
  3. `faceforce stored`: turns her through her stored facing (sub_140C34C80), no globals at all.
     If player 0 stays put and she still turns smoothly, that is the fix.
  - `facewatch` uses debug registers on all game threads (offline test: 3/3 clean). Untried in the game.
- Switches: `faceforce on|stored|off` (off removes the hook), `standturnto camera|body`, `standturnstep <rad>`, `standturnms <ms>`.

### 4.2 Jump
- **Sender:** `JumpDetector` (protocol.h). Fires on a rise above 1.1 m/s with a sudden onset (more than 0.8 m/s
  over the lower of the last two windows), held 300 ms. It sets state flag `kJumped` (0x20).
- **Receiver:** taps A (0x20) for 150 ms on the rising edge, unless the partner's pad already holds A.
- **History:**
  - 2 m/s missed the real jumps (peak ~1.45 m/s).
  - The "steeper than ground speed" rule missed the 14:04 jumps, because a TLOU2 standing jump hops forward 1.7-3 m/s.
  - The current rule fires on all 3 real 14:04 windows when applied by hand. Not yet tested live after deploy.
- **To verify:**
  1. PC B logs `jump: own player jumped`.
  2. PC A logs `clonejump: ... tapping A`.
  3. The owner sees her jump.
- If the tap doesn't jump her: scripted `press a` lifted neither player at this story spot, so try elsewhere, or
  use `player-force-jump-at` (native 0xC9A170) under `SetPlayerOverride(player 1)`.
- Research tools: `animlog on` (base-layer anim state id + vertical speed), `jumptrace <s>` (every state tick).
  The base layer (0x9A7CE19BAA54C278) did not change on jumps. A real jump/vault state is still to find, per the watchdog.

### 4.3 Pickups to "mixed"
- Live check that runtime pickup names match level-data names and classes (log line `pickup: '<name>' is a <class>`).
  Then ask the owner before switching `[sync] Pickups` from `log`.

### 4.4 Smaller items
- Shots: PC B fires; check her shots and damage (livebatch has the steps).
- Dialogue by ear; barks (`vox_hook`).
- Checkpoint-write forwarding is still log-only.
- GPU "device removed" crashes after long background pauses (10-19 11:12). Track them.
- Theta: kept across reloads, starts settled, and drops >0.6 rad outliers. If her walk stalls, check `clonetheta:` lines; `clonetheta 3.13` resets it.

## 5. Dev loop and tools

- Build `cmd /c ".\build.cmd"` (tests run). Deploy `cmd /c ".\deploy.cmd"` (hot reload on PC A; PC B pulls from share.py).
- Commands go as lines in `<game>\tlou2-coop\cmd.txt`. On PC B, over SSH: `(echo cmd)>> file`.
  - Clone and test commands: `clonebody i`, `props i`, `redress`, `catchup on|off`, `clonetheta r`, `faceforce`,
    `standturn*`, `lookanswer on|off`, `animlog on|off`, `jumptrace s`, `facediff`, `facewatch [ms] [ctl|glob]`,
    `faceptrs`, `peek addr n`, `look <id>`, `leaks`.
  - Virtual pad: `stick l|r x y ms`, `press a 150`, `trigger l|r|lr v ms`.
- Logs: `<game>\tlou2-coop.log`, rotated to `.prev1-3.log` on each reload. Key lines:
  - `clonedrive:` (want, engine world, game in front)
  - `clonedrift:` (m behind)
  - `cloneface:` (clone yaw, other body, other camera, standing or moving)
  - `clonestance:`, `clonejump:`, `jump:`
- Recording: `tools/live/rec.ps1` records only while the game is in front. Then take a frame with ffmpeg and look at it.
- IDA: `"/c/Program Files/IDA Professional 9.0/idat.exe" -A -L"re/probe.log" -S"re/<script>.py" "re/tlou-ii.i64"`
  with `re/decomp.py` (FN=…), `re/dref.py` (EA=…), `re/strxref.py` (KEYS=…).
  `python -I tools/native.py <exe> <native names>` finds natives. Some report func 0x0: then read obj+0x10 in the exe's static data.

## 6. Engine facts added today (rva = addr − 0x140000000)

- Facing: `player-force-facing/f` 0xC8CF20 → 0xDB63B0; flag 0x40FC87E, dir 0x40FC880. Reader 0xC31340, called from controller update 0xC31230.
- `player-force-camera-facing/f` 0xCAF9C0 (flag 0x40FC898).
- Quick turn: flag 0x40FC6EA (1 = on), checked in 0xC2F310. It skips the quick turn when `sub_14126CC80(0)+18740 == 2`.
- Anim state: `sub_141AFCAD0(proc)` → ctl; `sub_141440CD0(ctl, 0x9A7CE19BAA54C278)` → layer; `sub_1415FDC10(layer, out)` → state id.
- Natives: `player-force-jump-at` 0xC9A170 and `player-force-jump-forward` 0xC9A4B0 (both use GetPlayer 0xDA8A80);
  `player-is-landing-from-fall?` 0xE74BF0; `player-climbing-up?` 0xC94760; `get-player-jump-or-fall-top-pos-y` 0xE64350.
- Players: table 0x411A510 {node, id}; SpawnPlayer 0xDC1440; Process::SetUserId 0x133EC60 (hooked: 'player-2');
  Player+21800 index, +21796 joypad, +19632 NdPlayerJoypad (+0xD0/+0xD8 world stick), Player::Active::Update 0xAD61E0.
- Raw pad 0x824C860 (0x540 each): sticks +0x00/+0x08, masks +0x518/+0x520/+0x528 (A 20, B 80, LB 100, Y 10, X 40,
  LT 200, RT 2000), triggers +0x4F0/+0x4F4. Raw->slot copy sub_14158F710 (hooked, slot 3 = clone).
- Camera managers: 2 (GetGlobal sub_141B1B2B0); manager 1 has no live camera. Aim: sub_140C6E800 direction, sub_140C6F020 ray.
- Guards: level listener add/remove (0x1A4D870/0x1A4E210), ActionCommandManager push/pop (0x15B6C90/0x15B6D70).
- Looks: sub_140DBB470 queues a pending look; draw control sub_141AFD810, show sub_141484610(dc, -1, 0, meshMask),
  hide sub_14147ECD0; meshes count dc+20, entries *(dc+8) x 912 bytes, visible bits +900, name id +0x18.
  Cloth props: inventory Player+11264 (SafePropObject).
- Restarts: GameRestartCheckpoint 0x105F4E0, GameRestartEncounter 0x105F8F0, task-restart-from-continue 0x427500.
- Pickups: ProcessPickup::PickedUp::Enter 0x88FBF0; entity-exists? 0xD3BA00; kill-entity 0xD4BB40.
- Natives: get/set-object-health 0x40F7A0/0x4230A0, npc-kill 0x63D260, npc-make-aware-of 0x63DDD0,
  c-spawn-npc 0x626240, give-player-weapon 0xC8F870, camera-get-dir-through-reticle 0x407A70.
