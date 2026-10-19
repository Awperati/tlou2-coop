# TLOU2 LAN co-op mod: handoff (2026-10-19)

Repo: `C:\Users\User\TLOU2 Co-op` (GitHub Awperati/tlou2-coop, private). Written at commit `88ca57f`.
Game: `E:\Games\The Last of Us - Part II Remastered` (Steam, v1.0.10402.1014, Watchtower save "Capitol Hill").
PC A = host (192.168.0.237), PC B = partner (192.168.0.238, SSH `ssh -i ~/.ssh/tlou2_pcb sharn@192.168.0.238`).
Full log of findings: [ROADMAP.md](ROADMAP.md). Player guide: [PLAYING.md](PLAYING.md). This file is the short version.

## 1. What works today

| Area | Status |
|---|---|
| LAN session, discovery, teleport-to-partner, overlay, auto-rejoin | Done, verified |
| **Clone mode** (default) | The partner is a real second Player (player 1) on each PC, in their look (PC A shows 'dina-player', PC B shows the host as Ellie). Story companion hidden; shown only for plot moments |
| Clone movement | Partner's world stick -> her joypad slot 3 at the raw-pad copy. Stick turn theta learned from her real motion (`ThetaCalibrator`, core/clonedrive.h, kept across reloads). Catch-up pull + sprint, drift snap at 4 m (1.5 m standing). User: "moves in the right directions" |
| Clone facing | `FaceSync`: rotation written right after her own Player::Active::Update each frame. User: "faces the right way" |
| Clone stance | Stance sync presses crouch for her until she matches (not the toggle button) |
| Clone buttons | A B LB Y X (mask 1F0). Player 1's ActionCommandManager group push/pop skipped (was the Y crash). Triggers and aim/fire flags (mouse and keyboard partners) reach her when 0x2200 is in the mask |
| Clone aim | No camera 1: her aim direction (sub_140C6E800) and aim ray (sub_140C6F020) are answered from the partner's aim while aiming. `giveweapon` gives her player 0's pistol. Shots untested live |
| Clone body | Show-mask bug fixed (show takes a mesh mask). Reloads keep her as she is; `redress` + poll until stable. Verified by mesh id: same 8 meshes/visible set as PC B's own Dina look |
| Plot moments | Cinematic / companion scripted anim -> story Dina shown, clone hidden; after, clone and partner moved to Dina's spot (`plotsim <s>` test) |
| Dialogue | Companion lines play from the clone ('player-2'); story characters in cinematics. Not checked by ear |
| Enemies (M6) | Health, death, spawn sync; alerts once per real alert (`AlertLatch`); clone is the hunt target |
| Pickups (M6) | Log-only: taken pickups are named and reported; the other PC logs whether it has it |
| Checkpoints/pause (M7) | Restart sync both ways (GameRestartCheckpoint + GameRestartEncounter), remote pause |
| Relay (M8) | `coop_relay.exe <secret>` (UDP 47830, secret, rate limit, frame cap). Real games joined through it (349 packets, 0 dropped). `[session] RelayAddress/RelayPort/RelaySecret` or the `relay` command |
| Hot reload | Leak-free over 5 reloads (`leaks`): trampolines in a per-process pool, clone reused, listener guard |

## 2. Dev loop

- Build `cmd /c ".\build.cmd"` (127 unit tests), deploy `cmd /c ".\deploy.cmd"`. coop.dll hot-reloads in the running
  game (also while in the background); PC B pulls it from `tools/share.py` (port 8765, keep it running).
- Launch: `tlou-ii.exe -nolauncher` (not admin), then `tlou2-coop\coop_launcher.exe`. PC B: tasks TLOU2-Game and
  TLOU2-Launcher over SSH.
- Commands: lines in `<game>\tlou2-coop\cmd.txt` (PC B over SSH: `(echo cmd 0)>> file` when it ends in one digit).
  Clone: `clonebody [i]`, `redress`, `giveweapon [i] [name|-] [ammo]`, `weaponinfo`, `clonebuttons <hex>`,
  `catchup on|off`, `facesync on|off`, `clonetheta <rad>`, `plotsim <s>`, `relay <ip> <port> <secret>|off`, `leaks`,
  `dropbody <spawner>`, `players`, `proc <addr>`, `peek <addr> [n]`.
- Live tests: `tools/live/livebatch.sh` (single instance, waits read-only for PC A's game in front; aim and fire,
  reload audit, catch-up A/B, relay walk, body check). Other scripts in tools/live. Recordings stay out of git.
