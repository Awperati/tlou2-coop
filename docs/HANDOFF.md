# TLOU2 LAN co-op mod: handoff (refreshed 2026-10-20, 06:15)

Repo: `C:\Users\User\TLOU2 Co-op` (GitHub Awperati/tlou2-coop, private). Status below is as of commit `a501392` (both PCs ran `676f843` from 05:56).
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
| Facing while standing | Smooth turn through the game's own turn (`FaceForce = stored`, default), toward the partner's camera yaw led by its turn rate (`FaceLead` 0.15 s, 676f843). Owner 13:0x: smooth, no A-pose; 05:2x: "late" while mouse-looking, lead deployed 05:56, not re-checked. Snap turn (`faceforce off`) is the fallback. See 4.1 |
| Stance | Crouch and prone confirmed by the owner |
| Body | 8 meshes and 3 cloth props match PC B's own Dina. Owner: torso and arms back (14:0x) |
| Hood | Owner: "Ellie hood is great" |
| Aim | Seen on video: she aims where PC B aims. Shots untested live |
| Combat animations on the clone | Built and deployed 10-21 (combat sync, 4.2d). Live: weapon draw on the clone 31-62 ms after the ask (ask 51 ms after the partner's draw), holster 49 ms, partner shot -> her RT press 55 ms, reload 15 ms after the ask. Not yet seen live: her aim pose, her shots and throws (her pad needs her PC's game in front) |
| Combat | Built 10-21, not yet live: no friendly fire on either PC (attack hooks, core/combat.h); the clone takes no hits and her hits count only on her own PC; her weapon, throws (RT) and melee (X) follow the partner (core/weaponsync.h). ROADMAP "Combat between the players" |
| Jump, ledge, ladder, dodge, ledge drop | Deployed 05:56 (dbe37e2/49806e9/4b63c04): the partner's state machine state is sent; entering Jump/Edge/Ladder/Evade/LedgeDrop presses A/A/Y/LB/B on her, gated by her own state. Ropes and ziplines follow out of the jump. Not yet seen live: owner's six checks in [patches/traversal-wiring.md](patches/traversal-wiring.md) section 4 |
| Idle gestures (nose wipe etc.) | Built and tested (64bf822, 832dfcb): partner's layer-1 gesture played once on her. Live end-to-end not seen |
| Pickups | Removal built behind `[sync] Pickups` (default `log` = report only); rules checked against Seattle level data. Open: live name check, and a removed pickup returns after a checkpoint restart on that PC (ROADMAP M6) |
| Hot reload | Code patches use `WriteCodeSafely` (c31569e) after the 05:26 GetPlayer crash; 05:56 reload clean on both PCs |
| Enemies, checkpoints, pause, plot moments | As in ROADMAP (M6/M7). Checkpoint-write forwarding stays log-only: it would overwrite the partner's save point (refused 10-16); owner asked 10-20 |

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

### 4.1 Smooth standing turn without the leak (FaceForce): solved 10-20
- **Result (live, 04:38-04:45, both standing, PC B camera turned by the virtual pad):**
  - `facewatch glob`: player 0's controller reads the force flag at 0xC31A3D as 0 every time (152 of 152 on PC A).
    The force never reaches player 0 with the lock in place. Lock give-ups: 0.
  - `facewatch ctl`: nothing but her own C31340 touches her [ctl+0x1B0] / [ctl+0x8E0].
  - `faceptrs`: no shared pointers. player+0x2E98 is the player's own movement controller.
  - Turns: 10 with the globals + lock (6 on PC A, 4 on PC B), 4 with `stored`, and 4 more after the reload with
    `stored` as the default. Each turn took 1-1.5 s. Own bodies stayed at -0.56 / -1.95 (PC A) and 2.90 / 2.88 (PC B),
    changing only when that PC walked.
  - So the 13:3x leak was the unlocked race. The lock went in at 13:36, but FaceForce was switched off before
    any turn was seen with it. The 13:39 "leak" was a body left over from before the lock.
  - Default now `[clone] FaceForce = stored`: her own stored facing (sub_140C34C80), no globals, no lock.
    `globals` (also `on`) keeps the locked force path; `off` = teleport snaps.
- **Mechanism (works):** native `player-force-facing/f` sets a global flag (0x40FC87E) and a direction (0x40FC880).
  The movement controller update `sub_140C31230(ctl, req)` → `sub_140C31340` uses them as desired facing ([ctl+0x1B0]).
  ctl+280 is the player.
  - The hook sets both only around player 1's update (`FaceCtlHookInstall`, fiber_hook.cpp).
  - She turns through the game's own turn, smoothly, with no A-pose (owner 13:0x).
