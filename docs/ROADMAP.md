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
| M3 | Companion puppet | **Verified in game** 2026-10-11 (young Ellie + Joel flashback): `GetAnyBuddy` finds Joel; `Npc::Teleport` (vtable +4520, what `npc-teleport` calls) moves him to the exact spot; `coop_lan_sim walk` sent a circling player over UDP and Joel tracked it. v1 snaps position at 10 Hz with no walk animation Walking via `npc-move-to` verified 2026-10-12 after disabling her AI (see notes) |
| M4 | Objective + transition gate | **Partly verified in game** 2026-10-11: the story task (`efm-discover-museum-space` -> `efm-flashback-capsule`) and chapter (`world-ellie-flashback-museum`, `Ellie Day 1`) are read live from `GameTaskGraphMgr` and sent as the objective; a chapter change marks a transition. LevelMgr's 482 loaded packages and their load/unload diffs are logged. Not yet: holding the engine's load until stragglers arrive, and the client-side prompt with a real second player. Chapter change seen live: `world-ellie-flashback-museum` -> `world-rescue-jesse` (Ellie Day 2) flagged CHAPTER CHANGE |
| M5 | Client plays the companion / spawned characters in solo sections | **Spawn verified in game without a crash** 2026-10-12 once heavy calls ran from the task-update phase: `c-spawn-npc` on `cdina-wat-explore-sb-8` made Dina id 12439, game stable. `npc-move-to` accepted (ret 1) and she walked over smoothly, but her buddy-follow AI may explain that; not yet proven. Earlier: teleporting a spawned Dina 8 m worked |
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

### Spawning (M5 research, 2026-10-11)
- `c-spawn-npc` (native rva `0x626240`) and `c-spawn-npc-with-look` (`0x626710`) spawn only from an
  existing EntitySpawner placed in level data. A solo section has no Dina/Joel spawner loaded.
- The dev menu "Spawn Character" (`spawn-character-devmenu.cpp`, rva `0x441CE0`) spawns by
  character name from a DC list. In retail it is dead: `IsDebugMemoryAvailable()` returned 0 and
  the list (symbol `0x83B038A657C850A5`) is not loaded (checked in game with `spawnlist`).
- Spawners are readable: `Level+256` -> `{+8 EntitySpawner**, +16 count}`; spawner `+0x30` name,
  `+0x78` art group, `+88` name id. In the Day 2 solo section: 2623 spawners in 680 packages,
  NPC ones named like `npc-res-store-infected-shambler-5` (art `shambler-01`); none for a companion.
  `load-level` (native rva `0xD4D170`) = `LoadRegistry::AddWantLoad(WantLoadInfo(1, name))`.
  Plan: in a chapter where Dina is present, `spawners dina` gives her spawner and its level;
  later, load that level in a solo section and spawn from it.
- Calling natives directly works: `fn(ret, argc, argv, ctx=null)`, symbols as FNV-1a ids.
  Checked in game: `is-level-loaded? 'wat-const` = 1, `get-spawner` returned the same address
  as the spawner dump, and `c-spawn-npc` spawned a Dina. Dina's spawners in Watchtower are
  `cdina-wat-*` in level `wat-const`, art group `dina`. The value `c-spawn-npc` returns is not a
  readable process pointer (maybe a relocated process or a handle part); still to work out.
- Leads still open: `create-spawner`, `NetEventNpcSpawn` (multiplayer NPC spawn by spawner name),
  `Process::SpawnProcess` / `ProcessSpawnInfo`, and loading a character package via the level
  want-load lists so a spawner from another chapter's level data becomes available.

### Crash 2, 2026-10-11 (puppet on a spawned Dina)
Ten seconds after the LAN puppet started teleporting the spawned Dina at 10 Hz, the game
crashed: write to `0x14` at rva `0x2169BB6`, inside `Npc::Evaluation` /
`NpcManager::NpcEvaluateUpdate` (stack rvas `0x215C70E`, `0x564539`, `0x4F781D`, `0x4AE4F4`).
No mod frames on the stack, so the game crashed later in its own NPC update. The same spawned
Dina had lived for minutes without the puppet, and survived a single teleport. Mitigation:
puppet now 4 Hz with a 1 m dead zone. Real fix: drive the NPC with a move-to command
(walk/run animation) instead of teleporting it; find the buddy move-to native next.
Dumps: `%LOCALAPPDATA%\CrashDumps	lou-ii.exe*.21364.dmp`, parser at `tools/minidump.py`.

### Crash 3, 2026-10-11 (stack overflow in c-spawn-npc)
The second `c-spawn-npc` call died with `0xC00000FD` (stack overflow) at rva `0x2866037`.
Our engine calls ran on whatever game fiber called `GetPlayer()`, which can be deep in its
own stack; spawning needs tens of KB more. Fix: every queued engine call now runs on a
private 4 MB stack (`stack_switch.asm`), with the TEB stack bounds swapped for the call.
`npc-move-to` (native rva `0x63F0E0`) is wired as `moveto <npc-name> <metres> <motion>`; untested.

### Crash 4 and 5, 2026-10-11 (spawning, stopped here)
- Crash 4: running engine calls on a private stack froze the game; the watchdog dumped it.
  The stack switch is removed. Heavy calls now wait for a `GetPlayer()` caller with at least
  480 KB of fiber stack left (fiber stacks are about 512 KB; `fiberstats` shows headroom).
- Crash 5: with that headroom, `c-spawn-npc` on `cdina-wat-explore-sb-8` hit an engine
  `__debugbreak` (rva `0x1243767`) in faction lookup ("Unable to find faction %s",
  function rva `0x14B7770`), called from NPC setup (rvas `0x5C7509`, `0x50622E`). The same call
  succeeded once earlier in the session, so some state differs between calls.