- Sims: `build\coop_lan_sim.exe follow [addr]` (a partner that stands behind the host), `... --relay <ip> <port> <secret>`.
- IDA: `"/c/Program Files/IDA Professional 9.0/idat.exe" -A -L"re/probe.log" -S"re/<script>.py" "re/tlou-ii.i64"`
  (copy scripts from tools/ida into re/). `python -I tools/native.py <exe> <names>` / `native_rev.py`.
- Crashes: `python -I tools/minidump.py <dmp>`; hangs: `tools/livedump.ps1` + `tools/dumpthreads.py`.

## 3. Rules from the user (also in memory)

- Don't stop at "reply X"; carry on. Test and verify yourself (logs, memory reads, recordings).
- Watchdogs: "Roblox Game 2 - Opus Fast" (asked by the user on 10-19) and "Godot game fundamentals scope". Report
  each step to them; the app pauses cross-session sends after 10 without a user message.
- **Banned:** focus tricks, bringing the game to the front by any method (also minimising other windows), hidden
  launches to dodge focus, patching the unfocused-input gate. No coop.ini edits on PC B without the user's OK.
- The user allowed launching and relaunching the games (PC A and PC B over SSH) after crashes.
- No game files or recordings in git.

## 4. Waiting on the game in front (PC A)

The clone's input, like the host's, is focus-gated: with PC A's game in the background her stick does nothing
(drive log `game in front 0`). `livebatch.sh` covers these once it is in front:
1. Shots: PC B aims and fires; her aim follows (`cloneaim:`), no crash.
2. Catch-up A/B and the relay walk: needs PC B's game in front too.
3. Body by eye (the id check already passed).

## 5. Open items

1. Pickups: turn the log-only report into removal once real pickup logs exist.
2. Dialogue by ear; barks (`vox_hook`) untested.
3. Checkpoint-write forwarding still log-only (no writes to the user's save).
4. A death most likely restarts through task-restart-from-continue (hooked); not proven.
5. GPU "device removed" crashes (renderer, ndgi::AssertDeviceRemoved, no mod frame; AMD RX 9070 XT). Timeline:
   10-15 07:57, 19:03, 19:58; 10-18 12:32 (after a level-listener hang, since fixed); 10-19 11:12:16, as the game
   came back to the front after ~1 hour paused in the background (no game-thread work in that time, no driver
   event logged). Track whether crashes follow a long background pause.

## 6. Key engine facts (rva = addr - 0x140000000)

- Players: table 0x411A510 {node, id}; SpawnPlayer 0xDC1440; Process::SetUserId 0x133EC60 (hooked: 'player-2');
  Player+21800 index, +21796 joypad, +19632 NdPlayerJoypad (+0xD0/+0xD8 world stick), Player::Active::Update 0xAD61E0.
- Raw pad 0x824C860 (0x540 each): sticks +0x00/+0x08, masks +0x518/+0x520/+0x528 (A 20, B 80, LB 100, Y 10, X 40,
  LT 200, RT 2000), triggers +0x4F0/+0x4F4. Raw->slot copy sub_14158F710 (hooked, slot 3 = clone).
- Camera managers: 2 (GetGlobal sub_141B1B2B0); manager 1 has no live camera. Aim override table 0x143DF9750
  (sub_1407FAAD0(index, pitch, yaw) degrees, row vectors). Camera pick sub_140C5A3B0 guarded.
- Guards: level listener add/remove (0x1A4D870/0x1A4E210), ActionCommandManager push/pop (0x15B6C90/0x15B6D70).
- Looks: sub_140DBB470 queues a pending look, ignores the current one; draw control sub_141AFD810, show
  sub_141484610(dc, -1, 0, meshMask), hide sub_14147ECD0; meshes count dc+20, entries *(dc+8) x 912 bytes.
- Restarts: GameRestartCheckpoint 0x105F4E0, GameRestartEncounter 0x105F8F0, task-restart-from-continue 0x427500.
- Pickups: ProcessPickup::PickedUp::Enter 0x88FBF0; entity-exists? 0xD3BA00.
- Natives: get/set-object-health 0x40F7A0/0x4230A0, npc-kill 0x63D260, npc-make-aware-of 0x63DDD0,
  c-spawn-npc 0x626240, give-player-weapon 0xC8F870, camera-get-dir-through-reticle 0x407A70.
