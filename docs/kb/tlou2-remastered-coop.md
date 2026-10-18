---
kind: game
title: 'The Last of Us Part II Remastered (PC): LAN co-op with a second Player'
game: The Last of Us Part II Remastered
games_also: []
game_version: 'Steam v1.0.10402.1014 (tlou-ii.exe PE timestamp 0x67ED75F7)'
platform: windows
engine: native
route: native-hook
tools: [IDA Pro 9.0 (Hex-Rays), MSVC 2022 BuildTools + CMake/Ninja, ffmpeg gdigrab, OpenSSH]
anti_cheat: none (single-player game; mod is offline/LAN only)
status: in-progress
agents:
- Claude (claude-opus-5-5)
humans: [Awperati]
date: '2026-10-18'
links: []
tags: [co-op, lan, second-player, joypad, hot-reload, dll-injection]
---
# The Last of Us Part II Remastered (PC): LAN co-op with a second Player

A two-PC LAN co-op mod for the single-player story. Each PC runs its own copy of the story; the host is
authoritative for enemies, checkpoints and pause. The partner is shown on the other PC as a real second
`Player` (spawned through the engine's leftover Factions code, wearing the partner's character look) driven by
the partner's stick and a small allowlisted button set, instead of the story companion NPC. Verified in the
real game on two PCs from logs, position reads and recordings; the second Player walked 11.4 m in the direction
the other PC pushed.

## Setup
- Steam build v1.0.10402.1014. Launch `tlou-ii.exe -nolauncher`, not as admin.
- C++ DLL built with MSVC 2022 BuildTools; `coop_launcher.exe` injects a small loader (`coop_inject.dll`,
  LoadLibraryW via CreateRemoteThread) that loads `coop.dll` from a shadow copy and hot-reloads it when the file
  changes. A `version.dll` proxy works too (loads with the game); a named mutex keeps one loader per process.
- LAN UDP 47820 (game) / 47821 (discovery); a tiny HTTP file drop (port 8765) on the host serves the newest
  `coop.dll`, which the partner fetches and reloads by itself.

## Route and why
Native hooks: there is no loader for this engine. Engine calls are made on the game's own job fibers (any other
thread lacks the per-fiber context and faults), by hooking a function jobs call every frame (GetPlayer) and
draining a queue there. Script natives (DC `npc-*`, `player-*`) are found by their FNV-1a-64 name hash next to
the registration call and called directly with the script ABI `fn(ret, argc, argv, ctx)`.

NPC stand-ins (driving the story companion or a spawned NPC with move-to / move-in-direction) were the first
route and were dropped: companion animation set, lag, and the companion AI and level scripts kept fighting it.

## How the game works (what we had to learn)
- Players live in a table of {handle node, id} pairs at rva 0x411A510; GetPlayer = entry 0. The Factions code
  is still in the exe: `SpawnPlayer` (rva 0xDC1440) takes a player index; story mode only ever uses 0.
- Spawning index 1 asserts in `Process::SetUserId` because Player::Init's id table has only index 0; a hook that
  turns id 0 into a unique id (`player-2`) during the spawn fixes it. Script lookups by that id then reach her.
- The spawned player comes up with head and props only; re-applying the look on it
  (the call at the end of `player-request-set-look-id`, on that player) builds the full body.
- A spawned player at the world origin can fall out of the map and die; teleport her within ~150 ms.
- `CameraControlStrafe::UpdateCameraAssist` must not run for player 1 (crashes on a null camera target).
- Input: each frame `sub_141397A40` fills six joypad slots (component 1 + 0x520, 0x2570 bytes each) from a raw
  pad array (rva 0x824C860, 0x540 bytes per pad); the slot's byte at +0x14C picks the raw pad. Player 1's slot is
  slot 3, which copies raw pad 0 (the host's own pad), so writes into the slot are overwritten every frame. The
  working injection point is the raw → slot copy (`sub_14158F710`, slot index 3): hand it a patched copy of the
  raw pad. That copy is skipped while the game window is not focused (the game's own gate).
- The clone's slot stick maps to world as world = -stick (a fixed half-turn); measure it from the player's
  NdPlayerJoypad world stick (Player + 19632 + 0xD0) rather than guessing.
- Raw pad held/pressed/released masks at +0x528/+0x518/+0x520: A 0x20, B 0x80, LB 0x100, bit 63 always set.
- Item/weapon buttons on a second player push/pop a shared action-command group stack and assert
  (`ActionCommandManager::GroupStack::Pop`); only movement buttons are passed through.
- Dialogue speaker resolution goes through process user ids, so a speaker remap (`dina` → `player-2`) moves the
  companion's lines onto the clone.

## Build steps
1. `build.cmd` (VS 2022 BuildTools, CMake/Ninja; also runs the unit tests), `deploy.cmd` into the game folder.
2. Host `coop.ini` `Mode = host`, partner `Mode = client` + `HostAddress`. Start the game, then
   `tlou2-coop\coop_launcher.exe` on both PCs.
3. Clone mode is on by default (`[clone] Enabled`).

## Verification
- Mod log per PC (`tlou2-coop.log`, partner log mirrored to the host), position reads of player 1 before and
  after the partner's stick push, recordings of the game window with ffmpeg gdigrab and contact sheets.
- Not yet verified: the clone in the partner's look end to end, cinematic swap, dialogue by ear.

## Gotchas
1. **Second player asserts on spawn ("Invalid user id").** Cause: Player::Init gives index > 0 a user id of 0.
   Fix: hook `Process::SetUserId` during the spawn and replace 0 with a unique id.
2. **Second player has no body.** Fix: re-apply the look on that player after spawning.
3. **Writes into the clone's joypad slot do nothing.** Cause: the slot is refilled from a raw pad every frame.
   Fix: patch the raw pad at the raw → slot copy for that slot index.
4. **Input tests "fail" at random.** Cause: the game ignores all pads while its window is not in front. Check the
   foreground window before every live test.
5. **Partner never reloads updates.** Cause: a missing backslash wrote `tlou2-coopcoop.dll` beside the game.
   Check paths in wide-string literals with a compiler warning on (C4129).
6. **A cached download keeps restoring an old DLL.** Fix: unique query string per check, no-store on the server.
7. **Partner stuck after "Session is full".** Cause: rejoining while the host still counted the old peer. Fix:
   retry after a rejection.
8. **Companion walks on after the partner stops.** Cause: a move-to on her own spot did not cancel
   move-in-direction. Fix: `npc-stop` (only relevant to the dropped NPC route).
9. **Level scripts re-crouch the companion every frame.** Fix: hook the Npc demeanor setters (NPC route only).

## Assets
None generated; the clone wears the game's own looks (e.g. the No Return `dina-player` look), loaded from the
user's install at run time.

## Cost and time
About a week of sessions (2026-10-09 to 2026-10-18).

## Open questions
- The clone in the partner's look end to end on both PCs; package loading while the game is in the background.
- Camera for player 1 (right stick), weapons and items for the second player, scripted companion moments.
- Dedicated server (later).