- **Engine facts (static RE):** only sub_140C31340 reads the flag/dir (rva 0xC31A36 / 0xC31A44). Controller vtable
  0x142CC3CC0, size 0x950, desired-facing getter slot +0x150 (sub_140C2EB10), update sub_140C37110, enter sub_140C35ED0;
  stored facing [ctl+0x890] + [ctl+0x8E0] via sub_140C34C80. player+0x2E98 = the player's own controller.
- **Turn delay (owner 07:2x "2-3 s late"), fixed 93cb149:** the force had waited for the partner to stand 0.5 s and her
  stick to idle 0.8 s, and while the partner walked 40-54 % of their stick samples never reached her engine, so she
  held a fixed heading for seconds. Now the force runs whenever her own engine stick has idled 150 ms (target: their body
  yaw while they move, their camera while they stand). Owner's recorded run 07:52 (100 ms facetrace): she reaches the
  target 0.2-0.45 s after it settles (one 1.2 s), was ~2 s; 0 drift snaps. `python -I tools/facetrace.py <log> [from] [to]`.
  Still open: why her engine ignores a partner stick sample that often (seen 10-19 too).
- Research tools: `facewatch [ms] [ctl|glob]` (debug-register watchpoints, offline 20/20 clean, ran live 10-20), `faceptrs`.
- Switches: `faceforce on|stored|off` (off removes the hook), `standturnto camera|body`, `standturnstep <rad>`, `standturnms <ms>`.

### 4.2 Jump and traversal (deployed 10-20 05:56, not yet seen live)
- **Sender:** each PlayerState carries the player's state machine state (`state`, FNV-1a of the state name, read with
  sub_14133D6B0 on the fiber every 100 ms; any Jump entry is latched). kJumped comes from entering "Jump"; the rise-speed
  `JumpDetector` is only the fallback when the state is unreadable.
- **Receiver:** `TraversalCopy` (core/traversal.h) presses A/A/Y/LB/B for Jump/Edge/Ladder/Evade/LedgeDrop for 150 ms,
  gated by her own state (no jump while airborne, no climb while hanging, B only while she hangs). Shimmy, slide,
  squeeze, rope, zipline and swim follow by her stick drive and the drift snap. Log: `clonetraversal: <state>`.
- **Live checks** (owner, PC A's game in front): [patches/traversal-wiring.md](patches/traversal-wiring.md) section 4.
  The ladder button (Y) is the one unconfirmed binding.
- State ids and the full list of 78 named player states: ROADMAP "Player state machine ids".
- Research tools: `animlog on` (own state changes, vertical speed), `jumptrace <s>`, `tools/logstats.py`.

### 4.2b Idle gestures (nose wipe etc.)
- **Works live (10-20 06:00-06:02, owner idle, nothing in front forced):** every own idle gesture on each PC started once on
  the other PC's clone with the same id: 4 of 4 PC B -> PC A (95df22f1, a0a75315, d8ad71f5, d38b965a), 3 of 3 PC A -> PC B
  (d8ad71f5, a0a75315, d846c30d). Paired times give a ~6.45 s clock offset (PC B behind) and ~85 ms one-way delay.
  At least seven different idle gestures seen. Owner eye check still open.
- Built 10-20. Each PC sends its player 0's layer-1 gesture (PlayerState.gesture);
  `GestureSync` (core/gesturesync.h, TestGestureSync over the wire) starts each partner gesture once on the clone,
  only while the partner stands, via sub_141DDA660 on player+0x54F8. `[clone] GestureSync`, `gesturesync on|off`.
- Live facts: player 0 plays one of at least five idle gestures about every 20 s (2.5-3.6 s each); the clone none.
  `gesturedirect 0x<id>` played one on the clone (3.3 s). simple-gesture / debug-menu events did not.
- Check: `gesturelog on` on both PCs, wait ~1 min standing: each `gesture: player 0 layer 1 <id>` on PC B should
  be followed on PC A by `clonegesture: ... <id> ... started` and `gesture: player 1 layer 1 <id>`.

### 4.2c Section sync (owner request 10-21; works live)
- Each PC saves the story sections its player has been in (`tlou2-coop\explored.txt`, `world|task` lines; delete it to
  reset) and sends its section (kWorldSection world event) on change and every 10 s. If the partner's section differs
  for 5 s: "Your partner is in a different section of the game. Press F9 to go to their section." An explored
  section (or an earlier chapter) goes on one press. Anything else first asks "This area is beyond anywhere you've
  explored before. Do you still wish to continue?" and needs a second press. `[sync] SectionSync`, `SectionKey`.
  Logic: src/core/sections.h (ExploredLog, SectionPrompt), TestSections.
- The jump is the game's own `task-play-debug!` (rva 0xD61500, a deferred task play from the task's start). Ids =
  FNV-1a 64 of the chapter world without `world-` and of the task name.
