# TLOU2 Co-op roadmap

Target build: `tlou-ii.exe`, PE timestamp `67ED75F7` (2025-04-03), image base `0x140000000`.
Engine: Naughty Dog in-house, native x64, D3D12. No anti-cheat. No official mod API.

## Dev loop (no game restarts)

`version.dll` is now only a loader. It runs a shadow copy of `tlou2-coop\coop.dll`
and hot-reloads it whenever that file changes. `deploy.cmd` copies a fresh build in.
Writing `teleport` to `tlou2-coop\cmd.txt` runs the test teleport without touching the game window.

## Synthetic input (verified in game 2026-10-09)

The game ignores injected keyboard/mouse: it reads raw input and drops events with
no device. It does poll XInput. `input_inject.cpp` patches `XInputGetState` and
`XInputGetCapabilities` in `xinput1_4.dll` and presents a virtual pad 0, connected from
start-up. Commands in `cmd.txt`, one per line:

| Command | Effect |
|---|---|
| `press down 200` | Button for 200 ms (`a b x y start back lb rb ls rs up down left right`, combine with `+`) |
| `stick l 0 32767 1500` | Left stick full forward for 1.5 s |
| `trigger r 255 300` | Right trigger |
| `wait 500` | Pause the script |
| `inputstats`, `modules` | Diagnostics in the log |

Verified: D-pad moved the pause-menu selection, Start unpaused, and the left stick
walked the player about 4 m. The game seems to pause itself when it loses focus.

## How the mod hooks in

The game imports `VERSION.dll`. Our `version.dll` sits next to the exe, forwards
all 17 real exports to `System32\version.dll`, and starts a co-op thread.
Everything that reads game memory lives in [game_bridge.cpp](../src/mod/game_bridge.cpp)
and is driven by [offsets.ini](../config/offsets.ini), so RE results drop in without a rebuild.

## Co-op model

Each PC runs its own copy of the story. The host is authoritative.

| Thing | Host PC | Client PC |
|---|---|---|
| Local player | Protagonist (Ellie/Abby) | Companion (Dina, Lev, ...) |
| Companion NPC slot | Puppet driven by the client's state | Puppet driven by the host's state |
| Enemies, story triggers | Simulated, sent as world events | Mirrored from host |
| Scene transitions | Held by the gate until all arrive or teleport | Follows host |

## Milestones

| # | Milestone | Status |
|---|---|---|
| M0 | Loader, LAN protocol, discovery, teleport rules, roster, accessible overlay | Unit-tested; loader + LAN join **verified in game** |
| M1 | Player actor and transform | **Verified in game** 2026-10-09: pos `+0x130` tracks walking, yaw from quat `+0x140` turns with the player |
| M2 | Engine teleport (dev-menu `teleport` event) run on a game job fiber | **Verified in game** 2026-10-09: moved the player to the exact target x/z, ground snapped y, no crash |
| M3 | Companion puppet | **Verified in game** 2026-10-11 (young Ellie + Joel flashback): `GetAnyBuddy` finds Joel; `Npc::Teleport` (vtable +4520, what `npc-teleport` calls) moves him to the exact spot; `coop_lan_sim walk` sent a circling player over UDP and Joel tracked it. v1 snaps position at 10 Hz with no walk animation |
| M4 | Objective + transition gate | **Partly verified in game** 2026-10-11: the story task (`efm-discover-museum-space` -> `efm-flashback-capsule`) and chapter (`world-ellie-flashback-museum`, `Ellie Day 1`) are read live from `GameTaskGraphMgr` and sent as the objective; a chapter change marks a transition. LevelMgr's 482 loaded packages and their load/unload diffs are logged. Not yet: holding the engine's load until stragglers arrive, and the client-side prompt with a real second player. Chapter change seen live: `world-ellie-flashback-museum` -> `world-rescue-jesse` (Ellie Day 2) flagged CHAPTER CHANGE |
| M5 | Client plays the companion: model swap, loadouts | Not started |
| M6 | World sync: enemies, damage, pickups, cutscenes | Not started |
| M7 | Save/checkpoint sync, pause and menus | Not started |
| M8 | Dedicated server and 3-layer movement | Later |

## RE leads (from strings in the retail exe)

The exe still carries dev-menu and assert strings. They point straight at the systems we need.

- `Teleport Player`, `Teleport To`, `Teleport To Cover`: dev-menu teleport. Its handler gives the player object and the engine teleport call (M1, M2).
- `Npc *__cdecl GetAnyBuddy(void)`: the companion lookup (M3).
- `BuddyCombat::Reposition ...`, `BuddyCombatSkill::GetRunSpeed`: buddy AI movement, where the puppet takes over (M3).
- `world-game-start`, `world-cutting-room`, ... : world pack names next to the loader code (M4).
- `SchemaRespawner::SpawnNpc`: NPC spawning, for extra co-op bodies beyond the story buddy (M5).

Workflow: open `re/tlou-ii.i64` in IDA 9.0, xref the string, name the function,
make a byte pattern, put it in `offsets.ini`, and check `tlou2-coop.log`.

## Verified in game (2026-10-09)

