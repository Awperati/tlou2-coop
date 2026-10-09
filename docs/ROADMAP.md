# TLOU2 Co-op roadmap

Target build: `tlou-ii.exe`, PE timestamp `67ED75F7` (2025-04-03), image base `0x140000000`.
Engine: Naughty Dog in-house, native x64, D3D12. No anti-cheat. No official mod API.

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
| M0 | Loader, LAN protocol, discovery, teleport rules, roster, accessible overlay, tests | Done |
| M1 | Find the player actor and its transform; fill `[player]` | Player pointer found; position offset pending (auto-finder in DLL) |
| M2 | Call the engine's own teleport instead of raw position writes | |
| M3 | Companion puppet: take over the buddy NPC with remote state | |
| M4 | Objective + transition gate: hook world/region loads, hold until gate is ready | |
| M5 | Client plays the companion: player model/skin swap, character loadouts | |
| M6 | World sync: enemy authority, damage, pickups, cutscene triggers | |
| M7 | Save/checkpoint sync, pause and menu handling | |
| M8 | Dedicated server and 3-layer movement (prediction, reconciliation, interpolation) | Later |

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

Next for M1: walk around in game for ~30 s and read the `discover:` lines in
`tlou2-coop.log`. The top candidate is the position offset; put it in `[player] pos`.

## Server phase (after LAN works)

The core is already split from the game (`src/core`). The server reuses the
protocol and session code headless. Movement gets three layers:
1. Client prediction: the local player moves at once from local input.
2. Server reconciliation: the server's authoritative state corrects the client, replaying unacked inputs.
3. Remote interpolation: other players render ~100 ms behind, smoothed between snapshots.