- Before the jump the clone is removed (`kill-entity 'player-2'`); clone mode respawns her once our player is back.
  While apart, the partner's clone is hidden and not driven or snapped. After our own jump the partner is dressed
  again in their character's look, and the story companion is made passive again.
- Crashes fixed on the way (all 10-21):
  - The player respawned while the clone was alive: assert at rva 0x12541C4 (6ce7826).
  - The clone was snapped to a 4e12 m spot that the other PC sent mid-reload: assert in MeshRayCaster (94a23f5,
    `SpotSane`).
  - A pause-menu load with the clone alive hit a broken nearby-edge list in NotifyFeatureDbLogout (a3df6a4, guard
    hook). It has not been hit since; look for `edge logout: dropped` in the log.
- Commands: `joinpartner` (same as the key), `gotosection <chapter> <task>`, `sectionids`.
- Open: a menu load (chapter select, load save) does not remove the clone first. Only the edge guard covers it.

### 4.2d Combat animations on the clone (combat sync, 10-21; partly live)
- **Goal (owner):** the partner aiming, shooting, reloading, throwing, swinging and switching weapons shows on their
  clone on the other PC within ~0.3 s.
- **Sender (engine state, not pad buttons; PC B's keyboard leaves no trigger bits):** every 33 ms in the script
  context  reads its own player with script natives: get-player-weapon-id 0xC8F1D0 (in hand),
  get-player-weapon-ammo-in-clip 0xC8EF60, get-player-weapon-total-ammo 0xC8F270, player-is-reloading? 0xC9E3A0,
  is-player-switching-weapon? 0xC90B80, player-is-aiming? 0xC9DEC0, player-is-aiming-throwable? 0xC9DF00,
  player-get-aimed-grenade 0xC9B110, player-is-firing? 0xB92990, and the state machine state (Melee).
   (core/combatsync.h) makes the 23-byte  tail: weapon, grenade, clip, held flags
  (aim, throwable aim, reload, switch, firing, melee) and fire/throw/reload/melee counters, so a lost datagram loses no
  event. A shot = a clip drop of the weapon in hand (player-is-firing? stays on ~1 s per shot). Throws come from the
  combat session's throw hooks (); the firing heuristic is only the fallback.
- **Receiver:**  acts on the clone with the GetPlayer override: draws the partner's weapon
  (player-has-weapon? 0xC9C980, give-player-weapon 0xC8F870, player-draw-weapon-by-name 0xC96EF0) only when the partner
  just drew it, aims, shoots or reloads (both games put an idle weapon away after ~10 s on their own), holsters it
  (holster-player-weapon 0xC900F0), keeps her clip equal to the partner's plus the shots she still owes
  (set-player-weapon-clip-ammo 0xCA8A40, never within 500 ms of a press), refills her reserve
  (set-player-weapon-reserve-ammo 0xC885E0), reloads (player-reload 0xCA15F0) and selects the throwable
  (player-select-grenade 0xCA1BB0). Aim holds her LT; each shot or throw is one RT press (70 ms down, 60 ms up);
  a swing is an X press. Her raw LT/RT/X copy is off while combat sync runs; the combat session's 
  block is the fallback with . Her hits do nothing on that PC (core/combat.h).
- **Live 10-21 08:04-08:21** (PC B's player driven by the virtual pad; logs, clocks aligned with the  offset):
  draw asked 51 ms after PC B drew, held 31-62 ms after the ask; RT press 55 ms after PC B's shot; holster asked
  49 ms after PC B's; reload started 15 ms after the ask (clip 2 -> 6 in 1.5 s), all with the game behind other windows.
- **Open:** with her PC's game behind other windows her pad actions read nothing: her NdPlayerJoypad aim byte
  (Player+19632 +0x294) stayed 0 under LT, RT fired nothing, and the virtual pad is ignored too. The action query
  sub_141D96F20 bails on a global input check (component 2 vfunc +24), likely the window focus (not to be patched:
  owner rule). Writing her aim bytes (+660/+661/+662) before her update sets the byte but gives no aim pose, and
  player-fire-weapon-at-reticle fires nothing then. So her aim pose, shots, throws and swings need one run with both
  games in front: each player aims, shoots, reloads and throws once;  on both PCs, then read
   on one PC against  /  on the other.
- **Switches:**  (combatsync on|off),  (auto: RT in front, the engine
  fire native behind), ; ;  drives her without the partner. The virtual pad takes triggers in combos ().
- **Engine facts:** natives were found by hashing the exe's own native-name strings (6774 registered). Player+11976
  inventory (vfunc +528 selected weapon, +232 find by id), +12128 PlayerItemController (in hand), +12144 fire
  controller, +12152 reload controller, +12304 grenade controller, +12648 throwable aim. NdPlayerJoypad actions are FNV
  names ("aim" 0xE701C01905288CB6, "fire" 0xAA77F578EFDFC4B9, "reload" 0xE7F9484DA4B873AC). player-force-aiming
  0xC99230 sets a global byte (0x1440FB837), not one per player. Square with nothing drawn is a melee swing (state
  "Melee"). The pistol 226cdcf298320a8c holds 6.

### 4.3 Pickups to "mixed"
- Live check that runtime pickup names match level-data names and classes (log line `pickup: '<name>' is a <class>`).
  Then ask the owner before switching `[sync] Pickups` from `log`.

### 4.4 Smaller items
- Shots: PC B fires; check her shots and damage (livebatch has the steps).
- Dialogue by ear; barks (`vox_hook`).
- Checkpoint-write forwarding is still log-only: it would overwrite the partner's save point (refused 10-16). The owner
  was asked 10-20 whether that's allowed; M4's engine load hold is waiting on the owner too (risk of freezing the game).
- Pickups after a checkpoint restart: see ROADMAP M6 (one live step with the owner idle).
- GPU "device removed" crashes after long background pauses (10-19 11:12). Track them.
- Hot reload crash (PC A 10-20 05:26, access violation at GetPlayer+0): code patches were plain memcpys under running
  job threads. Now every patch goes through `WriteCodeSafely` (src/mod/code_patch.h: suspend other threads, wait until
  none sits inside the bytes, write). ctest `patch_race`: the old memcpy crashed 6/6 runs within 100 swaps; safe 0
  wrong results over 3 x 400 swaps. In the game: the 05:56 reload on both PCs was clean ("GetPlayer redirected").
- PC B screen full of black lines (owner, 10-20 04:38). The game kept responding and the mod log showed nothing. Not fixed, not ours to fix: watch it.
- Theta: kept across reloads, starts settled, and drops >0.6 rad outliers. If her walk stalls, check `clonetheta:` lines; `clonetheta 3.13` resets it.

## 5. Dev loop and tools

- Build `cmd /c ".\build.cmd"` (tests run). Deploy `cmd /c ".\deploy.cmd"` (hot reload on PC A; PC B pulls from share.py).
- Before a deploy: `bash tools/live/predeploy.sh` (build without the slow patch_race test, coop_tests, git state,
  share.py, PC B over SSH, game PIDs; read-only). `--deploy` then deploys and waits until each running PC's mod
  log restarted after the deploy (its own clock). Run summaries: `python -I tools/logstats.py <logs> [--from HH:MM]`.
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
- Guards: level listener add/remove (0x1A4D870/0x1A4E210), ActionCommandManager push/pop (0x15B6C90/0x15B6D70),
  NearbyEdgeMgr::NotifyFeatureDbLogout 0xA83F30 (edge list +0xF0 in frame memory, swapped each frame in 0xA842A0).
- Tasks: task-get-current 0xD61110, -world-id 0xD61380, -node-id 0xD611A0, -symbol 0xD612F0, task-find 0xD60F60,
  task-play-debug! 0xD61500 (queues sub_141C46890).
- Looks: sub_140DBB470 queues a pending look; draw control sub_141AFD810, show sub_141484610(dc, -1, 0, meshMask),
  hide sub_14147ECD0; meshes count dc+20, entries *(dc+8) x 912 bytes, visible bits +900, name id +0x18.
  Cloth props: inventory Player+11264 (SafePropObject).
- Restarts: GameRestartCheckpoint 0x105F4E0, GameRestartEncounter 0x105F8F0, task-restart-from-continue 0x427500.
- Pickups: ProcessPickup::PickedUp::Enter 0x88FBF0; entity-exists? 0xD3BA00; kill-entity 0xD4BB40.
- Attacks: SendAttack 0x3921E0, PostAttack 0x392070 (AttackInfo*, target), Player::HandleAttackEvent 0xB7AF30; attacker = AttackInfo+8/+16 handle. Player type id +72 0x333DC56DDFFD8EA0.
- Weapons: get-player-weapon-id 0xC8F1D0, player-get-aimed-grenade 0xC9B110, player-instant-draw-weapon-by-name 0xC9DA80; grenade release 0xB8D9A0.
- Natives: get/set-object-health 0x40F7A0/0x4230A0, npc-kill 0x63D260, npc-make-aware-of 0x63DDD0,
  c-spawn-npc 0x626240, give-player-weapon 0xC8F870, camera-get-dir-through-reticle 0x407A70.
