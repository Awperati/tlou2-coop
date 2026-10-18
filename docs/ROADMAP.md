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
- Mesh route, step 1 (2026-10-12): `set-all-meshes-visible` (rva `0xD54D00`) `player 0` hides
  Ellie's body meshes, but her hair (cloth), backpack and weapons stay visible; `player 1`
  restores her. Plan: hide those too (`set-all-character-weapons-visible`,
  `player-set-prop-visible`, backpack/hair objects), then keep the local story companion on
  the player's spot with the walking puppet ("reverse puppet"), so the partner sees Dina while
  keeping Ellie's player systems and inventory. Animation sync (aim, crouch) comes after.
- **Reverse puppet working in game (2026-10-12).** `hideplayer` hides Ellie's body, weapons
  (`set-all-character-weapons-visible`, rva `0x41FF10`) and all five `ProcessCharCloth (Player)`
  props from her PropInventory (`Player+11264`; object getter rva `0x1E1D2D0`, DrawControl rva
  `0x1AFD810`, hide rva `0x147ECD0`). `selfpuppet on` then walks the local story Dina onto the
  player's spot; walking Ellie with the virtual pad, Dina followed and settled 0.9 m away.
  The user's screenshot shows Dina standing in front of the camera where Ellie would be.
  `selfpuppet off` / `showplayer` restore everything.
- Leftovers fixed (2026-10-12): weapons, weapon strap and the flashlight are attachables under
  ProcessMgr (EngineComponents[19]) `+512`/`+528`, walked with rva `0x1448C10`. They store no
  owner handle we could find, so the mod captures their process ids (+148) while the companion
  stands apart, then hides only those; it re-hides every second because drawing a weapon
  re-shows it. A first distance-only version also hid Dina's gear; `showall` undoes that.
- Lock: self mode teleports her onto the player's exact spot and facing every 120 ms within
  3 m (0.0-0.1 m while walking). Camera framing puts the character left of centre, as it
  does for Ellie.
- **Stance mirroring (verified in game 2026-10-12).** Companion demeanors (Npc+7776/+7784 table)
  for Dina: 0 ambient, 1 uneasy, 2 aggressive, 3 crouch, 4 crawl, 5 search. The mod samples
  `player-is-crouched?` / `-prone?` / `-aiming?` (rvas `0xC9DF90`/`0xC9E260`/`0xC9DEC0`) every
  200 ms and sets `npc-set-demeanor` (rva `0x644620`, forced) on change: crouch -> 3, prone -> 4,
  aiming -> 2, else 0. Crouching Ellie with the virtual pad made Dina crouch (demeanor 3).
  Stance and aim travel in `PlayerState` too, so the network puppet mirrors the partner.
- Torso fade: locked at exactly the player's spot, Dina's chest disappeared (the camera fades
  NPCs in its path; `camera-set-npc-fade-out-expand/f` only applies to camera-type objects, and
  `character-set-fade-out-distance` is not registered in retail). Self mode now stands her
  `selfoffset` (default 0.8 m) ahead of the hidden player along her facing; from behind she
  renders whole in game. Proper fix still open: find the per-object near-camera-fade flag.
- Correction (front view, 2026-10-12): the "missing torso" was Dina's jacket. Her four
  `ProcessCharCloth (cdina-...-sb-5)` cloth objects were hidden by the early distance-only
  attachable hide, and showing their meshes again does not bring cloth back. A checkpoint
  restart restores them. The current id-capture hide never touches her objects.
- `selfpuppet` now persists across hot reloads (`tlou2-coop\selfpuppet.on` marker); before,
  each redeploy turned it off and gave her AI back, so she wandered away.

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

### Camera fade off for the companion (verified in game 2026-10-12)
The camera fades any character standing at the camera's near point. The check is rva
`0x1CE32A0` (Character vtable +0xFF8 caller). It returns "no fade" when byte
`Character+0x9BC` is set, or while the frame counter is below `Character+0x9C0`.
The reverse puppet sets `+0x9BC = 1` on Dina every tick (`nofade on|off`, on by default),
so she now stands on the hidden player's exact spot (`selfoffset 0`, the new default).
Seen in game: Dina fully opaque at 0.0 m, facing the player's way. `ReleasePuppet` clears it.
Dead ends: `character-set-fade-out-distance` is an empty stub in retail;
`camera-set-npc-fade-out-expand` (Character+0x888) only pads the radius by 0.06-0.1 m.

### Reverse puppet: limits of NPC locomotion, and the look-swap route (2026-10-12)
Driving the companion with AI locomotion (`npc-move-to`, then `npc-move-in-direction`)
gets within ~0.3 m at a walk but swings 1.5-2 m at a run and lags on stops and turns.
User verdict: still not good enough. Writing her position (+0x130) is overwritten by the engine.
Better route: make the local *player* look like Dina. No Return has a playable Dina:
look `dina-player` (also `dina-player-patrol`, `dina-player-no-jacket`) in
`bin.psarc:dc1/look2/characters.bin`; rogue selection ids `dina`, `dina-patrol`,
`dina-winter-no-jacket`, `dina-default-no-jacket` in `dc1/ss-rogue/player-selection-definitions.bin`.
Its meshes are `actor97/dina-player-*.pak` inside `world-rogue.psarc`, which story levels do not load.
Next: find how No Return loads those actor packages and sets the player look, then do the same in story.
`tools/dsar.py` reads the archives (outer DSAR of LZ4 blocks, inner PSARC; names matched by MD5).
- 2026-10-14: the `dina-player` look needs 12 packages: `dina-player-skel`, `-head`, `-hair`, `-seattle-body`,
  `-backpack`, `-backpack-gas-can`, `-r-forearm-bandage`, `-l-bicep-bandage`, `-leg-holster-r` (only in
  `world-rogue.psarc`) and three cloth packages (in `sp-common.psarc`). All 43 archives, `world-rogue.psarc`
  included, are open from start-up (`openfiles psarc`). `player-request-set-look-id` (rva 0xCA1880) returns 0 and
  changes nothing until every package is loaded, so it is safe to poll. `load-level` (rva 0xD4D170) takes a C
  string: it builds a WantLoadInfo and calls `LoadRegistry::AddWantLoad`. Called with "dina-player-skel" it
  returns 1 and the game stays up, but the package never loads (566 packages before and after). Not yet known:
  which want list keeps a load alive, or how No Return asks for its actor packages.
- 2026-10-14 (static): `LoadRegistry::Update` (rva 0x1AF2690) walks both want lists (a1+168 and a1); entries persist
  until `RemoveWantLoad`/`ClearWantLoads`. Want type 1 = level/package (load-level, particle packs), type 2 = actor
  (`AddLevelDependentActors`, sub_141AEDDB0 -> `WantLoadInfo(info, 2, cachedName, sid)` rva 0x1AAEE20). Actor
  packages need type 2. New `loadactor <name>` builds that want on a game fiber: name via StringCache
  (rva 0x28F2010, global 0x3603798), `AddWantLoad(*0x3603788, info, true)`. Not yet run live.
- 2026-10-14 (live): `loadactor` works for actor packages in always-available archives: `dina-player-hair-cloth`
  (sp-common.psarc) loaded in under 12 s (LevelMgr 566 -> 567, is-level-loaded? = 1). `dina-player-skel` gets an
  identical want (list +0, type 2, flags 0x29, def state 6, seen with `findwant`) but never loads. Every core
  `dina-player*` mesh, and every other Dina player outfit, is only in world-rogue.psarc. So that archive's contents
  look unreachable outside No Return, even though the file is open. Next: find how a world archive is enabled for
  package lookups (file system / "Dynamic PSARC"), or the per-world gate on package paths.
- 2026-10-14: `mounts` lists GameLoading's mounted archives (count rva 0x9363E60, entries rva 0x9363E70,
  object+8 = path): 41 mounted, world-rogue.psarc included (slot 27). So the file system can reach it, and the
  block is in package resolution or loading (PackageMgr::PreparePackageForLoading, rva 0x12DC610, builds
  "actor%i/%s.pak"), not mounting. Next: hook or trace why no package load starts for dina-player-skel.
- 2026-10-14 (live): with type-2 wants, 11 of the 12 `dina-player` packages loaded, world-rogue ones included
  (head, hair, seattle-body, backpack, bandages, holster). Only `dina-player-skel` won't load. It was the one name
  given to `load-level` (type 1) earlier, ~200 times through keepload. Removing those wants (`unwant`, wants are
  refcounted) doesn't unstick it, and its level def matches a loaded one. LevelMgr likely keeps a failed entry for
  the session. player-request-set-look-id `dina-player` still returns 0. Fix: restart the game, then `look dina-player`
  (wants all 12 as actors, polls the look request each second for up to 60 s). The engine print hooks
  (`enginelog`) printed nothing during loads.
- 2026-10-14 (after a game restart): `look dina-player` loaded all 12 packages, the skeleton included, so the earlier
  type-1 calls had poisoned it for one session. The look request still returned 0. `backpack-rogue-ellie-base`
  (also in the look) was missing; it's now in the look's list. **Crash 7:** `loadactor military-pistol-pelvis` killed
  the game. That's a mesh part, not a package, so WantLoadInfo::AsyncLoadConstructor hit its "Asset not found"
  debug-break. `loadactor` now refuses names not listed in build\pc\main\actor97.txt.
- **2026-10-14: player look swapped to No Return Dina (verified in game).** Launched from the launcher (click Play),
  title screen and Continue with the virtual pad, then `look dina-player` (13 actor packages). The request was
  accepted in 3 s; get-object-look on the player returns 0x57e7ad8e62d36813 (`dina-player`). The player was invisible
  until `native !d54d00 sid:player 1` (set-all-meshes-visible). Now visible: Dina's head, hair bun, jeans, holster,
  hands, and a green backpack (`backpack-rogue-ellie-base`). Missing: jacket, torso and arms. The jacket is cloth
  (`dina-player-seattle-jacket-cloth`), and the player still owns Ellie's 3 ProcessCharCloth props (7239-7241), so the new
  look's cloth objects were probably never spawned. Next: respawn or attach the look's cloth, or test the
  `dina-player-no-jacket` / patrol looks, whose bodies may include a torso.
