// Everything that touches TLOU2's memory lives here.
// Addresses come from config/offsets.ini as byte patterns + field offsets,
// so new reverse-engineering results drop in without a rebuild.
// Each feature reports whether it is mapped; unmapped features are no-ops.
#pragma once
#include "core/config.h"
#include "core/protocol.h"
#include <atomic>
#include <map>
#include <optional>
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
    void QueueProcess(uintptr_t proc, bool here);
    // The process a spawner spawned: spawner+96 handle {node, id@+104}; process = *(node+8).
    void QueueSpawned(const std::string& spawner, bool here);
    size_t LoadedLevelCount() const { return levels_.size(); }
    const BridgeFeatures& features() const { return f_; }

    std::optional<PlayerState> ReadLocalPlayer(uint32_t tick);
    bool TeleportLocalPlayer(const TeleportTo& t);
    bool DrivePuppet(uint8_t slot, const PlayerState& s);
    std::string CurrentWorldPack();
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
