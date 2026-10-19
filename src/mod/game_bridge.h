// Everything that touches TLOU2's memory lives here.
// Addresses come from config/offsets.ini as byte patterns + field offsets,
// so new reverse-engineering results drop in without a rebuild.
// Each feature reports whether it is mapped; unmapped features are no-ops.
#pragma once
#include "core/config.h"
#include "core/protocol.h"
#include "core/clonedrive.h"
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace coop {

struct BridgeFeatures {
    bool localPlayer = false;   // read the player's position/yaw/health
    bool teleport = false;      // write the player's position
    bool puppet = false;        // drive the companion NPC as a remote player
    bool worldPack = false;     // know which chapter is loaded
    bool objective = false;     // know the current objective / transition
    bool combat = false;        // know when enemies are engaged
};

class GameBridge {
public:
    bool Init(const Ini& offsets);
    void Shutdown();   // undo code patches; call before the DLL unloads

    // Companion (M3). Both run on a game fiber and log what they find.
    void QueueBuddyReport();
    void QueueBuddyTeleport(const Vec3& pos, float yaw);

    // Levels (M4): poll LevelMgr's loaded-level list and log loads/unloads.
    void PollLevels();
    void DumpLevels();        // detail for working out the Level layout
    // Story tasks: GameTaskGraphMgr (global, rva-relative) -> graphs -> active subnode.
    void DumpTasks();
    void PollTasks();
    // Cinematic (IGC) playing, from CinematicMgr as the cinematic-playing? native reads it.
    bool InCinematic();
    void PollCinematic();
    bool rawCinematic();
    // M5 research: log the dev-menu spawn-character list (DC data) and its gate.
    void QueueSpawnListDump();
    // Spawners in loaded levels (Level+256 -> {+8 EntitySpawner**, +16 count}); filter by text.
    void DumpSpawners(const std::string& filter);
    // Research: call a script native at <rva> with args on a game fiber and log its result.
    // Args: "sid:<name>" (FNV-1a symbol), numbers (0x.. or decimal). Build-pinned.
    void QueueNativeCall(const std::string& line);
    // Research: describe a process pointer; with here=true teleport it beside the player.
    void QueueProcess(uintptr_t proc, bool here, float metres = 2.f);
    // The process a spawner spawned: spawner+96 handle {node, id@+104}; process = *(node+8).
    void QueueSpawned(const std::string& spawner, bool here, float metres = 2.f);
    // Which NPC the network puppet drives: the story buddy (empty) or a spawner's NPC.
    void SetPuppetSpawner(const std::string& spawner) { puppetSpawner_ = spawner; }
    // Give the companion her AI back (partner left, session ended, mod unloading).
    void ReleasePuppet();
    // Self mode: stand this far ahead of the hidden player (out of the camera's NPC fade).
    void SetSelfOffset(float metres) { selfOffset_ = metres; }
    // Character+0x9BC: when set, the camera's near-NPC fade skips that character
    // (gate at the top of rva 0x1CE32A0). Lets the companion stand on the player's spot.
    void SetNoCameraFade(bool on) { noFade_ = on; noFadeLogged_ = false; }
    std::atomic<bool> noFade_{true};
    bool noFadeLogged_ = false, colliderOff_ = false, playerIgnoring_ = false, navIgnoreLogged_ = false;
    unsigned long long lastIgnoreAt_ = 0;
    uintptr_t noFadeOn_ = 0;
    // npc-move-to (native rva 0x63F0E0): walk/run an NPC (by name) to a point beside the player.
    void QueueMoveTo(const std::string& npcName, float metres, int motion, int goal = 0, bool inScript = true);
    // Which Npc* a script NPC argument resolves to (arg reader rva 0x3FF7D0).
    void QueueResolveNpcArg(const std::string& name);
    // Log the state-script context seen inside a script native call.
    void QueueScriptContextReport();
    // Research: list process pointers stored inside the player (children, attachments).
    void QueuePlayerLinks();
    // Hide/show the player's body, weapons and every prop object (hair, hood, backpack).
    void QueuePlayerVisible(bool visible, bool quiet = false);
    int WalkSubtreeSetVisible(uintptr_t root, bool visible, bool quiet = false);
    std::vector<uint32_t> playerAttachIds_;
    std::atomic<int> stance_{0};          // 0 stand, 1 crouch, 2 prone
    std::atomic<bool> aiming_{false}, stanceQueued_{false};
    int lastDemeanor_ = -1;
    Vec3 moveRefPos_; unsigned long long moveRefAt_ = 0; bool movingNow_ = false, lastRemoteMoving_ = false;
    // standdem <n>: demeanor for a standing partner (-1 = uneasy where the level is tense). Default 0:
    // uneasy (1) made her flick between crouch and stand every second in the tunnels (recorded 06:49).
    int standDemeanor_ = 0;
    unsigned long long lastDemeanorAt_ = 0, stopSince_ = 0, lastDemOverrideLog_ = 0;
    uint32_t lastDemOverrides_ = 0;
    Vec3 stopTarget_;
    bool parked_ = false;
    // Shots (2026-10-17): this PC's trigger, counted for PlayerState, and the other PC's
    // shots replayed by its stand-in NPC (the buddy on the host, the host body on the partner).
    void PollFiring();
    void MirrorShots(const std::string& npc, uint64_t target, int n);
    ShotCounter shotCounter_;
    std::atomic<bool> firing_{false}, firingQueued_{false};
    std::atomic<float> aimYaw_{0}, aimPitch_{0};   // our aim direction (PollFiring)
    std::atomic<float> cloneAimYaw_{0}, cloneAimPitch_{0};   // the other player's, for the clone
    bool cloneAimOverride_ = true;   // [clone] AimOverride: point the clone's camera with the aim override
    bool cloneAimSet_ = false;
    float cloneAimYawSign_ = 1.f, cloneAimPitchSign_ = -1.f;   // [clone] AimYawSign / AimPitchSign
    int cloneAimOrder_ = 0;          // [clone] AimOrder: 0 = override(pitch, yaw), 1 = override(yaw, pitch)
    std::atomic<uint8_t> shots_{0};
    std::atomic<uint64_t> shotTarget_{0};
    std::map<uint8_t, uint8_t> lastShotsSeen_;   // per remote slot
    unsigned long long lastShotLog_ = 0, lastTargetLog_ = 0;
    // A mirrored shot that hits an enemy must not count twice: the shooter's own PC already
    // reports its hit. Health before the stand-in's shot, kept for kFireGuardMs.
    std::mutex fireGuardMu_;
    std::map<uint64_t, std::pair<unsigned long long, float>> fireGuard_;
    bool FireGuardHp(uint64_t sid, float* hp);
    void SetFireGuardHp(uint64_t sid, float hp);
    Vec3 selfPrevPos_, selfVel_;
    unsigned long long selfPrevAt_ = 0, lastSelfLog_ = 0;
    float selfLead_ = 1.8f;   // metres ahead of a moving player to aim (move-to arrival slowdown)
    void SetSelfLead(float s) { selfLead_ = s; }
    float selfNudge_ = 0.f;   // fraction of her error pulled per tick by writing Process+0x130; no effect in game (overwritten), off
    bool selfDir_ = true;
    float remoteScale_ = 0.f;   // remotescale <s>: fixed speed scale for the remote body (0 = auto)
    bool remoteCatch_ = false, catchUp_ = false;   // remotecatch on|off: slowed run to close a walk lag (no gain, 07:36)
    bool remoteSnap_ = true;   // remotesnap on|off: snap her heading when she starts off facing away
    unsigned long long catchSince_ = 0;
    bool remoteDir_ = true;   // remote body: move-in-direction steering (off = move-to only)
    float selfDirLead_ = 1.5f;   // loop selftest 2026-10-14: 0.5 -> 1.6 m mean at a run, 2.0 -> 1.5 m
    bool dirActive_ = false, runGait_ = false;
    int lastGait_ = -1;
    float selfWalkSpeed_ = 1.4f, selfRunSpeed_ = 3.3f;   // her nominal pace per gait (m/s), see self log
    float selfSpeedGain_ = 1.0f;
    Vec3 lastDir_;   // heading of the last move-in-direction order
    int selfStyle_ = 1;   // 0: gait from her error, re-send every 300 ms; 1: gait from player speed + speed scale                          // m/s of pace change per metre ahead/behind
    float lastScale_ = 1.f; unsigned long long lastScaleAt_ = 0;
    Vec3 herPrevPos_, herVel_; unsigned long long herPrevAt_ = 0;     // steer with npc-move-in-direction while the player moves
    void SetSelfDir(bool on) { selfDir_ = on; }
    unsigned long long nudges_ = 0;
    void SetSelfNudge(float f) { selfNudge_ = f; }
    float selfOffset_ = 0.f;   // 0 = exact spot, needs noFade_
    unsigned long long lastAimAt_ = 0, lastFaceAt_ = 0;
    bool aimingNow_ = false;
    // Make every attachable in the world visible again (undo a bad hide).
    void QueueShowAllAttachables();
    // Show every attachable within 2.5 m of the companion and her own meshes; log names.
    void QueueShowCompanion(bool visible = true, float radius = 2.5f, bool quiet = false);
    // load-level / is-level-loaded? on a package name (e.g. dina-player-skel), logged.
    void QueueLoadPackage(const std::string& name, bool load, bool log = true);
    // Want an actor package (WantLoadInfo type 2), as level-dependent actors are loaded.
    void QueueLoadActor(const std::string& name);
    void QueueUnwant(const std::string& name, int times = 1);
    // Hide the story companion (meshes off, no collision, AI kept) or show her again.
    void QueueCompanionHidden(bool hidden, bool keepAi = false);
    // Plot moments of the story companion (clone mode): in a scripted anim
    // (npc-is-in-scripted-anim?, rva 0x63C270) or inside an action pack such as a boost or a door
    // (npc-get-entered-ap-type, rva 0x636280; 0 = none). Polled into these.
    void QueuePlotState(const std::string& npc);
    std::atomic<bool> plotScripted_{false};
    std::atomic<uint64_t> plotAp_{0};
    std::atomic<bool> plotQueued_{false};
    void QueuePlayerToBuddy(unsigned index);   // teleport player <index> onto the story companion's spot
    void QueueVoxNames();
    void QueueTreeOf(const std::string& spawner, int moveToOff = 0);
    void QueueRehome(const std::string& spawner, const std::string& levelName);
    void QueueSpawnWithLook(const std::string& spawner, const std::string& look, bool nearPlayer = false);
    bool MoveSpawnerTo(const std::string& spawner, const Vec3& at);   // script context
    // Spawn a plain NPC wearing a player look, make it friendly and drive it as the remote body.
    void DropRemoteBody(const std::string& spawner = {});
    void QueueRemoteBody(const std::string& spawner, const std::string& look);
    // M6 enemy sync, keyed by spawner-name FNV ids. Host: QueueEnemySnapshot collects
    // health/death of every spawned "npc-" spawner; TakeEnemyOut hands it to the session.
    // Partner: ApplyEnemyStates follows the host and reports local damage via TakeDamageOut.
    // partner = true: the partner's own scan. Enemies alive here that the host never listed
    // are reported (kReportSpawned) so the host spawns them too.
    void QueueEnemySnapshot(bool partner = false);
    std::vector<EnemyState> TakeEnemyOut();
    void ApplyEnemyStates(const std::vector<EnemyState>& v);
    void ApplyEnemyDamage(const EnemyDamage& d);
    std::vector<EnemyDamage> TakeDamageOut();
    void QueueNamePickups(std::vector<uintptr_t> procs);
    std::vector<std::pair<uint64_t, std::string>> TakePickupOut();
    void QueuePickupCheck(uint64_t sid, const std::string& name, bool despawn);
    std::vector<std::pair<uint64_t, std::string>> pickupOut_;
    std::mutex enemyMu_;
    std::vector<EnemyState> enemyOut_;
    std::vector<EnemyDamage> damageOut_;
    std::map<uint64_t, bool> enemyAlive_;      // host: ids seen alive (dead ones keep being sent)
    std::map<uint64_t, float> enemyHostHp_;    // partner: last health the host sent
    std::set<uint64_t> enemyKilledHere_;       // partner: already npc-kill'ed
    std::map<uint64_t, unsigned long long> enemyHitAt_;   // partner: when a local hit was reported
    AlertLatch alertLatch_;                              // either side: one action per real alert
    std::atomic<bool> enemySnapBusy_{false};
    std::set<uint64_t> enemyHostKnown_;       // partner: every id the host ever sent
    std::set<uint64_t> enemySpawnReported_;   // partner: already reported as partner-only
    std::map<uint64_t, unsigned long long> enemySpawnTried_;   // either side: last spawn attempt
    std::string PickBodySpawner();   // script context
    void QueueAutoBody(const std::string& look);   // keep a live remote body wearing `look`
    void RunPickBody();   // pickbody: dry run of PickBodySpawner
    std::set<std::string> badBodySpawners_;
    std::wstring bodyFile_;   // tlou2-coop\body.txt: spawner of the current body, kept across reloads
    bool bodyFileChecked_ = false;
    // Partner's PC: speaker names of the host's character ("player", "ellie"). Their lines
    // are remapped to the body that stands in for the host (dialog facts hook).
    std::vector<std::string> hostSpeakers_;
    void MapHostSpeech(const std::string& bodySpawner);
    bool autoBodyBusy_ = false;
    int autoBodyTries_ = 0;
    unsigned long long autoBodyAt_ = 0;
    void QueueNpcState(const std::string& name, int faction);   // npcstate <npc> [faction]
    bool companionHidden_ = false, companionLogged_ = false;
    // Make the local player wear a No Return look (e.g. dina-player): load its packages, poll the request.
    // Packages from looks.ini (tools/looks.py); empty = the built-in Dina table.
    void StartLookSwap(const std::string& look, const std::vector<std::string>& packages = {});
    void PollLookSwap();
    std::string lookSwap_;
    unsigned long long lookSwapStart_ = 0, lookSwapLast_ = 0, lookShowAt_ = 0, lookTeleAt_ = 0;
    std::vector<std::string> lookPackages_;
    int lookStable_ = 0;
    bool LookPackagesLoaded();
    void QueueLookState();
    // Second player research (2026-10-17): SpawnPlayer (rva 0xDC1440) with a player index > 0,
    // the way spawn-player does it for index 0 but without killing the current player.
    void QueueSpawnPlayer(unsigned index, const std::string& art);
    void QueuePlayerTable();
    // Engine joypad slots: component 1 + 0x520, 0x2570 bytes each (sub_141D95E60). Slot 2 =
    // player 0's pad, slot 3 = player 1's. padmirror copies 2 into 3 each tick (input test).
    uintptr_t JoypadSlot(int slot);
    void TickPadMirror();
    bool padMirror_ = false;
    // clonedrive on|off: the other player's stick drives player 1 (slot 3).
    void SetCloneMove(float x, float z, uint64_t held = 0, const Vec3* pos = nullptr, float yaw = 0, bool moving = false, int stance = -1, float aimYaw = 0, float aimPitch = 0, uint8_t flags = 0);
    std::atomic<bool> cloneAimingFlag_{false}, cloneFiringFlag_{false};
    // Stance sync ([clone] StanceSync): crouch is a toggle, so passing the B button through let the
    // clone's stance drift from the other PC's (crawling on one PC, crouching on the other, 12:06).
    // Instead B is pressed for her until her stance matches the other player's (0 stand, 1 crouch, 2 prone).
    bool cloneStanceSync_ = true;
    bool cloneCatchUp_ = true;            // [clone] CatchUp: steer and sprint toward the other player's spot
    float cloneAimSign_ = 1.f;            // [clone] AimSign: right stick direction that turns her toward the target yaw
    unsigned long long lastAimLog_ = 0, lastErrLog_ = 0;
    std::atomic<int> cloneWantStance_{-1};
    unsigned long long stanceBUntil_ = 0, stanceNextAt_ = 0;
    Vec3 cloneTarget_; float cloneTargetYaw_ = 0; bool cloneTargetMoving_ = false, cloneHasTarget_ = false;
    unsigned long long cloneFixAt_ = 0, cloneStillSince_ = 0;
    std::mutex cloneMu_;
    std::atomic<uint64_t> cloneHeld_{0};
    Vec3 plotEndPos_; float plotEndYaw_ = 0; std::atomic<bool> plotEndReady_{false};   // host: send the partner here
    void TickCloneDrive();
    bool cloneDrive_ = false;
    bool logPadBits_ = false;          // padbits on|off: log this PC's held-button mask when it changes
    uint64_t lastPadHeld_ = 0;
    uint64_t cloneButtonMask_ = 0;     // [clone] Buttons: which held bits reach the clone (0 = none yet)
    bool cloneWorldStick_ = true;    // write the wanted world stick before her update: her body faces where she walks (user 11:25: "faces normally"; the rotation write bugged her animation)
    std::atomic<bool> cloneDriveQueued_{false};
    std::atomic<float> cloneMoveX_{0}, cloneMoveZ_{0};
    std::atomic<unsigned long long> cloneMoveAt_{0};
    bool cloneFaceSync_ = false;          // [clone] FaceSync: face where the other player faces
    unsigned long long lastFaceLog_ = 0;
    bool cloneThetaFromMotion_ = true;
    ThetaCalibrator thetaCal_;   // [clone] ThetaFromMotion: calibrate the stick from her real motion
    float lastCmdX_ = 0, lastCmdZ_ = 0, motionCmdX_ = 0, motionCmdZ_ = 0, motionStickX_ = 0, motionStickY_ = 0;
    Vec3 motionRef_; unsigned long long motionRefAt_ = 0, lastThetaLog_ = 0;
    float cloneTheta_ = 3.14159265f, lastStickX_ = 0, lastStickY_ = 0, prevStickX_ = 0, prevStickY_ = 0;
    int steadyTicks_ = 0;
    unsigned long long lastCloneLog_ = 0;
    std::atomic<bool> padMirrorQueued_{false};
    void IssueStop(const std::string& name);
    void ActiveCount();
    uintptr_t PlayerProcess(unsigned index);   // entry i of the player table, game fiber only
    void QueuePlayerBeside(unsigned index, float metres);
    void QueuePlayerShow(unsigned index, bool visible = true);
    // Give player <index> a weapon with the game's give-player-weapon, run with GetPlayer() pointed at
    // that player. Empty name: the pistol in player 0's pistol slot (get-player-slotted-pistol).
    void QueueGiveWeapon(unsigned index, const std::string& name, int ammo);
    void QueueWeaponInfo(unsigned index);
    void QueueCloneBody(unsigned index);
    void QueuePlayerProps(unsigned index);
    int ShowPlayerProps(uintptr_t player, bool visible);
    std::vector<uint8_t> MeshEntries(unsigned index);
    void QueueCloneMeshPoll();
    std::atomic<int> meshTotal_{-1}, meshVisible_{-1};
    bool HasPlayer(unsigned index) { return PlayerProcess(index) != 0; }
    void QueuePlayerJoypad(unsigned index, int joypad);
    void QueuePlayerLook(unsigned index, const std::string& look);
    // Clone look: want the look's actor packages (QueueLoadActor) and report when all are loaded.
    void WantCloneLook(const std::vector<std::string>& packages);
    void QueueCloneLookCheck();
    std::vector<std::string> clonePkgs_;
    std::atomic<bool> clonePkgsLoaded_{false}, clonePkgsQueued_{false};
    // Names of the buddy's demeanors (Npc+7776 count, +7784 defs; def+0 char* name).
    void QueueDemeanorNames();
    // Player stance, sampled on a game fiber (player-is-crouched?/prone?/aiming? natives).
    void PollStance();   // process ids of the player's weapons/flashlight
    size_t LoadedLevelCount() const { return levels_.size(); }
    const BridgeFeatures& features() const { return f_; }

    std::optional<PlayerState> ReadLocalPlayer(uint32_t tick);
    bool TeleportLocalPlayer(const TeleportTo& t);
    // selfMode: s is the local player (self puppet); otherwise a remote player (any slot, incl. the host = 0).
    bool DrivePuppet(uint8_t slot, const PlayerState& s, bool selfMode = false);
    std::string CurrentWorldPack();
    std::string CurrentTask() const { return taskName_; }
    // M7 actions. task-restart-from-continue (native 0x427500, no args) runs
    // GameRestartCheckpoint(0, 1), like the pause menu's Restart Checkpoint.
    // pause-game-clock 0x41A060 / unpause-game-clock 0x427B40 freeze gameplay time.
    void QueueRestartFromCheckpoint();
    void QueueGameClock(bool paused);
    // game-paused? (native 0xD3D240) without a script call, so it works while scripts are
    // stopped: clock component 1 +0xF8 time scale is 0, or byte [0x143603768]+99 is set.
    // std::nullopt if the fields can't be read.
    std::optional<bool> IsPaused() const;
    std::optional<Objective> CurrentObjective();
    bool InCombat();
    bool GameWindowFocused() const;

    // Player found but position offset unknown: call ~1/s while the player moves.
    // Scores float3 fields that change like a walking character and logs the best.
    bool needsDiscovery() const { return player_.root && (!offPos_ || discoverForced_); }
    void DiscoverTick();
    uintptr_t PlayerObject() const { return follow(player_); }

private:
    struct Chain { uintptr_t root = 0; std::vector<uint32_t> hops; };
    bool resolveChain(const Ini& ini, const char* section, Chain& out);
    uintptr_t follow(const Chain& c) const;
    uintptr_t resolveFunction(const Ini& ini, const char* key);
    bool engineTeleport(uintptr_t target, const Vec3& pos, float yaw, uint64_t eventId = 0);

    BridgeFeatures f_;
    Chain player_, companion_, worldName_, objective_, combat_;
    uint32_t offPos_ = 0, offYaw_ = 0, offRot_ = 0, offHealth_ = 0, offVel_ = 0;
    uintptr_t fnEventSend_ = 0, fnBindingDefault_ = 0, fnBoundFrameCtor_ = 0, fnGetAnyBuddy_ = 0;
    uintptr_t findBuddyOnFiber();
    uint64_t teleportEvent_ = 0, npcTeleportEvent_ = 0;
    uint32_t npcTeleportSlot_ = 0;
    float puppetMaxDistance_ = 40.f;
    std::string puppetSpawner_;
    float puppetTeleportDistance_ = 15.f;
    bool puppetAiOff_ = false;
    std::string puppetName_;
    std::string remoteBodySpawner_;   // NPC stand-in body (QueueRemoteBody); hidden by DropRemoteBody
    Vec3 lastMoveTarget_;
    Vec3 progressPos_;
    unsigned long long moveIssuedAt_ = 0, progressAt_ = 0;
    void SetNpcAi(const std::string& name, bool on);
    void IssueMoveTo(const std::string& name, const Vec3& pos, float yaw, int motion);
    void IssueMoveInDir(const std::string& name, const Vec3& dir, int motion);
    void SetSpeedScale(const std::string& name, float s);
    uintptr_t spawnedProcessOnFiber(const std::string& spawner);
    unsigned long long lastPuppet_ = 0, lastPuppetLog_ = 0;
    std::atomic<bool> puppetQueued_{false};
    bool engineTeleport_ = false, discoverForced_ = false;
    uint32_t offObjPos_ = 0, offObjRadius_ = 0, offObjTransition_ = 0;

    struct Snapshot { uintptr_t obj = 0; std::vector<uint8_t> self; std::vector<std::vector<uint8_t>> kids; std::vector<uintptr_t> kidPtrs; };
    Snapshot last_;
    std::map<uint64_t, int> scores_;   // key: (childIndex+1) << 32 | fieldOffset
    int discoverTicks_ = 0;

    uintptr_t componentTable_ = 0, taskMgr_ = 0, cinematicMgrPtr_ = 0, igcObjPtr_ = 0, igcArgPtr_ = 0;
    std::atomic<int> igcPlaying_{0};
    std::atomic<bool> igcQueued_{false};
    std::string cineRaw_;
    unsigned long long cineSeen_ = 0;
    std::atomic<bool> cinePlaying_{false};
    int cineState_ = -1;
    std::vector<uintptr_t> activeSubnodes_;
    std::string taskName_, chapterWorld_;
    Vec3 taskStartPos_;
    bool chapterChanged_ = false;
    std::vector<uint64_t> levels_;
    std::map<uint64_t, std::string> levelNames_;
    uintptr_t levelMgr() const;
    std::vector<std::pair<uint64_t, uintptr_t>> readLevels() const;
    std::string levelName(uint64_t sid, uintptr_t level);
};

} // namespace coop