- **2026-10-14: new co-op model (user's design), verified in game.** Each player is a real player wearing their
  character's No Return look. `look dina-player-no-jacket` renders fully (hair, shirt, arms, jeans, backpack);
  `dina-player` (Seattle jacket) still misses its cloth jacket. `look` loads the look's actor packages, polls
  player-request-set-look-id, then calls set-all-meshes-visible on the player. `companion off` hides the story
  companion on this PC: body, nearby cloth and weapons hidden, collider off, player ignores her. AI is kept for
  story scripts, and it's re-applied every 2 s and remembered across reloads (`tlou2-coop\companion.off`).
  Note: hidden cloth may not come back with `companion on` (seen before with her jacket).
- Dialogue speaker (started 2026-10-14): `character-get-vox-name` (rva 0x626CF0, Character+2128) returns null for
  both the player and Dina, so it's not the speaker key. Leads: `request-dialog` (rva 0x319F90),
  `get-vox-process-id` (rva 0x319AD0, finds a vox process per object), `vox-override-character-id`,
  `vox-mesh-to-character-map-node`, `subtitle-remap-character/f` (rva 0xD5FF90).
- Dialogue (2026-10-14): `request-dialog` (rva 0x319F90) takes a dialog id plus up to 4 speaker names (process names
  such as "dina"). It hands them to `DialogManager::RequestDialog` (rva 0x316C50), which stores them as speaker facts;
  those pick the actor each line plays from. The new dialog hook logs every request ("dialog: <id> speakers ...") and
  remaps speakers (`speakermap dina player`, `speakermap clear`, `dialoglog on|off`). Installed and running; no
  request fired in a short scripted walk, so the remap is not yet seen working. Other request paths
  (sub_140313E40/0x140313FB0/0x140314250/0x1403159C0, conversations) also resolve speakers through sub_141B05470
  and may need the same treatment.
- **Freeze, 2026-10-14 21:2x:** after the look tests (dina-player, then dina-player-no-jacket, then back to
  ellie-seattle) the player could not move: keyboard and virtual pad both failed, camera still turned. Swapping back
  to Ellie's look, unloading the hooks (print, dialog), `companion on` and removing every Dina package want did not
  help. **Restart Checkpoint** (pause menu, driven by the virtual pad) fixed it at once. Likely cause: the look
  request leaves the player waiting on loads. The swaps asked for packages that later unloaded
  (dina-player-seattle-body, both bandages, the jacket cloth). Not proven. Print and dialog hooks are off in
  offsets.ini until it's clear they're not involved.
- Look state (static RE, 2026-10-14): player+384 is the current look, +21032 the requested base look and +21040 the
  pending look. sub_140DAB290 applies the pending look only when sub_140DB1950 says its packages are ready, then
  clears it, so a request whose packages never load stays pending forever. +21048 is a keep-state flag; +21049 is a
  lock set from a player state (controller at rva 0xB71D70). `look` now waits for 3 straight polls (about 3 s) where
  every package reports loaded and nothing is pending before it asks. `lookstate` prints the fields. Built, not run.
  The freeze itself is still unexplained.
- Live 21:32 (2026-10-14): the safe `look dina-player-no-jacket` swapped in 3 s with nothing pending before or after
  (lookstate: current = base = new look, pending 0, locked 0). **The player froze straight away again** (virtual stick
  moved nothing for 25 s). So the freeze comes from applying a No Return look itself, not a stuck pending request.
  Restart Checkpoint (virtual pad) restored Ellie (look 712c65c6c3e102bf) and movement. Next suspect: the player's
  locomotion/anim set or controllers rebuilt by the apply (sub_140D99F90, sub_140DCC670, sub_140DAC710, the anim
  controller at player+12120), versus what No Return's own player setup does.
- Static RE of the freeze (2026-10-14): the pending look is applied by `Player::ChangeLookId` (sub_140D99F90,
  player.cpp). It tears down and rebuilds the player's animation setup: the anim instance at player+0x1C0 (a1[56]) is
  deleted and recreated for the look's skeleton (player+0x1A8 = skeleton id). It also resets mesh/joint buffers
  (a1[2595], 0x80000 bytes), re-registers controllers and reinitialises the anim controller at player+12120
  (a1[1516]). `dina-player` looks use `dina-player-skel`, Ellie uses `ellie-skel`. A retarget ellie-skel ->
  dina-player-skel exists in bin.psarc dc1/skel-retargets.bin (also dina-skel, abby-skel, jesse-skel and others ->
  dina-player-skel), so animations can be retargeted. Most likely cause: the swap is designed for spawn time (No
  Return picks the character before the player spawns), and rebuilding the anim instance mid-gameplay leaves the
  locomotion state machine dead until a respawn (Restart Checkpoint). Next ideas: (1) after the swap, force a
  respawn-like reset, e.g. the dev-menu teleport event (M2) to the same spot, or a checkpoint reload with the look
  kept; (2) set the look before the player spawns (find where the spawn picks the player's look and hook it).
- **Fixed 2026-10-14 21:45 (verified in game):** after the swap, `look` sends the dev-menu teleport event (M2) to the
  player's own spot. That restarts locomotion, like a respawn. Clean test from a fresh checkpoint: Ellie ->
  `look dina-player-no-jacket` -> accepted in 2 s -> meshes shown -> reset teleport -> the virtual stick walks Dina
  about 3 m. No freeze. The spawn-time hook idea (step 2) isn't needed for now.
- (b) done 2026-10-14 21:46: with the proper swap (`look` + reset teleport), `look dina-player` shows the full Seattle
  jacket and sleeves. The missing jacket earlier was a side effect of the frozen anim state. Screenshot:
  re\dina-jacket-2026-10-14.png.
- **Crash 8 (2026-10-14 21:49):** `spawnlook cdina-wat-horse-explore-freeway-1 ellie-seattle`
  (c-spawn-npc-with-look, rva 0x626710, args spawner, 0, look) returned an Npc pointer, then the game died within
  about 5 s. It's the reverse of crash 6: a player-only look on an NPC (Dina's spawner and AI). Next: give the remote
  body an NPC-safe look. Either check how the dina-player look record differs from an NPC look (npc-normal-base vs
  player data), or use a story spawner whose character matches (e.g. an Ellie NPC spawner from a flashback level),
  or use the NPC look "ellie" (no player parts).
- NPC-safe Ellie look (static, 2026-10-14): look records in bin.psarc dc1/look2/characters.bin hold the name (+0x00),
  skeleton (+0x10/+0x18), mesh parts (+0xA0), cloth (+0xB0), dependency packages (+0xC0) and animation packages
  (+0xD0). NPC looks (`dina`, `jesse`, `joel`) list `npc-normal-base` and NPC anims (`anim-dina`, `anim-joel`...).
  **All 83 looks on ellie-skel are player looks:** player anim packages (`anim-ellie-player-*`), no `npc-normal-base`.
  So there is no NPC Ellie look, plain `ellie` included. Crash 8 most likely came from an Ellie (ellie-skel, player
  anims) look on a Dina spawner, whose NPC uses dina-skel and Dina's NPC anims. The NPC Ellie seen in Abby's
  chapters (theater fight) must come from an Ellie-character spawner in those levels (world-abby-ellie-fight /
  world-theater), not from a look. Options for the host's body on the partner's PC: (1) load or borrow an Ellie
  spawner and archetype from world-abby-ellie-fight; (2) a Dina NPC body wearing her NPC look (works today) as a
  stand-in; (3) longer term, a second player-like process.
- Ellie NPC source (static, 2026-10-14): the theater fight levels `aef-*` live in world-theater-ambush.psarc (mounted
  from start-up). `aef-script` (data in aef-script-ingame.pak) holds the Ellie NPC spawners `npc-enemy-ellie-1` and
  `npc-enemy-ellie-pistol-1`, the archetype string `ellie-simple`, and `ellie-no-headshot-npc-collision-settings`.
  NPC Ellie animations are actor packages `anim-ellie-boss` (world-abby-fights-militia.psarc), `anim-melee-ellie-npc-t2`
  and `anim-melee-ellie-abby-fight` (part-actor-anim-melee-ellie-abby-fight). `aef-const-audio` also has
  `vox-remap-abby-ellie-fight`, a built-in vox remap symbol worth studying for the dialogue work. Plan: load level
  `aef-script` with `loadpkg aef-script` (type 1 is right for a level), plus the NPC Ellie actor packages, then
  `spawnlook npc-enemy-ellie-1 ellie-seattle` (or plain c-spawn-npc). Then AI off and a friendly faction. Risks: the
  level's state scripts may start the fight logic; the spawner is an enemy.
- **NPC Ellie spawned in Watchtower (verified in game, 2026-10-14 21:59-22:03).** Relaunched the game myself
  (launcher.exe; title skip and Continue with the virtual pad). Then: `loadpkg aef-script` (theater fight level,
  type 1) plus `loadactor` anim-ellie-boss, anim-melee-ellie-npc-t2, anim-melee-ellie-abby-fight and
  `loadpkg part-actor-anim-melee-ellie-abby-fight`. LevelMgr went 566 -> 609; Ellie's NPC body parts and Jesse came
  in too. Spawners npc-enemy-ellie-1 / -pistol-1 appeared. `native 626240 sid:npc-enemy-ellie-1` (c-spawn-npc)
  spawned NPC Ellie (theater outfit). `spawned npc-enemy-ellie-1 here` placed her beside the player. AI off with
  npc-set-script-mode 1 1. Faction 3 -> 7 (Dina's) with set-object-faction (rva 0x422FB0, args: object, faction id);
  get-object-faction is rva 0x40F700. Game stable throughout. She stands in a combat stance (theater fight demeanor);
  her look is the theater outfit. Screenshot re\npc-ellie-2026-10-14.png. Next: give her the player's look
  (spawnlook on her own Ellie-archetype spawner should be safe), demeanor 0, and drive her as the remote host's body.
- 2026-10-14 22:04: `spawnlook npc-enemy-ellie-pistol-1 ellie-seattle` works with no crash, since that spawner's
  archetype is Ellie, so a player look is fine on an Ellie NPC. She wears the Seattle hoodie (re\shot.png at
  22:04). Demeanor set to 0 (npc-set-demeanor returned and reads 0). **But both NPC Ellies are frozen:** same pose in
  every frame. npc-move-to returns 1 and they don't move. Turning AI back on (script mode 0) does nothing; current
  behavior = 0. Teleports do move them. Most likely cause: they belong to level `aef-script`, which is loaded but
  not an active region of the world, so its NPCs don't update. Not confirmed. Next (static): find what gates NPC
  updates per level (level active / visible / region flags in LevelMgr or Level), then either activate aef-script or
  move the NPC to an active level (e.g. the level owning Dina's spawner).
- **Crash 9 (2026-10-14 22:09):** copying the Ellie spawner's archetype fields (+0x70 sid/+0x78 name/+0x88 def ptr)
  into Watchtower Dina spawner cdina-wat-horse-explore-freeway-3, then `spawnlook ... ellie-seattle`, returned an Npc
  and crashed within about 5 s. Archetype patching is unsafe as done; more state ties a spawner to its character.
  Don't retry without RE of how the spawner's character def is used. The user relaunched the game.
- Automation note: my Alt-tap focus trick opened the Windows task switcher while the user was using the PC, and the
  virtual-pad presses landed on the wrong menu item (No Return info page). Only drive the game when the PC is free.
- Spawn internals (static, 2026-10-14): spawner+0x88 is **not** a character def. It's a spawner group whose
  +256 list holds child EntitySpawners (group spawners recurse), which explains crash 9. spawner+0x70 is the
  process-type/archetype sid passed to the spawn info; spawner+0x90 is copied into spawn info[20] (likely the owning
  level); spawner+0x98 holds properties (a "birth" property is checked). Process::SpawnProcess (rva 0x14C9BB0) puts
  the new process under the spawn info's parent tree, or ProcessMgr+448 (default tree) when none. ProcessMgr trees:
  root +416, top-level trees +440 onwards. `ProcessMgr::RequestChangeParentNextFrame(mgr, pid, tree)` (rva 0x1447EB0)
  re-parents a process on the next frame; GetParentProcess is sub_14133D550. New commands (built, not deployed):
  `treeof <spawner|buddy>` shows the parent chain; `reparent <spawner> [mgr offset]` asks to move the NPC under that
  tree. Next live test: `treeof buddy` vs `treeof npc-enemy-ellie-pistol-1`; if they differ, reparent Ellie to
  Dina's tree and see if she animates.
- 2026-10-15 06:26 (live): `treeof buddy` (Dina) = `treeof npc-enemy-ellie-pistol-1` (NPC Ellie): both sit under
  ProcessMgr+464, so the **process tree is ruled out** as the reason she's frozen. Re-spawned Seattle-look NPC Ellie
  first appears at the theater's own coordinates (385.4, -159.7, 598.5; 935 m away), then is teleported beside the
  player. She stays frozen: move-to returns 1, position unchanged. Remaining suspects: (1) her owning level
  (spawner+0x90 -> aef-script) isn't in the active/logged-in set, and NPC updates are gated per level; (2) her nav
  and binding point at the theater's navmesh/space (not loaded here), so AI and locomotion can't run. Next (static):
  find what Npc update checks about its level or nav (e.g. `set-nav-mesh-enable-by-level` rva 0x21086F0 hints
  navmeshes are enabled per level).
- 2026-10-15 06:28-06:31: **correction:** spawner+0x88 *is* the owning Level (Dina spawner -> wat-const Level
  0x..14356E0, Ellie spawner -> aef-script Level 0x..13876A0; Level+256 is that level's spawner list). So crash 9
  re-homed a Watchtower spawner into the aef-script level. `set-nav-mesh-enable-by-level aef-script #t` returns 0
  (no navmesh enabled). Diff of the wat-const vs aef-script Level objects: same status word (+0x50 = 0x0c), only
  pointers, counts and flag byte +0x12 differ (0x29 vs 0x2b). A spawn from npc-enemy-ellie-1 with Dina's spawner
  flags (+0x80) copied in still gives a frozen NPC (walk and run orders return 1; position unchanged; behavior 0).
  Not yet tried: animating her at all (is the anim controller ticking?), or spawning an Ellie NPC from a level
  whose region is Watchtower. Also worth testing: does a Dina NPC spawned from her own Watchtower spawner, then
  teleported, walk? (On 2026-10-12 one did.) That would isolate "theater NPC" from "spawned NPC".
- Isolation test 2026-10-15 06:31 (game already in front, no focus tricks): a Dina spawned from her own Watchtower
  spawner (cdina-wat-horse-explore-freeway-1), then teleported beside the player, **moves on her own**: about 2 m in
  8 s with her AI on. So spawn-then-teleport isn't the problem; the freeze is specific to NPCs from the aef-script
  level. NPC Ellie npc-enemy-ellie-1 did move once (0.8 m along x toward a run target, which may be a teleport
  settle), then nothing more on a new walk order. Next per branch: Level+0x12 bit 0x02 (aef-script 0x2b vs wat
  0x29) and the aef spawner's +0x98 properties (e.g. a "birth"/dormant property checked in the spawn function).
- NPC level ownership (static, 2026-10-15): at the end of a successful spawn (sub_141BBFD50) the game calls
  process->vtable[+280](level), with level = the level named by spawner property 0x474CA443B8FFDEF7 if set, else
  spawner+0x88 (owning Level). Npc slot +280 = sub_141AF7190 -> sub_14133B990: unregister from the old Level
  (Process+168, sub_141E950C0) and register as a dependent process of the new one (sub_141E94B60). So every NPC is a
  dependent of one Level, and aef-script's dependents stay frozen in Watchtower. New `rehome <spawner> <level>`
  calls that same setter to move a spawned NPC to another loaded level (e.g. `rehome npc-enemy-ellie-1 wat-const`).
  Built, not deployed or run. Level+0x12 bit 0x02 not traced yet; the rehome test will show whether it matters.
- Rehome test 2026-10-15 06:36 (user idle, game in front): `rehome npc-enemy-ellie-1 wat-const` worked
  (Process+168: aef-script Level -> wat-const Level), but she still won't walk: move-to returns 1, position unchanged
  after 7 s. Script mode 0 (AI on), current behavior 0, faction 7. So level ownership is not the (only) cause. She
  had moved about 2.3 m at some point while the user was playing, so she isn't completely dead. Next ideas: (1) her
  brain is an enemy/boss archetype that only runs inside an encounter (behavior 0, no idle behavior); compare her
  AI state with the walking Dina's (npc-get-script-mode, behavior, demeanor); (2) try the script-owned move-to path
  (script mode 1 bound + move-to in script context) on the rehomed NPC; (3) Level+0x12 bit 0x02 still untraced.
- AI state comparison 2026-10-15 06:37 (user idle): story Dina: script mode 0, behavior 0x46f9879bc6b6c976 (active),
  demeanor 0. NPC Ellie (rehomed): script mode 0, **behavior 0 (none)**, demeanor 2 (aggressive). With the script
  path (npc-set-script-mode 1 1, demeanor 0, move-to in script context) her script mode reads 1, but behavior stays
  0 and she doesn't move. move-to never starts a behavior for her, while it did for Dina on 2026-10-12. Classes (from
  the assert strings their vtables use): Dina's vtable 0x2B91580 is a Buddy (Npc subclass); Ellie's 0x2BA7B90 is a
  plain Npc/Human (enemy). Her AI brain runs no behavior at all, which points at encounter/brain setup (an enemy
  NPC with no encounter, or a brain never started), not navigation. Level+0x12 bit 0x02 still untraced.
- Control test 2026-10-15 06:40 (user idle): a plain Watchtower enemy, `npc-wat-laundry-militia-4` (archetype
  militia-male-04, **same Npc vtable 0x2BA7B90 as NPC Ellie**), spawned with c-spawn-npc, AI off (script mode 1 1),
  faction 7, teleported beside the player, then move-to: it **walked to the target** (-64.99,-201.91 vs -65.00,-201.99).
  So neither the Npc class nor the spawn/teleport/move-to method is at fault. The freeze is tied to the theater Ellie
  NPC itself: her archetype "ellie" and its brain or animation set. All shared NPC locomotion packages
  (anim-npc-m, -shared, -crouch...) are loaded, plus anim-ellie-boss, anim-melee-ellie-* and anim-ellie-facial-idles.
  Leading suspect: the "ellie" NPC archetype (theater boss) has a boss/scripted anim set or brain with no normal
  locomotion, so move-to never starts a behavior. Option that avoids it: spawn the host's body from a plain
  Watchtower NPC spawner (militia or Dina) and dress it with an Ellie look. That needs a look the archetype's
  skeleton can take (Dina's NPC look already lists ellie-skel among its dependencies).
- NPC Ellie archetype (static, 2026-10-15): bin.psarc dc1/ai-archetypes.bin defines `enemy-ellie`, the theater
  boss archetype, whose strings are boss skills (`ellie-boss-sneak`, `-surprise`, `-throw-grenade`, `-uncover`). Her
  melee data is dc1/melee/npc-standing/npc-enemy-ellie*.bin, with phases 1-3. Standard archetypes carry generic skills
  (script, follow, explore, idle, stick-drive...). Consistent with the live result: her brain never starts a behavior
  for move-to. Record layout not fully decoded, so "no script skill" isn't proven. Skeleton compatibility: every
  Ellie look is on ellie-skel with player anim packages. The only NPC archetype that has worked with it is
  enemy-ellie. A Dina-archetype NPC + ellie-seattle crashed (crash 8). Plain militia archetypes use
  base-male/base-female skeletons, so an Ellie look on them is untested and risky. Options: (a) give the
  enemy-ellie NPC a standard archetype at runtime (find npc-get-archetype's field and the archetype table), keeping
  her Ellie anims; (b) find what crash 8 actually was before retrying an Ellie look on a plain archetype.
- Archetype storage (2026-10-15): Npc+15768 (0x3D98) = archetype id (npc-get-archetype-id, rva 0x635B10), Npc+15776
  (0x3DA0) = archetype record pointer (npc-get-archetype, rva 0x635A60; the default archetype is a DC symbol lookup).
  NPC Ellie's id 0x75c11b36a1b3dd23 = `enemy-ellie`. Live (user idle): copying the walking militia NPC's 16 bytes
  (id + record, militia id 0xa552351ca5ad9b3d) into NPC Ellie makes npc-get-archetype-id read militia's. But move-to
  still starts no behavior and she doesn't move. Skills and brain are evidently built from the archetype at spawn,
  so swapping the fields afterwards changes nothing. Crash-8 path (player look on a plain archetype) not decompiled
  yet.
- Crash 8 analysis (static, 2026-10-15): c-spawn-npc-with-look passes the look through a spawn callback (object
  off_142BC2510, look id at +8). sub_141BBFD50 calls it on the spawn info before Process::SpawnProcess, so the NPC is
  built with that look from the start. **Skeletons are not the problem:** dc1/skel-retargets.bin has retargets
  dina-skel -> ellie-skel and base-female-skel -> ellie-skel (325 pairs; ellie-skel retargets to/from almost every
  character skeleton). The working case (theater spawner + ellie-seattle) was a **plain Npc** (vtable 0x2BA7B90). The
  crashing case was Dina's spawner, a **Buddy** (vtable 0x2B91580); buddy logic most likely assumes Dina-specific
  data (buddy anim sets, facial/gesture rigs). The walking militia NPCs are the same plain Npc class as theater
  Ellie. **Prediction:** a plain female militia spawner in Watchtower (e.g. npc-wat-laundry-militia-3,
  militia-female-04-backpack) + `spawnlook ... ellie-seattle` should neither crash nor freeze, giving a walking
  Ellie body. Not tested.
- **Walking NPC Ellie body (verified in game, 2026-10-15 06:47):** `spawnlook npc-wat-laundry-militia-3 ellie-seattle`
  (plain female militia Watchtower spawner, plain Npc class) with no crash. Then npc-set-script-mode 1 1, faction 7,
  `spawned ... here`, and move-to: she **walked** to the target (-64.72,-202.20 vs -65.00,-201.99) and looks like
  Ellie in the Seattle hoodie and backpack (re\ellie-militia.png). Game stable. Recipe for the host's body on the
  partner's PC: a plain Watchtower NPC spawner + the host's player look. Next: wire this NPC into DrivePuppet as the
  remote body (puppetSpawner_ = that spawner) and test with coop_lan_sim walk.
- **Remote body end to end (2026-10-15 06:49, user idle):** `body npc-wat-laundry-militia-2 ellie-seattle` spawned an
  Ellie-look militia NPC (no crash), made her friendly and set her as the puppet. `coop_lan_sim walk -63 -6.8 -202
  127.0.0.1` joined as slot 1 (31 ms RTT) and sent a host circling 3 m at 1.5 m/s. DrivePuppet took her over (AI off),
  teleported her in from 775 m, then walked her toward the remote position. The data path works. Motion quality is
  poor: every ~3 s "stuck" -> teleport (her own progress < 0.5 m in 2.5 s while 2+ m away). Likely the same lesson
  as the companion: remote mode re-issues move-to every 1 s or 1 m and restarts her stride. Next: give the remote mode
  the direction steering + speed scale used for the self puppet, and relax the stuck check for a moving target.
- **Remote steering port (2026-10-15 06:57, game in background):** DrivePuppet's remote branch now uses the self
  puppet's steering: target velocity from network samples, lead point, npc-move-in-direction with a 12 degree / 2 s
  re-send, walk/run hysteresis and npc-set-movement-speed-scale; move-to with a 0.9 m overshoot when the remote stops.
  Stuck only counts while the remote is still or the gap is over 6 m. `remotedir off` restores the old move-to path.
  The sim result is not a verdict: her own speed read 0 in BOTH modes, and a plain `moveto ... 6 1` left her
  Process+0x130 unchanged for 4 s. The 3 s "stuck" teleports in the 06:49 run were the same thing: she never moved.
  SumatraPDF had the foreground, so the game is most likely paused on focus loss. Re-run with tlou-ii in front.
- **Correction + motion result (2026-10-15 07:00-07:05, game in front, user idle 3-6 min):** the game was NOT paused.
  `npc-wat-laundry-militia-2` (the `body` spawn) does not move at all: move-to (AI on or off, owned or not) returns 1
  and its Process+0x130 stays put. `npc-wat-laundry-militia-3` walks on the same move-to. Cause unknown (candidates:
  faction 7 set by `body`, or it was left off the navmesh by the 775 m teleport). On militia-3, sim walking a 3 m
  circle at 1.5 m/s:
  - `remotedir on`: her speed 1.2-1.6 m/s, gap 1.2-2.6 m, 0.5-3 m behind. Once per lap (~12 s) she stalls to
    0.1-0.3 m/s and the gap reaches 5 m. The sim circle crosses an obstacle there; a real player can't walk through it.
  - `remotedir off` (old move-to every 1 s / 1 m): her speed 0.0 for the whole run. The re-issue restarts her each time.
  - A catch-up run when 1.5 m+ behind did not raise her pace and flipped gait every few s; dropped.
- **Why the `body` spawn can't move (2026-10-15 07:07-07:14, game in front, user idle 9-12 min):** it is DEAD.
  New `npcstate <npc> [faction]` logs faction, script mode, behavior, demeanor, `npc-alive?` (rva 0x62D110) and position.
  militia-2: faction 7, behavior 0, alive 0, hovering 0.7 m above the ground. militia-3: faction 7, alive 1, walks.
  So faction 7 is not the cause. A fresh `body npc-wat-laundry-militia-1 ellie-seattle` spawned at the spawner's own
  spot (0.86, -79.07, -348.92), 225 m away, and was already dead 4 s later, before any teleport. Likely it spawns in
  an area with no loaded collision, falls, and dies. DrivePuppet now logs a WARNING when it takes over a dead body.
  Fix to try: spawn at the remote's position (c-spawn-npc-with-look's 2nd arg may be a location; or move the
  spawner's locator first).
- **Straight-line sim on militia-3** (`coop_lan_sim line x y z heading len speed`: 8 m legs, 2 s stop at each end):
  - 1.5 m/s: steady 1.2-1.8 m behind at her 1.4-1.6 m/s. Each leg starts slow (0.2-0.6 m/s for ~1 s), so the gap
    peaks at 2.4-4.2 m early in a leg, then closes during the stop. Speed scale sits at 1.6 and her walk never goes
    past ~1.6 m/s: npc-set-movement-speed-scale may have no effect on her. Not yet checked.
  - 3.5 m/s: legs are only 2.3 s, so she never settles. Gap 2-5.6 m, back to ~1 m in each stop.
- **Body spawn fixed (2026-10-15 07:17, game in front, user idle 19 min):** c-spawn-npc-with-look (rva 0x626710) =
  <spawner> <location spawner or 0> <look>. Arg 2 is not a point: it is another spawner, whose locator is used via
  sub_141BBE450 (rva 0x1BBE450). That helper copies spawner+0 (local pos vec4 + quat at +0x10) and applies the
  parent (spawner+144 parent spawner, else +136 owning Level). `MoveSpawnerTo` writes spawner+0 so the world spot
  lands where wanted. `body` now spawns 2 m beside the player. `body npc-wat-bank-militia-1 ellie-seattle` moved its
  spawner from -74.36 -7.84 -231.54 to -66.29 -6.81 -203.52 (miss 0.00 m). npcstate 3 s and 8 s later: alive 1,
  running behavior 259cefc1e29d44fd, faction 7. It follows the sim. Caveat: this spawner's own spot was only 28 m
  away, so this run alone does not prove the far spot was what killed militia-1/-2.
- **Speed scale works, but only downward for a walk** (`remotescale <s>` pins it; line sim 10 m legs at 1.5 m/s,
  upper-half mean of her speed while the remote moves): 0.6 -> 0.57 m/s, 1.0 -> 1.04, 1.6 -> 1.29 (max 1.5),
  auto -> 1.30 (max 1.6). Her walk caps near 1.5 m/s, so a 1.6 scale can't close a gap at a walk. Run gait reaches
  3.4-4.1 m/s (07:11). One leg direction stalls 8-10 m out (her speed 0.1-0.4): likely an obstacle on that path.
- **Remote follow tuning (2026-10-15 07:35-07:49, user away):** new `tools/remote_ab.ps1` (A/B a toggle over the LAN
  sim, log + clip per setting) and `tools/remote_ab.py` (summary). Line sim, 8 m legs, 2 s stops, on
  npc-wat-bank-militia-1 (alive). Gap = her distance to the remote while it moves.
  - Catch-up run (`remotecatch`, slowed run when 1.5 m+ behind): no gain. Without snap: gap mean 2.87 vs 2.92 m.
    With snap: median 2.5 vs 1.8 m, worse, and 5 gait flips. Default OFF.
  - The big lag was the 180 degree reversal: her turn-in-place took ~2 s (speed 0.2-0.4) and she fell 3-5 m behind.
    `remotesnap` (default ON): when she starts off facing 57+ degrees away, one teleport on her own spot with the new
    heading. Walk 1.5 m/s: gap median 1.9 vs 2.3 m, and the 3-5 m spikes after reversals are gone (steady 1.6-2.0 m).
    Run 3.5 m/s: gap median 3.0 vs 3.3 m, max 5.1 vs 7.0 m.
  - The steady ~1.8 m walk lag is her ~1 s start-up at a walk that caps at the remote's 1.5 m/s. Not fixed.
- **Automatic remote body (2026-10-15 07:53):** `[body] Auto = client|always|off`, `HostLook = ellie-seattle` in
  coop.ini. On the partner's PC, the host's state packets keep a live body. Every 3 s `QueueAutoBody` checks the
  current body: process present and `npc-alive?`. If it's missing or dead, it picks a new spawner. `PickBodySpawner`
  wants a human archetype (militia/wlf/scar/...) and skips infected, dogs, bosses, buddies and story characters. It
  also needs no birth error and never spawned before, and takes the nearest. Then `body` (spawn beside the player,
  faction 7). Up to 4 spawns, and bad spawners are remembered. Commands: `bodyauto <mode>`, `bodylook <look>`,
  `pickbody` (dry run). Tested with `bodyauto always` on the host with the sim: picked npc-wat-bank-militia-2
  (20 m away), spawned it at the player, took it over 0.3 s later. Alive and following after 48 s, no respawns.
- **Partner auto setup (2026-10-15 07:57, untested end to end):** roster.ini characters take `look =` (ellie ->
  ellie-seattle, dina -> dina-player). `[character] AutoLook = true`: on Welcome the partner swaps to their
  character's look. If they play the chapter's story companion, the companion is hidden (`companion.off`).
  world-watchtower now lists `companions = dina` (the buddy there is Dina). Client test: the game was switched to
  Mode=client against `coop_lan_sim host`. It joined as slot 1 (dina), logged `autolook: look 'dina-player', story
  companion off` and started the look request. Then the game died with a display-driver error ("DXGI_ERROR_DEVICE_RESET",
  AMD RX 9070 XT, driver 26.7.1). The mod had stalled 26 s before the main-thread hook came back (07:57:04 -> 07:57:30),
  so the reset may predate the look swap. Cause unknown. The auto body never ran because the game stopped ticking.
  coop.ini was put back to Mode=host and companion.off removed. Closing the crashed game was refused by the
  permission classifier, so it still needs a manual restart.
- **Look tables for every character (2026-10-15 15:20, offline; game down after the GPU reset):** `tools/looks.py`
  parses characters.bin look records. List entries point to a struct with the name pointer at +0x10. Real looks
  have a skeleton at +0x10. It writes `config/looks.ini`: 1190 records, each with skel, parts (+cloth), deps and
  anims. The look swap now takes its package list from looks.ini (skel + parts + deps, filtered to actor97.txt), and
  falls back to the built-in Dina table. Every player look checked resolves to real actor packages (13-21 each).
  Only ellie-seattle has one non-package: ellie-seattle-hair-cloth. roster.ini now gives each character a look
  (lev-player, yara-player, jesse-player, tommy-player, joel-player, manny-player, mel-player, abby-tracking). It also
  takes `look.<world pack>` overrides (lev -> lev-player-santa-barbara in world-santa-barbara). Unit-tested (57 pass).
  Not tried in game yet: only the Dina looks have been swapped live.
- **Generic look swap verified live (2026-10-15 17:44, relaunched game, Watchtower checkpoint):** the game process
  was gone by 17:35. It was relaunched with `launcher.exe` and pad presses through the menus (`press a` -> Continue).
  `look lev-player` wanted 21 packages from looks.ini. The request was accepted 4 s later, and the player became
  Lev and walked 8 m on the stick. `look ellie-seattle` (20 packages) swapped back in 2 s, and she walks.
  No freeze, no crash. A game launched this way can be closed by the mod's shell (PROCESS_TERMINATE opens); the
  instance that crashed earlier could not.
- **Partner side end to end (2026-10-15 17:49, game in client mode vs `coop_lan_sim hostline ...`):** it joined as
  slot 1 (dina). `autolook` swapped the player to dina-player (accepted 2 s later) and hid the story Dina.
  `autobody` picked npc-wat-bank-militia-sniper-1, spawned it in Ellie's look 1.5 m away, and took it over. The host
  walked 8 m legs: gap mean 1.9 m, max 2.4 m, no teleports, 5 snap turns. Clip re/clips/client_test2.mp4.
  Bug found and fixed: the host is slot 0, and DrivePuppet treated slot 0 as the self puppet (lead point, collider
  off, player-ignore). It now takes an explicit `selfMode` flag. New sim modes `hostwalk` / `hostline` (the sim is
  the host). Not cleaned up yet: each mod reload spawns a new body, and earlier ones stay in the world; there is no
  despawn native, only npc-kill. A checkpoint restart clears them. Afterwards the game was set back to host mode,
  Ellie's look and the companion shown, then the checkpoint restarted (pad: start, down x6, a, left, a).
- **Body reuse across reloads (2026-10-15 18:03):** the spawner of the current body goes to tlou2-coop\body.txt.
  After a mod reload, `QueueAutoBody` takes that body back if it is still alive, instead of spawning another.
  Live: round 1 spawned npc-wat-bank-militia-8; after the reload, round 2 logged `autobody: reusing
  'npc-wat-bank-militia-8'` and drove it again.
- **Dialogue speaker remap on every path (2026-10-15 17:58-18:03):** every RequestDialog overload ends in
  sub_1403172A0(mgr, dialog, FactDictionary*, ...). Speakers are facts keyed by the 7 slot ids at qword_142B049E0
  (fbf1ac4d96ebd022, 5a2d194b8a8ae591, ...). FactDictionary: +0 entries (24 bytes: key, BoxedValue with the u64 first),
  +16 count, +32 lock (sub_1412432F0 / sub_141243A50). The new hook (`dialog_facts_hook`, on by default) logs every
  request and rewrites speaker values through `speakermap`. The 0x316C50 hook saw none of Dina's idle lines; this
  one logs them every ~30 s: `dialog: 22542302a05c4ea5 speakers 0=dina`, then 1523207a46e188a4 and b58a3da844ede06b.
  With `speakermap dina player` they log `0=dina->player`, and the game kept running. Not proven: that the audio
  then comes from the player. There is no loopback audio device to record it. `is-speaking?` (rva 0x319BC0) returns 0
  even for normal lines. `is-vox-busy?` (rva 0x319CF0) returns 1 for both dina and player while a line plays, so it is
  probably global. Dialog ids exist only as hashes (vox-dialog/global2.bin has no names). `request-dialog`
  (rva 0x319F90) replays an id, e.g. `native !319F90 0x22542302a05c4ea5 sid:dina`.
- **Remap proven at the actor (2026-10-15 18:07):** the speaker actor comes from `sub_140313FB0(slot, facts)`. It reads
  the slot's fact (binary search; entries are sorted by key, so rewriting values in place is safe) and looks the
  name up with sub_141B05470 (process by name). The facts hook now logs that actor. Without the remap:
  `22542302a05c4ea5 speakers 0=dina, plays from 'dina' 114F4BC700`. With `speakermap dina player`:
  `b58a3da844ede06b speakers 0=dina->player, plays from 'Player' 114F1CF600`. sub_1403172A0 and the vox request
  queue resolve the speaker with this same call.
- **Host's speech from the body (2026-10-15 18:09):** a spawned NPC's process name is its spawner name, and the
  speaker lookup resolves it. With `speakermap dina npc-wat-bank-militia-8`, Dina's lines logged `plays from
  'npc-wat-bank-militia-8'`. On the partner's PC, the Welcome now sets the host speakers: "player" plus the chapter's
  protagonist ("ellie"). `MapHostSpeech` remaps them to the body's spawner whenever a body is spawned or reused. The
  remap is single-pass, so dina->player and player->body don't chain. Live, client mode vs `coop_lan_sim hostline`:
  `'player' dialogue now plays from 'npc-wat-bank-militia-8'`. Dina's idle lines logged `0=dina->player, plays from
  'Player'`. Test requests with speaker player / ellie logged `plays from 'npc-wat-bank-militia-8'`. No natural Ellie
  line fired while she stood idle, so it is shown with replayed ids only. Afterwards: host mode, Ellie's look and
  the companion restored. The militia-8 test body is still in the world.
- **Which dialogue paths the remap covers (static, 2026-10-15 18:20):** the vox request queue has three request
  kinds. Each has its own AllocRequest<>.
  1. `RequestedDialog`: every DialogManager::RequestDialog overload (speaker array, FactDictionary, speaker
     selector, `request-dialog`) ends in sub_1403172A0. **Covered** by the facts hook.
  2. Conversations: DetermineValidSpeakers (sub_140312F70) builds the ConversationHandle's speakers from the same
     request facts (ConversationHandle::AddSpeaker per speaker-slot fact). It is reached only via sub_140315200 <-
     sub_140315020 <- sub_1403172A0 / sub_140330F60. PlayNextDialog (sub_1403159C0) and `RequestedNextDialog`
     (sub_140314AF0, sub_1403177B0) continue on that handle. Later speaker lookups (sub_140327840,
     sub_14032AC40, sub_140330F60) read the request's own copy at RequestedDialog+40, which sub_140327FA0 makes
     after the hook has run. So follow-up lines in a conversation inherit the remap. **Covered**, by reasoning.
  3. `RequestedVox`: barks and callouts. An actor's own VoxController queues them in
     sub_140331A80(VoxController*, VoxParams*), reached only through vtables. The line plays on the controller's
     owner (+80 handle node, +88 id; process at node+8, id at +148), with no facts involved. **Not covered** by
     the facts hook. A new `vox_hook` caches each owner's controller by name. When `speakermap` sends that owner
     elsewhere, it queues the request on the target's controller, once the target has spoken at least once and
     the cached controller still belongs to it. Built; `vox_hook = false` in offsets.ini until tested in game.
- **Bark test, live (2026-10-15 18:17-18:20, user idle 23 min, game in the background):** with `vox_hook = true`
  and `speakermap dina player`, the hook installed and the game ran 3.5 min with no crash. Dina's idle lines went
  through the facts hook as before (`0=dina->player, plays from 'Player'`). sub_140331A80 was never called, so
  there were no barks to redirect. Barks need gameplay: combat, aiming, pickups. The virtual pad does nothing while
  the game window isn't in front: a 2.5 s stick push moved the player 0 m. `request-vox` / `vox-action-event`
  are event symbols, not natives, so no way was found to trigger a bark from a script. The vox hook is back off.
  Needs: the game in front (the user playing, or focus allowed) and some combat or aiming near Dina.

### M6 world sync: static RE (2026-10-15 18:30)
Both PCs run the same level, so every enemy has the same spawner name on both. The spawned process is found
through spawner+96/+104 (`spawnedProcessOnFiber`), and its process name is the spawner name. That name is the
sync key. Natives found (rva, args from their decompiles):
| Need | Native | rva | Notes |
|---|---|---|---|
| read health | get-object-health <obj> | 0x40F7A0 | also get-object-max-health 0x40FB50 |
| set health | set-object-health <obj> <float> | 0x4230A0 | refuses dead characters ("failed to set health of dead character") |
| kill (death anim, drops) | npc-kill <npc> | 0x63D260 | -> sub_1404ADAD0(component 16, npc) |
| dead? | npc-killed? <npc> | 0x63D300 | also npc-alive? 0x62D110, npc-is-dead-in-checkpoint? 0x63BA00 |
| damage with direction | npc-take-damage-from-position | 0x6496F0 | npc, int amount, two floats, a position, a bound frame |
| despawn anything by name | kill-entity <name> | 0xD4BB40 | Process kill ("kill-entity or terminate"), no corpse. Also fixes the leftover test bodies |
| awareness | npc-get-awareness 0x635C90, npc-make-aware-of 0x63DDD0, npc-make-aware-of-at 0x63DF70, npc-is-aware-of? 0x63B7D0, npc-in-combat? 0x63AC80 | | alert sync |
| position | get-object-position 0xD42FE0, npc-teleport 0x649B90 | | |
| pickups | player-apply-pickup <pickup> | 0xC937A0 | player (sub_140DA8A80) +12424 = pickup controller; vtable +392 applies a pickup. One hook point for every pickup |
| drops | npc-drop-pickup 0x632880, npc-disable-drop-pickup-on-death 0x631EA0 | | |
Proposed v1, cheapest first:
1. Enemies keep their own AI on both PCs. The host is authoritative for health and death only. Every ~200 ms
   the host sends {spawner name, health, dead, awareness}. The partner applies set-object-health, and on death
   npc-kill. Damage the partner deals is found by polling: the partner's local health drops below the last host
   value, so it sends the delta to the host, which lowers health there. No damage hook is needed.
2. Alerts: if an enemy becomes aware of either player on one PC, npc-make-aware-of the same player
   proxy on the other (the partner's body on the host = buddy, the host's body on the partner = spawned body).
3. Pickups: instanced loot (each player loots their own world copy), so no sync. If shared loot is wanted
   later, hook the vtable +392 apply and kill-entity the same pickup on the other PC.
4. Positions diverge with separate AI. If it looks wrong in play, puppet enemies on the partner the way the host
   body is driven (AI off, move-to). That is heavier, so only if needed.
Not done: none of these natives has been called live yet.
- **M6 enemy health/death sync, live (2026-10-15 18:24-18:36, user idle 30-40 min, game in the background):**
  - Natives first: get-object-health on a spawned militia NPC returned 100.0 (raw float bits 0x42c80000).
    set-object-health 40 then read 40. npc-kill made it dead, and its spawner handle went empty. kill-entity
    removed the leftover body npc-wat-bank-militia-8 (alive 1 -> process gone). Player health read 83.
  - Protocol: `EnemyStates` (host -> partners, unreliable, {spawner FNV id, health, flags bit0 dead}, 80 per packet)
    and `EnemyDamage` (partner -> host, reliable). Loopback unit test: 170 states in 3 packets plus one damage report
    (59 tests pass).
  - Host, every 200 ms with a partner connected: every spawned "npc-" spawner except the body, with health and
    alive. Enemies seen alive and later dead keep being sent as dead. Partner: dead -> npc-kill once; local health
    below the last host value -> report the drop as a hit; otherwise set health to the host's value, but don't heal
    back up for 700 ms after a reported hit. Host: a hit lowers health, and npc-kill at 0 or below. `[sync] Enemies`.
  - Host test (sim as the partner, `--hit`): a 30 hit logged `partner hit ... (100.0 -> 70.0)`, and the sim then
    received 70.0. An 80 hit logged `(70.0 -> -10.0, killed)`, then the enemy came through as dead, and npcstate
    showed alive 0.
  - Partner test (game as client vs `hostline --enemy npc-wat-bank-militia-7 25 10`): `1 health set` at join, then
    `1 killed` when the host flagged it dead at 10 s; npcstate alive 0. Local hit (set-object-health 100 -> 60 on
    the partner): `1 hits reported`.
  - Not covered yet: alert/awareness sync; enemies the host never spawned but the partner did (only host-spawned
    ones are sent); a client doesn't rejoin a host that restarted (needs a mod reload).
  - Afterwards: host mode, Ellie's look and companion restored, and test NPCs militia-9/10 removed with kill-entity.
    Killed test enemies militia-6/7 lie far off at their spawn spots until a checkpoint restart.
- **Alert sync + client auto-rejoin (2026-10-15 18:38-18:44, user idle ~45 min, game in the background):**
  - `npc-make-aware-of <npc> <target> <level>`: level 3 put a militia NPC in combat (npc-in-combat? 1,
    npc-get-awareness 3, demeanor 2). A 4th arg faulted inside sub_140501110 (SEH caught), so it takes 3 args.
  - EnemyState flag bit1 = in combat. EnemyDamage gains a flags byte (bit0 = alert). Partner: if the host says
    combat and it isn't here -> npc-make-aware-of the host's body. If it is in combat only here -> report an alert.
    Each is throttled to once per 5 s per enemy. Host: an alert report -> npc-make-aware-of the buddy (the
    partner's stand-in there).
  - Host test: sim partner `--alert npc-wat-bank-militia-1 3` -> `partner was spotted by ..., now hunting 'dina'`.
    The enemy went in combat (1), demeanor 2 then 5 (search). In an earlier run the alerted enemy ended up dead,
    probably shot by Dina.
  - Partner test: the spawned enemy was already in combat locally, so it sent `spotted us, alert sent to the host`.
    It also reported hits: the hidden story Dina is still fighting on the partner's PC. To fix: the hidden
    companion should not fight (AI off or passive) when the partner plays her. The host -> partner
    make-aware-of branch did not fire, because the enemy was already in combat.
  - Auto-rejoin: when the host drops (Bye or timeout), a client keeps sending Hello to the same address. Unit test:
    host.Stop then StartHost again -> client reconnects (63 tests pass). Live: `player 0 left` 18:43:44, then
    `joined as slot 1` 18:43:48 against a new sim host, with no mod reload.
  - Restored afterwards: host mode, Ellie's look, companion shown. Test NPCs removed with kill-entity.
    Dina and the player are not in combat.
- **Passive hidden companion + partner-side alert (2026-10-15 18:47-18:50):** while hidden, the companion now has
  AI off (npc-set-script-mode; script mode read 2), perception off (npc-disable-perception 0x632260 / enable
  0x633300), is invisible to AI (set-object-invisible-to-ai 0x423490, set-invisible-to-npcs 0x421FF0) and her
  weapon is holstered (npc-holster-weapon 0x63A8E0, takes at least 3 args). All of it is undone when shown
  (script mode back to 0). Live on the partner's PC, Dina stayed out of combat the whole time. With a calm
  enemy (npc-wat-bank-militia-4) and the sim host flagging it in combat from 6 s, the log showed `alerted by the
  host, now hunting 'npc-wat-gas-2'` (the host's body) at 6 s. New problem: the enemy then fell through the
  floor at its own x/z (y -7.9 -> -639 -> -2167) and died. The earlier host-side alerted enemy at almost the same
  spot didn't fall. Cause unknown. Suspects: hunting a target 30+ m away across unloaded navmesh, or a
  traversal action. Also fixed: lan_sim parsed `--combat`/`--alert` only with a dummy third value.
  Restored afterwards: host mode, Ellie, companion shown (script mode 0), test NPCs removed.
- **The fall is the spawn spot, not the sync (2026-10-15 18:51-18:52, host mode, natives only):** an enemy at
  npc-wat-bank-militia-4's spot (-76.6, -7.9, -239.3), made aware (level 3) of the body (bank-militia-2, 35 m away):
  within 1 s it stepped to x -75.85 and fell (y -8.2, -16, -34, ... -489; dead at -62). The same with the target
  "player": it stepped to -75.85 and fell (dead at -57). An enemy at bank-militia-1's spot (-74.3, -7.8, -231.5)
  hunting the body stayed at y -7.84 for 8 s (in combat, demeanor 2). It also didn't move at all, which suggests
  no navmesh or collision there: the bank interior is another streamed area. So the make-aware-of target doesn't
  matter. Test enemies must stand on loaded ground; real story enemies are spawned by the game where their area
  is loaded. Test NPCs removed afterwards.

### M7 save/checkpoint and pause: static RE (2026-10-15 19:00)
| What | Where (rva) | Notes |
|---|---|---|
| Checkpoint written | `GameSave::RememberCheckpoint` = sub_141200770(a1, a2, a3, a4) | Asserts g_checkpoint.m_gameplay/m_records/m_bodyManagerState/m_gameClocks/m_tasks/m_stats. Calls NpcManager::SaveCharacterPositionsInCheckpoint (sub_1404B2E60). Five callers (checkpoint triggers): sub_141042750, sub_141043170, sub_1410475B0, sub_14105D470, sub_1411F7EC0. Prologue mov [rsp+10h],rbx; push rbp/rsi/rdi/r12/r13/r14 = 14 bytes, hookable |
| Restart from checkpoint | `GameRestartCheckpoint(bool, bool)` = sub_14105F4E0 | The pause menu's Restart Checkpoint and the native `task-restart-from-continue` (0x427500, no args, calls it with (0, 1)) end here. Prologue mov [rsp+18h],rdi; push rbp/r14/r15; lea rbp,[rsp-47h] = 15 bytes, no RIP-relative |
| Permadeath restart | Menu2::RootGame::DoPermadeathRestartCheckpoint (sub_140FDC0D0) | |
| Auto-save to disk | `auto-save-game` 0x405020 -> sub_141B7FC60(&SaveManager 0x149341460, 0, 0); `auto-save-in-progress` 0xD31EF0; `is-waiting-for-checkpoint-commit?` 0x415E30 | |
| Dynamic checkpoint | `remember-dynamic-checkpoint` 0xD52BF0 (4 args) | Mid-encounter checkpoints (combat/search/stealth) |
| Paused? | `game-paused?` 0xD3D240: clock component 1 +0xF8 (time scale == 0) or byte qword_143603768+99; `prog-paused-last-frame?` 0xD50ED0 = byte qword_143603768+96 | Flags readable directly every frame |
| Game clock | `pause-game-clock` 0x41A060, `unpause-game-clock` 0x427B40 | Freezes gameplay time without opening a menu |
Proposed M7 v1:
1. Checkpoints follow the host. Hook RememberCheckpoint on the host and send `CheckpointWritten` (task + chapter).
   The partner, if it is far from the host, teleports to the host first (the existing teleport), then writes its
   own checkpoint. Both restart points then match.
2. Restarts follow the host. Hook GameRestartCheckpoint on the host (death or menu restart) and send
   `CheckpointRestart`. The partner calls `task-restart-from-continue`. If the partner dies, it asks the host,
   and the host restarts for both: one shared death, as in most co-op games. The partner's own GameRestartCheckpoint
   calls are let through only when the host asked for them.
3. Pause: poll `game-paused?` (raw flags) on both sides. When either player opens the pause menu, the other's
   gameplay clock is paused with pause-game-clock and an overlay "Partner paused" shows; resume when it clears.
   Backpack/crafting menus don't pause in TLOU2, so they need nothing.
4. Save files stay per player. Only the host saves story progress that matters (the partner follows the host's
   chapter anyway). The partner's autosave could be disabled later if it causes trouble.
Not done: nothing from M7 has been hooked or called live yet.
- **M7 build, log-only (2026-10-15 19:10, not deployed):** protocol messages `CheckpointWritten`
  {seq, task} (host -> partners), `CheckpointRestart` {seq, reason HostRestarted | PartnerDied} (either way) and
  `Paused` {0/1} (either way). Tests cover round-trips, an unknown restart reason being rejected, and loopback
  delivery in both directions (71 tests pass). Hooks: RememberCheckpoint (14-byte prologue) and
  GameRestartCheckpoint (15 bytes) only count and log calls. The main loop turns new counts into messages: a host
  checkpoint -> CheckpointWritten; a restart -> CheckpointRestart (HostRestarted on the host, PartnerDied on a
  partner). Pause is read directly, without a script call (clock component 1 +0xF8 == 0, or byte
  [0x143603768]+99), every 250 ms; changes go out as Paused. The receiving side only logs and shows an overlay
  note; it doesn't restart or pause yet. `checkpoint_hooks = false` in offsets.ini until the first in-game test,
  which waits for the user, since a restart throws away progress since the last checkpoint.
- **Crash 10, 2026-10-15 19:03 (graphics driver again, not the mod):** dump tlou-ii.exe.12480.dmp. Write to address 0
  at +0x19C5950, reached from ndgi::AssertDeviceRemoved (sub_1412F5210, "device removed with HRESULT") and
  sub_1419C0EE0 (DXGI_ERROR_DEVICE_RESET / DRIVER_INTERNAL_ERROR strings), inside a compute dispatch from
  BgJointWindJob. This is the same GPU device reset as 07:57 (AMD RX 9070 XT, driver 26.7.1). No mod frame on
  the stack, and the last mod log line was a routine position line 5 s before. The M7 hooks were installed 3 min
  earlier but never fired (no "checkpoint:" lines). The game was relaunched with launcher.exe and the pad
  (Play, intro, Continue) back to the Watchtower checkpoint; both checkpoint hooks installed again.
- **Crash 11, 2026-10-15 19:15 (likely the M7 hook):** dump tlou-ii.exe.14324.dmp shows a write to 0x224 at
  +0x139FA20 on a job-system worker (FreeJobArray, ConditionVariable::Wait on the stack). No coop frame in the
  scanned stack. The user was playing (the game was relaunched by them at 19:50). The RememberCheckpoint hook was
  declared with 4 args, but its callers also pass stack arguments ([rsp+20h] and up, at 0x1410427F7, 0x1410431EE
  and 0x141047663). The hook dropped them, so the save ran with garbage. Fixed: both checkpoint hooks forward 12
  integer args unchanged and only bump a counter; the log line moved to the main loop. Deployed at 19:55 into the
  running game. Not proven: no checkpoint has been written since.
- **Crash 12, 2026-10-15 19:58 (graphics driver, 3rd time today):** tlou-ii.exe.17224.dmp has the same null write at
  +0x19C5950 via ndgi::AssertDeviceRemoved as crash 10. No checkpoint had been written since the hook fix (19:55),
  so the M7 hooks didn't run. The mod's overlay is a GDI layered window (WS_EX_LAYERED|TOPMOST), not a D3D hook.
  The mod has no render hook, so nothing points at it. Device resets so far: 07:57, 19:03, 19:58, all on AMD RX 9070
  XT driver 26.7.1. The game was relaunched (launcher + pad, Continue) to the Watchtower checkpoint at 20:04.
- **M7 actions built, not run (2026-10-16 14:20):**
  - Partner: on HostRestarted it runs `task-restart-from-continue` (rva 0x427500, GameRestartCheckpoint(0, 1)).
    It skips that if it restarted on its own within 8 s, and doesn't report the restart it was asked for.
  - Host: on PartnerDied it restarts, and its restart hook then tells the partner.
  - Pause: either side's Paused freezes the other's game clock (pause-game-clock 0x41A060 /
    unpause-game-clock 0x427B40). A clock pause applied for the other side isn't reported back, so it can't bounce.
  - `[sync] Checkpoints` and `Pause` in coop.ini. lan_sim: `--restart <at> <0|1>`, `--pause <from> <to>`.
  - Live test blocked: launching launcher.exe now ends in "The operation was canceled by the user" (an elevation
    prompt). The game was last started by the user at 19:50, probably elevated. A game started that way can't
    be closed from the mod's shell either.
- **Launching without the launcher:** tlou-ii.exe takes `-nolauncher` (its command-line flag list, next to the
  string at file offset 0x2ECFD78: -nixxes, -benchmark, -nolauncher, -launcher, -d3ddebug, -resetSettings,
  -noStreamline). `-launcher` forces the launcher on. The "Show launcher" checkbox is stored as `ShowLauncher`
  (next to DisableDriverWarn). For automation: `tlou-ii.exe -nolauncher` in the game folder, then the pad to
  get through the intro and pick Continue.
- **M7 actions verified live (2026-10-16 15:51-15:53):** the user started the game with `-nolauncher`, not as admin,
  so the mod's shell may now close it. The pad took it Story -> Continue, fresh at the checkpoint.
  - Host, sim partner `--restart 6 1` (partner died): `partner died, asks for a restart` -> `restart from checkpoint
    requested` -> the hook's `checkpoint: restart (#1)` went to the partner as HostRestarted, and the sim printed
    `slot 0 restart #1 reason 0`. The reload showed as a 0.5 s pause.
  - Partner (client mode), sim host `--restart 8 0`: `host restarted` -> restart requested -> `restart #1 was asked
    for by the host, not reported back` (no loop). The restart removed the spawned body, and autobody spawned a new
    one 1 s later.
  - Pause both ways: a remote pause logged `game clock paused` / `resumed`, with no echo back. Not verified: how
    much pause-game-clock actually freezes. IsPaused (time scale) did not change, so it may stop the game clock and
    timers but not the simulation.
  - Restored: host mode, Ellie, companion shown, test bodies removed.
- **Remote pause now opens the real pause menu (2026-10-16 16:06):** with the user watching, pause-game-clock
  left Dina moving ("they kept moving"). `debug-pause` is a stub in retail, and no writer of the pause flags
  (byte [0x143603768]+96..99) turned up near its references. So a remote pause now presses Start on the virtual
  pad to open the pause menu, but only if the game isn't paused already. A remote resume presses Start again,
  only if that menu is still open. Live: `opened the pause menu for the partner` -> local game paused 0.1 s later ->
  `closed the pause menu` -> resumed. Needs the game window in front, since the pad is ignored otherwise.
  User confirmed on screen (16:10): the pause menu came up and everything stopped moving. The menu closed on resume.
- **Pause fix + self-check (2026-10-16 16:12):** a 16:10 clip showed the pause menu still open after "Partner resumed":
  Start doesn't close it, B does. Resume now presses B. Checked from a recording, not by eye: per-second
  frame-difference motion (ffmpeg tblend + signalstats YAVG) was 0.5-0.65 before, 0.00-0.04 while paused (6-13 s), and
  0.6-1.2 after resume. The contact sheet shows the menu open, then gone. The game-clock pause (16:04 clip) never
  dropped below 0.49, so it really didn't freeze anything.
- **M7 status (2026-10-16 16:35):**
  - Working, verified live: host restart -> partner restart; partner restart -> host restarts both, no echo loop;
    remote pause opens and closes the real pause menu, checked from clip motion.
  - Built, not verified: a checkpoint written on the host goes to the partner as CheckpointWritten (log only), and
    the RememberCheckpoint hook now forwards all args. Test skipped. The game window wasn't in front for pad
    driving, and writing a checkpoint in place through the game's trigger callback (sub_1410475B0(0, 5) ->
    RememberCheckpoint) was refused because it would overwrite the user's save point.
  - Open: whether a death runs GameRestartCheckpoint, so it reaches the shared-death path. Also, the partner
    writing its own checkpoint when the host does is not built; it needs a checkpoint write, the same refused action.
  - Note: the game keeps polling XInput while unfocused (GetState calls keep climbing), so the focus gate is
    inside the game's input handling, not in XInput.
- **Death -> restart path (static, 2026-10-16 16:45, inconclusive):** GameRestartCheckpoint (sub_14105F4E0) callers:
  the native task-restart-from-continue (0x427500); the pause-menu message box (Menu2::RootGame::EventHandlerMessageBox,
  sub_140FE2E70); a yes/no box handler (sub_140FFE9B0, events 0x81E6BFABC11550F7 ...), which also clears the save
  manager and autosaves; and sub_1403678F0, a data-ref'd handler that restarts and then autosaves after loading an
  inventory/state set. None is clearly the death reload. The id of task-restart-from-continue doesn't appear in
  gui2-script-funcs(-game-impl) or nd-script-funcs. So whether a death passes through the restart hook (and
  reaches the shared-death path) stays unknown until a death happens in a real session; the hook logs it either
  way. Stopped here: the focus gate and pad driving without focus are off limits.
- **M6: enemies missing on one side get spawned (2026-10-16 18:37):**
  - Partner: an enemy the host lists as alive but that has no live NPC here is spawned with c-spawn-npc
    (0x626240; the game refuses if it's already spawned), at most every 10 s per enemy.
  - Partner, every 2 s once the host's list has arrived: enemies alive here that the host never listed are
    reported once (EnemyDamage flag bit1 `kReportSpawned`). The host spawns them unless it has seen that enemy
    alive before, so a dead one is never respawned.
  - lan_sim `--spawned <spawner> <at>`.
  - Live, host: `--spawned npc-wat-bank-militia-1` -> `exists on the partner's PC, spawned it here`, alive 1 at its
    spot. Live, partner (client vs `hostline --enemy npc-wat-bank-militia-1`): `alive on the host but not here,
    spawned it`, alive 1. It then noticed the player and sent `spotted us` every 5 s (the throttle); a real host
    makes it hunt the buddy once.
  - Restored afterwards: host mode, Ellie, companion; militia-1 and the body (militia-8) removed.

### Partner actions: shots (2026-10-17)
- `npc-fire-weapon <npc> <n> <flag>` (rva 0x6346A0) fires where the NPC's gun points. `npc-fire-at-object
  <npc> <target> a b c d` (rva 0x634350) queues a shot at an object whatever way she faces (it fills a fire
  request at Npc+16336). Live on Dina: muzzle flash; a tracer to the player with no damage to the player.
- `player-is-firing?` (rva 0xB92990) and `player-get-target` (rva 0xC9C070) on the shooter's PC, polled every
  100 ms. PlayerState (protocol v2) carries a shot counter and the last target id; the other PC's stand-in
  (buddy on the host, host body on the partner) replays new shots, capped at 3 per update.
- Verified live: sim `--fire 1.5 none` made Dina aim and fire every 1.5 s on the host; Ellie firing her rifle
  (pad `trigger lr`) logged exactly one shot. A thrown bottle does not count as a shot.
- Verified in play (user, 08:33): `player-get-target` returns the aimed-at enemy's spawner name id, the same key
  as EnemyState (e.g. `npc-wat-watchtower-escape-1-4` = 1e3e277f99db46fb), 0 when nothing is in the sights.
  The counter went up 3 per shot: `player-is-firing?` stays true about 1 s, so the 300 ms repeat is now off and
  each trigger pull counts once (automatic weapons undercount).
- Not verified: the damage guard. The guard keeps an enemy's
  health from before a mirrored shot for 1.5 s, so only the shooter's own PC's hit report counts. A mirrored
  shot that kills outright can't be undone.

### Second player: the Factions code is still in the exe (2026-10-17)
Goal (user): no story companion; the partner is a real second player (full player animations and actions) on both
PCs, wearing their character's model.
- Players live in a table of `{handle node*, id}` at rva 0x411A510: `GetPlayer` (0xDA8A80) = entry 0, `sub_140DA5C30(i)`
  = entry i. Live on PC A: entry 0 = Ellie, entry 1 empty.
- `NetPlayerTracker::SpawnPlayerForTracker` (0x99F860) spawns one player per Factions tracker through
  `sub_140DB4340(tracker, ?, index, ...)`. `SpawnPlayer` (0xDC1440; out, index, art, 0, 0, spawn info) takes the index too.
  The `spawn-player` script native (0xCAA920) kills player 0 and calls it with index 0; its spawn info is the
  checkpoint's player data (global 0x35F71A0, 0x2080 per character) plus `sub_140AB04F0`'s 0x6C8 block at +64.
- `spawnplayer 1 ellie-seattle` (new command, same steps with index 1) ran `Player::Init` for player 1, which then
  asserted in `Process::SetUserId` (0x133EC60, "Invalid user id"): Init's id table only has index 0 ('player'),
  so index 1 got id 0. Crash dump 18:06. Fix built: a SetUserId hook turns id 0 into `player-2` during the spawn.
- 18:11 live, with the hook: `spawnplayer 1 ellie-seattle` returned and the game kept running. Table entry 1 now
  holds a real `Player` (same vtable as Ellie, id 7206), spawned at the world origin. `playertp 1` (engine teleport
  event, like our own teleport) put it 1.4 m beside Ellie on the ground.
- Input: the game polls only XInput pad 0 (pads seen mask 0x1, GetCapabilities never called), so a virtual pad 1
  (`p2 ...` lines, built) is never read. Player 1 needs its input from somewhere else: Player::CreateControllers
  (0xD9E000) and the Factions remote-player path (AnimNetPlayerInfo, NetPlayerTracker) are the leads.
- Player 1 came up with only head, hair, holster, rifle and backpack (user, screenshot 18:17). Showing its meshes
  did nothing; re-applying the look on it (`playerlook 1 ellie-seattle`: sub_140DBB470(player, look, 0, 1, 1), the
  end of player-request-set-look-id) built the full body (screenshot 18:20). Order: spawnplayer, playertp, playerlook.
- Next: find what Player reads its joypad from and point player 1's at our own state; or drive player 1 the way
  Factions drives remote players (network anim state).
- Input leads (static, 18:25): Player::CreateControllers (0xD9E000) makes the joypad controller at Player+12264 with
  `sub_140CB6F70(player index at Player+21800)`, so player 1 already asks for joypad 1. XInput is loaded at run time
  (sub_141A07980 fills XInputGetState/SetState/GetCapabilities pointers at 0x149174D60/68/70). The XInput device class
  has a probe method (sub_141A07900: GetState(index) -> connected flag at device+185, index at +328/+348) and a read
  method (sub_141A07A80). Only a device for pad 0 polls today. Next: find where devices are created per index (or
  rescanned), so a virtual pad 1 becomes joypad 1; failing that, hook player 1's joypad read and fill it from the
  partner's input.
- 18:35 live: Player+21796 is the player's joypad (net-get-joypad-from-player; engine slot = 2 + (value != 0)) and
  Player+21800 its index; player 0 = 0/0, the spawned player = 1/1. The joypad controller at Player+12264 keeps its
  own copy (controller+304 -> +428). `playerjoy 1 0` set both to 0, but player 1 still did not move when the pad
  moved player 0 (position unchanged, recorded). Its animation does tick (head turns between screenshots).
  So something else gates input: most likely the local player index (NdGameInfo GetPlayerIndex, which also picks
  the CameraManager in xmmword_1492AE950[index], kMaxCameraManagers) or a per-player update that only runs for
  player 0. Next: find who calls the joypad controller's update for each player, and whether player 1 needs a
  camera manager. Alternative: drive player 1 as Factions drives remote players (NetPlayerTracker / AnimNetPlayerInfo).
- 18:37 live: with player 0's joypad set to 1, the pad still moved player 0, so the joypad field is not what feeds
  movement. Player::Active::Update (0xAD61E0) calls CameraManager::GetGlobal(player index) (0x1B1B2B0), which asserts
  unless a CameraManager exists for that index (table xmmword_1492AE950, kMaxCameraManagers = 2). CameraManager::Init
  (0x1B1C840) spawns both at start-up, and live both exist (ids 0x17 and 0x18), so that is not the gate.
  Next: check whether player 1's Active::Update runs at all (hook it, count per player), then follow how the stick
  reaches Player+19632 (get-joypad-stick-ws reads it) for player 0, and fill the same for player 1.
- 2026-10-18 06:44: `activecount` (hook on Player::Active::Update) counted 282 calls for each player in the same
  window, so player 1 is updated every frame. Player+19632 is its NdPlayerJoypad: +0x14/+0x18 = 0/1 and +0x1C = engine
  joypad slot 2 (player 0) / 3 (player 1). Next test: poke player 1's +0x14/+0x18 = 0 and +0x1C = 2 (done 06:45), then
  move player 0 and see whether player 1 follows the same stick.
- Companion idle: tried demeanor 1 (uneasy, the cautious idle) for a standing partner where the level is tense.
  Recorded 06:49: she flicked between crouch and stand about once a second. Back to 0 by default (`standdem` to try
  others). The cautious idle needs another route (likely an idle/animation-set override, not the demeanor).
- 07:38: joypad slots live in engine component 1 at +0x520, 0x2570 bytes each (sub_141D95E60); slot 2 = player 0's
  pad, slot 3 = player 1's; NdPlayerJoypad (Player+19632) +0xD0/+0xE0 = left/right stick in world space.
  `clone <look>` spawns, teleports and dresses player 1; `padmirror on` copies slot 2 into slot 3 every tick.
  With the mirror on, the game crashed 13 s later in CameraControlStrafe::UpdateCameraAssist (0x8215E0) run for
  player 1 (null+0x80). Now hooked: it runs only for player index 0. Not retested yet.
- 09:45-09:56 (clone drive): PC B's world stick (its NdPlayerJoypad +0xD0/+0xD8) reaches PC A in PlayerState. Writing
  it into slot 3's stick fields, into a copy of slot 2 with the stick replaced, or straight into player 1's
  NdPlayerJoypad +0xD0 before its Active::Update did not move her: the engine rebuilds the slot from its device
  input history each frame. Only a full copy of a live slot (padmirror) moved her.
  Input manager = *(*(rva 0x915A9B8) + 0xB8) (sub_1419D2010, 0x8D0 bytes): devices at +0x590 (20: 0-7 WGI,
  8-11 XInput, device +0xB9 connected, +0x148 pad index), local player count at +0x828 (1), per-player entries
  at +0x690, 20 bytes each ([4] = device, player 0 -> 8 = XInput 0). Setting count 2, entry 1 -> device 9, device 9
  connected, and pushing virtual pad 2 did not move player 1 either (reverted). Next: find who fills joypad slot 3
  (callers that write slot +0x18C8 history) and which manager field maps a player entry to a slot.
- 10:19-10:25, slot mapping (static + read-only peeks): the slot filler is sub_141397A40, run every frame. Component 1
  (the object behind sub_14126CC80(1)) holds 6 joypad slots at +0x520, 0x2570 each. Slot i copies raw pad entry
  *(u8*)(slot+0x14C) from the raw pad array at rva 0x824C860 (0x540 bytes per entry, 2 entries) through component
  1's vfunc +32 = sub_140399320, which ends in sub_14158F710(slot, raw, slot index), the raw -> slot copy. Slot
  type at +0x158: slots 0/1 = 3, 2/3 = 1 (gameplay, player 0 / player 1), 4/5 = 5. Live sources: 0,1,0,0,0,1, so
  slot 3 (player 1, NdPlayerJoypad +0x1C = 3) copies raw pad 0, your own pad, every frame. That is why writes
  into slot 3 vanished and why the full mirror moved her. Raw pad layout: floats, stick at +0x00/+0x04 with a
  copy at +0x10/+0x14, button masks at +0x518 (pressed), +0x520 (released), +0x528 (held).
  Tried: pointing slot 3 at raw pad 1 and writing raw pad 1 (overwritten before the copy); hooking
  sub_14158F710 for slot index 3 with a patched copy of the raw pad (hook hit about 60 times a second, but slot 3
  still read 0 and player 1's world stick stayed 0). Next: see what sub_141590C30 / sub_141590600 /
  sub_141590D60 do after the copy (likely rebuild sticks from the history ring around +0x18C8), and patch the
  values they read, or hook after them.
- 10:30, post-copy static read: sub_141590C30 is action-command bookkeeping (no sticks). The raw -> slot copy
  sub_14158F710 itself is gated: if slot+0x2408 != 0, or byte_144259DAB == 0, it neutralises the slot; and if
  byte_144259DAC == 0 and byte_148DFBCE3 == 0 it swaps buffers and resets it. Otherwise it copies the sticks
  (slot+0x00 = raw+0x00 * scale, +0x04, +0x08, +0x0C, ...) and builds the rest. Live with PC A's game in the
  background: 0x4259DAB = 1, 0x4259DAC = 0, so every slot is reset, player 0's included. The 10:21-10:25
  clone tests ran with PC A unfocused (Firefox in front), so they could not show anything.
  Decision: keep the hook on sub_14158F710 (patching the raw pad it copies for slot 3). It must be tested with
  PC A's game already in front. Patching the focus gate is banned by the user, so no workaround.
- **10:40: the clone walks from PC B's stick.** With PC A's game in front, `clonedrive on` and PC B's virtual
  stick held for 6 s, player 1 walked 11.4 m (-68.69,-196.43 -> -58.76,-202.04), direction (0.87,-0.49) against
  PC B's wanted world direction (0.91,-0.42); her engine world stick matched PC B's exactly. The working path:
  PC B sends its NdPlayerJoypad world stick in PlayerState; PC A converts it to a slot stick (world = -stick for
  the clone, theta = pi, re-measured only on a steady stick) and the raw-copy hook (sub_14158F710, slot index 3)
  swaps it in. Needs PC A's game in focus (the game's own input gate; not patched).
  Next: dress the clone in the partner's look (dina-player packages on PC A), keep her position in sync
  (correct drift with a teleport when far off), add a safe button set (sprint, crouch, jump), hide the story
  companion except where the plot needs her, and the same clone of the host on PC B.
- 10:45-10:55: clone steps built (commit c9309a2+). (1) `clone <look>` now wants the look's actor packages,
  spawns her as Ellie, then applies the wanted look once they are loaded (90 s limit) and shows her meshes.
  (2) Position sync: she snaps to the partner's reported spot when more than 4 m off, or 1.5 m once the partner
  has stood still 1 s (at most every 2 s). (3) Buttons: PlayerState carries the sender's raw pad 0 held mask
  (+0x528); the raw-copy hook gives the clone only allowlisted bits ([clone] Buttons, default 1A0) with
  pressed/released edges. Bits measured on PC B with `padbits on`: A 0x20, B 0x80, LB 0x100, bit 63 always set
  (kept from the base pad). Live clone tests of (1)-(3) wait for PC A's game to be in front.
- 11:00 clone mode built (off by default, `[clone] Enabled = true` in tlou2-coop\coop.ini to try): once both PCs
  are linked and a chapter is loaded, each PC spawns the other player as player 1. The host uses the partner's
  character look from the roster (dina-player for Dina); the partner uses [body] HostLook. Clone drive goes on,
  and the NPC stand-ins (host body, puppeted companion) stay off. On the host the story companion is hidden,
  except while a cinematic plays: then she is shown and the clone is hidden (and the other way round after).
  She is respawned if a level load removes player 1. Not tested live yet. Open: the companion's dialogue still
  plays from the hidden story companion on the host (remap to the clone not done); the clone's process name for
  the speaker map is unknown.
- 11:05 dialogue in clone mode: the speaker lookup (sub_141B05470) resolves process user ids; the clone's is
  'player-2' (SetUserId fix at spawn). Live check: get-object-health on sid:player-2 read 100 (the clone),
  sid:player 83 (the host). Clone mode now maps the partner's character (host side) or the host's speakers
  (partner side: player, ellie) to 'player-2', and back to themselves while a cinematic plays. Not heard yet.

### Clone mode live test checklist (run when PC A's game is already in front; never force focus)
Setup: both PCs on the same build (check the `build ...` stamp in both logs), PC B joined, clone mode on
(log `clone: player 1 ready as 'dina-player'` on PC A, `'ellie-seattle'` on PC B). PC B commands go to its
`tlou2-coop\cmd.txt` over SSH (`ssh -i ~/.ssh/tlou2_pcb sharn@192.168.0.238`); PC B's game must be in front too.
The queued script `scratchpad/clonetest.sh` does steps 1-3 by itself.

| # | Check | How | Pass |
|---|---|---|---|
| 1 | Look | screenshot of PC A | the clone wears Dina's model with a full body |
| 2 | Movement | PC B `stick l 0 32767 3000`; read player 1's pos before/after (`players`, `proc <addr>`) | she walks 3+ m the way PC B walked; `clonedrive: want == engine world` |
| 3 | Crouch | PC B `press b 300`, then `stick l 32767 0 2500` | she crouches (recording) and moves right |
| 4 | Sprint / jump | PC B `press lb 2000` with the stick held; `press a 200` | she runs / jumps; no `ActionCommandManager` assert |
| 5 | Snap-back | push her 5+ m off (or let PC B walk into a wall) | `clonedrive: drift ... snapped` and she lands on PC B's spot |
| 6 | Plot swap | play to a cutscene or a Dina scripted moment (boost, door) | `clone mode: ... story companion shown, clone hidden`, then `gameplay, clone shown` and `player 1 moved onto the companion's spot` |
| 7 | Dialogue | listen to a Dina line in normal play | it comes from the clone's position (`speaker(s) now play from the clone`) |
| 8 | PC B's clone of the host | move PC A's own player (needs PC A in front) | on PC B player 1 (Ellie look) walks the same way; PC B log `clonedrive: want ...` |
| 9 | Stability | play 10 minutes including a checkpoint load | no crash; the clone is respawned after the load (5 s after player 0) |

#### Clone test run 12:06 (PC A in front)
- Movement: PC B's 3 s stick push moved the clone ~4.9 m on PC A; `clonedrive: want == engine world` (0.16, 0.99).
- Crouch: the B press toggled from different starting stances, so the clone crawled on PC A while PC B crouched.
  Fixed with stance sync (`[clone] StanceSync`, on): B is pressed for the clone until her stance matches the
  partner's (tap = stand/crouch, 1 s hold = prone). PC B logged `clonestance: clone 1, other player 0` and fixed it.
- Crash 12:06:32 (PC A): the prone clone's state camera pick (`sub_140C5A3B0`) read a null camera controller at
  state+224 (player 1 has none). Guarded: no controller → camera id 0, which the function returns itself in another case.
- The player 1 process moved in memory during the run (proc FCF2180 → FCF0800, same node): never cache her proc.

#### Freeze/crash 12:11-12:43: second player links a level listener twice (fixed)
- Live dump of the hung game: one thread spun in `LevelMgr::OnLoadLevel` (sub_141A4D2A0) walking the level-loaded
  listener list; the static node 0x1437FF478 pointed at itself. A singleton process links that node in its Init
  (sub_141E59620); spawning player 1 ran the Init again and linked it twice. The next level/package load spun
  forever, and the game later died with "device removed" (12:32 dump, `ndgi::AssertDeviceRemoved`).
- Guards (fiber_hook.cpp): the link (sub_141A4D870) skips a node already in the list; the unlink (sub_141A4E210)
  skips a node that is not linked. Live 12:57: `level listener: node ...f478 already linked` at the clone spawn,
  then the dina-player packages loaded and the clone came up as 'dina-player' with no freeze.
- Tools: tools/livedump.ps1 (minidump of a running process), tools/dumpthreads.py (every thread's stack).