- A second dump each time came from our `std::thread` destructor at process exit
  (`std::terminate`); fixed by never destroying it.
Crash 5 explained (IDA): the faction step reads the spawner's `faction` property
(vtable +104, id `0xD75736026043571B`) and looks it up in a global hash table
(`0x14823F8C8`, function rva `0x14B7770`); the break came from that table's iterator check.
Most likely another job was changing the table while our spawn ran on an arbitrary fiber.
Scripts call spawn natives from the game-logic phase, so heavy tasks now run only from a
hook on `GameTaskGraphMgr::Update` (rva `0x1B6FB00`, found by its assert name).
Verified 2026-10-12: with the phase hook the same spawn succeeded and the game stayed up.

### Move-to experiments, 2026-10-12 (no luck yet)
- Both the story Dina and the spawned one are named `dina` (Process::GetName, vtable +40).
- With the spawner name as the NPC argument, `npc-move-to` returns 1 but the spawned Dina
  stays with Ellie; with `dina` it returns 0. `npc-set-script-mode` (rva `0x647210`, modes
  0/1/2) and `npc-stop-behavior` (rva `0x648F10`) made no visible difference, and
  `npc-get-script-mode` (rva `0x6382C0`) always returned 0. Unclear whether the NPC argument
  resolves at all; next step is to log what `sub_1403FF7D0` (the NPC arg reader) returns.
- `npcarg` (calls that reader directly) settled it: a spawner name resolves to nothing; `dina`
  resolves to the story Dina only (id 2591), never the spawned one. So the earlier "ret 1"
  orders on the spawner name did nothing.
- On the story Dina, out of combat: `npc-move-to` returns 1 and `npc-set-script-mode 1`
  runs, but she does not move and `npc-get-script-mode` stays 0. Likely cause: behaviour
  natives tie their command to the calling script (they read the current script process via
  `sub_141D919B0`/`sub_141D91FF0`); called from the mod there is none. Spawn and teleport do
  not need that. Next: give the call a script context, or drive locomotion below scripts.
- Script context attempt (option 1): hooked `NativeObject::Invoke` (rva `0x160F950`), the
  function the script VM calls every native through, and ran queued commands only when the
  SsContext stack index (`+896`, context getter rva `0x1D91F80`) was >= 0. Verified that
  inside it a real state-script group and thread are on the stack. Even there,
  `npc-set-script-mode` leaves `npc-get-script-mode` at 0, `npc-move-to` returns 1, and
  `npc-get-current-behavior` (rva `0x635FE0`) stays `0x91aaf91fc7515743` before and after.
  Dina does not move. Not solved; the remaining guess is wrong argument encoding.
- Arguments checked (2026-10-12): `npc-set-script-mode npc mode bound`. With `bound = 1`
  (ties the AI-disable to the calling script thread), run in script context, it works:
  `npc-get-script-mode` -> 1 and Dina stops following (AI disabled; `Npc::ScriptRequestDisableAi`,
  checks `ScriptSkill+132 & 3` and `Npc+17760 & 4`). `npc-set-script-mode dina 0 0` restores her.
  `npc-move-to` with motion 0/1/2 and goal 0/1 still only turns her a little. Useful now:
  with AI disabled, the teleport puppet no longer fights her follow behaviour.
- **Walking works (verified in game 2026-10-12).** The earlier targets (10-12 m ahead, indoors)
  were simply unreachable. With Dina's AI disabled (`npc-set-script-mode dina 1 1` in script
  context), `moveto dina 1 0 0` walked her from (-79.6, -204.2) to (-73.12, -200.75), 0.2 m from
  the target, and `moveto dina 4 1 0` took her to (-74.97, -198.74), 0.08 m off. The move
  command does not need a script owner (owner is only used for "wait").
  Recipe for the puppet: disable AI once (bound), then issue `npc-move-to` toward the remote
  player's position, falling back to `Npc::Teleport` when the target is off the nav mesh.
- **Walking puppet built and run in game (2026-10-12).** On partner join the buddy's AI goes off
  (bound script mode, in script context); every 300 ms she gets `npc-move-to` toward the remote
  player (walk under 4 m, run beyond), re-issued when the target moves 1 m or every second.
  Teleport when the gap is over 15 m or she has not moved 0.5 m in 2.5 s while 2+ m away.
  On partner leave or mod unload her AI comes back. `coop_lan_sim walk` (now 1.5 m/s) drove
  Dina around a 3 m circle in a cramped Watchtower room: mostly walk/run orders with some
  "stuck" teleports where furniture blocked the path. Game stayed up; AI restored on leave.

### Model swap, 2026-10-12 (crash 6)
- Look natives: `get-object-look` (rva `0xD42940`, object `+384`), `player-request-set-look-id`
  (rva `0xCA1880`, checks the look is loaded), `player-swap-look-id` (rva `0x41CB50`); both call
  `sub_140DBB470(player, look, ...)`. In Watchtower: Dina's look `0xC9E19167573AEE93`, the
  player's `0x712C65C6C3E102BF`.
- Setting Dina's look on the player returned 1, then the game crashed reading `0x18` at rva
  `0xB1B43B` in the player's breathing/animation update (rva `0xB1B290`). Player systems need
  per-character player data (breathing, facial, gestures) that only playable characters
  have: Ellie (all ages), Abby, Joel. Dina, Lev, Jesse, Yara, Tommy are never playable.
- Options: swap only the visible meshes on Ellie's player (keep her look id and systems), or
  build/borrow player data for companions. Playable looks (Abby, Joel) may swap cleanly.
- The overlay's `std::thread` also aborted at process exit (second dump); now never destroyed.

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
