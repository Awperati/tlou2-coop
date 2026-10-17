# TLOU2 LAN co-op mod: handoff (2026-10-17)

Repo: `C:\Users\User\TLOU2 Co-op` (GitHub Awperati/tlou2-coop, private). Last commit when written: `d9626e8`.
Game: `E:\Games\The Last of Us - Part II Remastered` (Steam, v1.0.10402.1014, Watchtower save "Capitol Hill").
Full running log of findings: [ROADMAP.md](ROADMAP.md). This file is the short version.

## 1. What works today

| Area | Status |
|---|---|
| LAN session, discovery, teleport-to-partner, overlay | Done, verified (M0–M2) |
| Partner's look on their own player (any character) | Done: `look <id>` and auto on join (`[character] AutoLook`). Lev/Dina/Ellie swapped live |
| Story companion hidden + passive on the partner's PC | Done: hidden, AI/perception off, invisible to AI, holstered |
| Host's body on the partner's PC | Done: auto-spawned NPC in the host's look (`[body] Auto=client`), steered by move-in-direction, snap turn, reused after reloads |
| Dialogue | Companion lines play from the partner's player; host lines from the host body (speaker remap at sub_1403172A0). Proven at the actor level, not by ear |
| Shots | Partner's shots replayed by their stand-in on the other PC (aim + fire, at the target when known). Verified with the sim and Ellie's rifle; real-enemy target untested |
| Barks/callouts remap | Built (`vox_hook`), **off**: never observed (needs combat) |
| Enemies (M6) | Health, death, alerts, and spawning enemies missing on one side all synced by spawner name. Verified with the sim |
| Checkpoints/pause (M7) | Restart sync both ways and remote pause (opens/closes the real pause menu) verified live. Checkpoint-write forwarding is log-only and untested |
| Client auto-rejoin after the host restarts | Done |
| M8 dedicated server | Not started |

## 2. Dev loop

- **Launcher (2026-10-17):** the mod no longer loads with the game. `tlou2-coop\coop_launcher.exe` attaches it
  (`coop_inject.dll`, same hot-reload loader). After a game relaunch, start the launcher too. `version.dll` was
  renamed to `version.disabled.dll`. Spoken prompts and sound cues are now off by default.
- Build: `cmd /c ".\build.cmd"` (also runs 71 unit tests). Deploy: `cmd /c ".\deploy.cmd"`. coop.dll hot-reloads in
  the running game; touch `tlou2-coop\coop.dll`'s LastWriteTime to force a reload.
- deploy copies offsets.ini and looks.ini every time. It copies coop.ini and roster.ini **only if missing**, so copy
  them by hand after changing them.
- Commands: write lines to `<game>\tlou2-coop\cmd.txt`; log: `<game>\tlou2-coop.log` (reset on every reload).
  Useful: `npcstate <npc>`, `native [!]<rva> args` (`sid:name`, hex floats as raw bits; `!` = script context),
  `spawnlook <spawner> <look>`, `body`, `puppet`, `look`, `companion on|off`, `speakermap a b|clear`,
  `remotedir/remotesnap/remotecatch on|off`, `bodyauto`, `pickbody`, `press <btn>`, `stick l x y ms`.
- Sim: `build\coop_lan_sim.exe walk|line|hostwalk|hostline ... [addr] [--hit n amt at] [--enemy n hp deadAt]
  [--combat from to] [--alert n at] [--spawned n at] [--restart at 0|1] [--pause from to]`.
- Game launch: `tlou-ii.exe -nolauncher`, **not as admin**, otherwise the mod's shell can't close it. Then the pad:
  `press a` (title), Story → Continue (`press a`, `press a`). The pad only works while the game window is in front.
- Recording: `ffmpeg -f gdigrab -offset_x X -offset_y Y -video_size WxH -i desktop` on the game window rect. Check
  motion with `tblend=all_mode=difference,signalstats` (YAVG ~0.5 idle, 0.00 frozen).
- IDA: `"/c/Program Files/IDA Professional 9.0/idat.exe" -A -L"re/probe.log" -S"re/<script>.py" "re/tlou-ii.i64"`.
  Scripts: xr.py (XR, DF, LIM, XR_OUT; needs XR=0x0 if no xrefs), callers.py, fname.py, decomp.py, disasm_head.py,
  callsites.py. `python -I tools/native.py <exe> <names...>` resolves script natives.
- Crash dumps: `%LOCALAPPDATA%\CrashDumps`; `python -I tools/minidump.py <dmp>`.

## 3. Rules from the user (also in memory)

- Don't stop at "reply X"; carry on yourself. Test gameplay yourself and verify visually yourself (recordings).
- Never ask the user to relaunch, dismiss dialogs, play to checkpoints or open menus. If something truly needs
  them, say it once, briefly.
- Every request to the user also goes to the watchdog session **"Godot game fundamentals scope"** (ListAgents
  first; it is often closed).
- **Banned:** focus tricks (Alt-tap), bringing the game to the front by any method, patching the game's
  unfocused-input gate, getting around permission denials. Live steps only when the user is idle (GetLastInputInfo
  over 60 s).
- If the game crashes, relaunch it yourself and return to the checkpoint. Never touch crack files or publish game
  files.

## 4. Open items / next steps

1. **M8 dedicated server**: not started.
2. Checkpoint-write test: skipped. Writing in place (sub_1410475B0(0, 5)) was refused as overwriting the user's
   save. The partner writing its own checkpoint isn't built, for the same reason.
3. Does a death go through GameRestartCheckpoint? Unknown; the hook logs it when it happens.
4. `vox_hook` (barks) untested; needs combat with the game in front.
5. GPU "device removed" crashes (07:57, 19:03, 19:58 on 10-15; AMD RX 9070 XT, driver 26.7.1). No mod frame
   on the stack.
6. Rough edges: the partner-side new enemy repeatedly reports "spotted us" (5 s throttle); a steady ~1.8 m walking
   lag on the host body; the remote pause needs the game window in front.

## 5. Key engine facts (rva = addr − 0x140000000)

- Natives (script ABI fn(ret, argc, argv, ctx), symbols = FNV-1a 64):
  - Health and death: get/set-object-health 0x40F7A0/0x4230A0, npc-kill 0x63D260, npc-alive? 0x62D110,
    kill-entity 0xD4BB40 (despawn).
  - Alerts: npc-make-aware-of 0x63DDD0 (npc, target, level 3), npc-in-combat? 0x63AC80.
  - Spawning: c-spawn-npc 0x626240, c-spawn-npc-with-look 0x626710.
  - Restart and pause: task-restart-from-continue 0x427500, pause-game-clock 0x41A060 (doesn't freeze
    characters).
  - Dialogue: request-dialog 0x319F90.
- Hooks (fiber_hook.cpp): dialog facts sub_1403172A0; vox sub_140331A80 (off); RememberCheckpoint sub_141200770 and
  GameRestartCheckpoint sub_14105F4E0. Both checkpoint hooks forward 12 args (a 4-arg version caused crash 11).
- Spawner: +0x30 name, +0x78 archetype, +0x80 flags, +0x88 Level, +96/+104 spawned handle; local locator at +0
  (MoveSpawnerTo moves the spawn spot). Spawned NPC process name = spawner name.
- Pause read: clock component 1 +0xF8 == 0, or byte [0x143603768]+99.
- Looks: config/looks.ini, from tools/looks.py over bin.psarc dc1/look2/characters.bin.