- `version.dll` loads in `tlou-ii.exe`, opens UDP 47820/47821, and the bridge initialises without crashing.
- `coop_lan_sim client` found the in-game host by LAN broadcast and joined (slot 1, 31 ms RTT).
- The game process cannot be killed from a normal shell ("Access is denied"). Close it from the game menu.

## Engine facts found (rva = address - 0x140000000)

| What | Where | Notes |
|---|---|---|
| `GetPlayer()` | rva `0xDA8A80` | Reads player handle global `0x14411A510` = {ptr, id}; Process* = ptr+8, valid if `Process+0x94 == id` |
| Dev menu "Teleport Player" | rva `0x3F86F0` | Builds a locator in front of camera 0, then `Event::Send` to the player |
| `Event::Send(Process*, ...)` | rva `0x156F870` | Event id `0xFC5BA7D4` used for teleport; calls Process vtable +240 |
| `GetRenderCamera(i)` | rva `0x138CA00` | `0x148243BE0 + 976 * i` |
| Player `teleport` event | id `0xB805933EFC5BA6D4` | Handled at rva `0xB766AD`; saves `Process+0x130` (pos) and `+0x140` (quat) first |
| Event arg | 16 bytes `{u64 value; u32 pad; u32 type}` | BoundFrame arg is type 18 |
| `BoundFrame(out, locator, binding)` | rva `0x133A1B0` | 80 bytes: 48-byte binding, then locator `{pos xyzw, quat xyzw}` |
| Default binding | rva `0x133A130` | |
| StringId64 | FNV-1a 64 | Offset `0xcbf29ce484222325`, prime `0x100000001b3`. `teleport-player` = `0xB805933EFC5BA6D4` |
| `Npc::Teleport(BoundFrame*, 1, 1, 1)` | vtable +4520 | Called by natives `npc-teleport` (rva `0x649B90`) and `npc-teleport-to` (rva `0x649DE0`) |
| Script native table | rva `0xB9080`.. | Each native: static object initialised with its function, registered by name hash |

Processes move in memory (the engine relocates its process heap): Joel's `Npc*` changed
between calls while his id stayed 29729. Never cache a process pointer; look it up each time.

### Threading rule (found the hard way)
Engine code reads per-fiber job context from `TEB->FiberData`. On a plain thread that is
`0x1E00`, and `Event::Send` faults at rva `0x123166F` reading it. The window thread is not
a game fiber either. We now patch `GetPlayer()` (rva `0xDA8A80`) with an exact
re-implementation that drains our queue when it is called from a fiber. The patch is
undone before every hot reload.

### Level and task facts
| What | Where | Notes |
|---|---|---|
| EngineComponents table | getter rva `0x126CC80` | `table[6]` = LevelMgr |
| LevelMgr | +424 count, +432 `Level**` | `Level+80` name id, `Level+0x48` `char*` name. Includes actor/anim packages, not only maps |
| GameTaskGraphMgr | global, `lea` at rva `0x1AF0995` | List at +224 of `{next, prev, task}`; `task+0` = task name, `task+288` -> chapter `{+0 name, +0x10 world pack, +0x40 day}` |

### Crash, 2026-10-11
Driving the puppet with a test origin of `0,0,0` jumped Joel about 90 m while a cinematic
played, and the game crashed (dump `tlou-ii.exe(1).3596.dmp` in `%LOCALAPPDATA%\CrashDumps`).
The puppet now only moves the buddy within `puppet_max_distance` (40 m) of the local player.
Cinematic flag found and checked in game: CinematicMgr (global read by the getter that
asserts `s_pCinematicMgr`, rva `0x1B2C300`) holds the playing cinematic's process handle at
`+0xA8`. During the `efm-swim-flashback-end` cutscene it was live; the flags at `+1432`/`+1384`
and the IGC query stayed 0. The handle drops for ~0.5 s between shots, so the mod holds
"playing" for 1.5 s. Puppet moves and the teleport prompt are suppressed while it plays.
Confirmed twice in game: the swim flashback ending, and the Ellie Day 2 opening (user saw the
cinematic while the mod reported `playing`).

### Risks
- Every address comes from byte patterns. A game patch can break them; the log then says `pattern not found` and that feature switches off rather than crashing.
- Engine calls run inside `__try`, from inside a game job that called `GetPlayer()`. That job may be mid-update; so far no crash.
- The quaternion yaw convention is assumed (Y up). If F10 jumps sideways, the axis order is wrong.

In-game check for M1 and M2 (needs a person at the PC):
1. Quit the game, copy `buildersion.dll` and `config\*.ini` into place.
2. Load a save. Walk. `tlou2-coop.log` should print a changing `player pos` every 5 s.
3. Press F10. The log says `engine teleport ... sent` and you move 3 m ahead.
If `player pos` stays 0 or constant, set `discover = true` in `[player]` and walk for 30 s.

## Server phase (after LAN works)

The core is already split from the game (`src/core`). The server reuses the
protocol and session code headless. Movement gets three layers:
1. Client prediction: the local player moves at once from local input.
2. Server reconciliation: the server's authoritative state corrects the client, replaying unacked inputs.
3. Remote interpolation: other players render ~100 ms behind, smoothed between snapshots.
