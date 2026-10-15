// UDP session for LAN play. One host (authoritative world), 1-3 clients.
// State/input packets are unreliable; story/teleport messages are reliable
// (resent until acked, deduplicated on receive).
#pragma once
#include "core/protocol.h"
#include <functional>
#include <map>
#include <deque>
#include <cstdint>

struct sockaddr_in;

namespace coop {

using SteadyMs = uint64_t;
SteadyMs NowMs();

struct FoundHost {
    std::string address;   // dotted IPv4
    Announce info;
};

class Session {
public:
    enum class Role { None, Host, Client };

    struct Callbacks {
        std::function<void(uint8_t slot, const Hello&)> onPeerJoined;
        std::function<void(uint8_t slot)> onPeerLeft;
        std::function<void(const Welcome&)> onWelcome;
        std::function<void(const std::string& reason)> onRejected;
        std::function<void(uint8_t slot, const PlayerState&)> onPlayerState;
        std::function<void(uint8_t slot, const InputFrame&)> onInput;
        std::function<void(const Objective&)> onObjective;
        std::function<void(uint8_t slot)> onTeleportRequest;
        std::function<void(const TeleportTo&)> onTeleportTo;
        std::function<void(uint8_t slot, CharacterId)> onCharacterSelect;
        std::function<void(uint8_t slot, const WorldEvent&)> onWorldEvent;
        std::function<void(const std::vector<EnemyState>&)> onEnemyStates;
        std::function<void(uint8_t slot, const EnemyDamage&)> onEnemyDamage;
        std::function<void(const CheckpointEvent&)> onCheckpointWritten;
        std::function<void(uint8_t slot, const RestartEvent&)> onCheckpointRestart;
        std::function<void(uint8_t slot, const PauseEvent&)> onPaused;
    };

    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool StartHost(const std::string& sessionName, uint16_t port, uint8_t maxPlayers, bool enableDiscovery = true);
    bool StartClient(const std::string& hostAddr, uint16_t port, const Hello& hello);
    void Stop();

    // Call once per frame. Receives, resends, times out peers, answers discovery.
    void Poll();

    // Host: to every client. Client: to host.
    void SendPlayerState(const PlayerState& s);
    void SendInput(const InputFrame& in);
    void SendObjective(const Objective& o);                 // host only
    void SendTeleportRequest();                             // client only
    void SendTeleportTo(uint8_t slot, const TeleportTo& t); // host only
    void SendCharacterSelect(CharacterId c);
    void SendWorldEvent(const WorldEvent& e);
    void SendEnemyStates(const std::vector<EnemyState>& v);   // host only; split into packets
    void SendEnemyDamage(const EnemyDamage& d);               // client only
    void SendCheckpointWritten(const CheckpointEvent& e);     // host only
    void SendCheckpointRestart(const RestartEvent& e);        // host: to all; client: to host
    void SendPaused(const PauseEvent& e);                     // host: to all; client: to host

    // Blocking LAN scan; returns hosts that answered within timeoutMs.
    static std::vector<FoundHost> Discover(uint32_t timeoutMs, uint16_t discoveryPort = kDiscoveryPort);

    Role role() const { return role_; }
    bool connected() const;
    uint8_t localSlot() const { return localSlot_; }
    size_t peerCount() const { return peers_.size(); }
    uint32_t rttMs(uint8_t slot) const;
    void setChapter(const std::string& c) { chapter_ = c; }
    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

private:
    struct Pending { std::vector<uint8_t> bytes; SteadyMs lastSend = 0; int tries = 0; };
    struct Peer {
        uint32_t ip = 0; uint16_t port = 0;   // network byte order
        uint8_t slot = 0;
        SteadyMs lastHeard = 0, lastPing = 0;
        uint32_t rtt = 0;
        uint16_t nextSeq = 1;
        std::map<uint16_t, Pending> outbox;
        std::deque<uint16_t> seenSeqs;
        bool welcomed = false;
    };

    void sendTo(const Peer& p, const std::vector<uint8_t>& bytes);
    void sendUnreliable(Peer& p, MsgType t, const std::vector<uint8_t>& body);
    void sendReliable(Peer& p, MsgType t, const std::vector<uint8_t>& body);
    void handlePacket(uint32_t ip, uint16_t port, const uint8_t* data, size_t len);
    void handleDiscovery();
    Peer* findPeer(uint32_t ip, uint16_t port);
    Peer* peerBySlot(uint8_t slot);
    void dropPeer(uint8_t slot);
    Announce makeAnnounce() const;

    Role role_ = Role::None;
    uintptr_t sock_ = ~uintptr_t(0);
    uintptr_t discoSock_ = ~uintptr_t(0);
    uint32_t sessionId_ = 0;
    uint32_t hostIp_ = 0;      // client: the host's address, kept to rejoin after it restarts
    uint16_t hostPort_ = 0;
    uint8_t localSlot_ = 0;
    uint8_t maxPlayers_ = 2;
    uint16_t gamePort_ = kDefaultPort;
    std::string sessionName_, chapter_;
    std::map<uint8_t, Peer> peers_;      // host: all clients; client: slot 0 = host
    Hello pendingHello_;
    SteadyMs lastHelloSend_ = 0;
    Callbacks cb_;
};

} // namespace coop
