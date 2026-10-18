#include "core/session.h"
#include "core/relay.h"
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <chrono>
#include <mutex>
#include <random>
#include <set>

namespace coop {

namespace {
constexpr SOCKET kBad = INVALID_SOCKET;
constexpr SteadyMs kPeerTimeoutMs = 10000;
constexpr SteadyMs kPingEveryMs = 1000;
constexpr SteadyMs kHelloEveryMs = 500;
constexpr int kMaxResends = 50;

void InitWinsock() {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}

SOCKET MakeUdp(uint16_t port, bool reuse, bool broadcast) {
    InitWinsock();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return kBad;
    BOOL on = TRUE;
    if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    // Stop ICMP "port unreachable" from surfacing as WSAECONNRESET on recvfrom.
    BOOL off = FALSE; DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closesocket(s); return kBad; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    return s;
}

std::vector<uint8_t> Packet(MsgType t, uint8_t flags, uint16_t seq, uint32_t sid, const std::vector<uint8_t>& body) {
    Writer w;
    Header h; h.type = t; h.flags = flags; h.seq = seq; h.sessionId = sid;
    Write(w, h);
    w.raw(body.data(), body.size());
    return w.data();
}

template <class T> std::vector<uint8_t> Body(const T& v) { Writer w; Write(w, v); return w.data(); }
} // namespace

SteadyMs NowMs() {
    using namespace std::chrono;
    return static_cast<SteadyMs>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

Session::Session() = default;
Session::~Session() { Stop(); }

bool Session::connected() const {
    if (role_ == Role::Host) return true;
    if (role_ == Role::Client) { auto it = peers_.find(0); return localSlot_ != 0 && it != peers_.end(); }
    return false;
}

uint32_t Session::rttMs(uint8_t slot) const {
    auto it = peers_.find(slot);
    return it == peers_.end() ? 0 : it->second.rtt;
}

bool Session::StartHost(const std::string& name, uint16_t port, uint8_t maxPlayers, bool enableDiscovery) {
    Stop();
    sock_ = MakeUdp(port, false, false);
    if (sock_ == kBad) return false;
    if (enableDiscovery) discoSock_ = MakeUdp(kDiscoveryPort, true, true);
    std::random_device rd;
    sessionId_ = rd() | 1u;
    sessionName_ = name;
    maxPlayers_ = maxPlayers < 2 ? 2 : (maxPlayers > 4 ? 4 : maxPlayers);
    gamePort_ = port;
    localSlot_ = 0;
    role_ = Role::Host;
    return true;
}

bool Session::StartHostViaRelay(const std::string& name, const std::string& relayAddr, uint16_t relayPort,
                                uint8_t maxPlayers, const std::string& secret) {
    if (!StartHost(name, 0, maxPlayers, false)) return false;
    in_addr a{};
    if (inet_pton(AF_INET, relayAddr.c_str(), &a) != 1) { Stop(); return false; }
    relayIp_ = a.s_addr; relayPort_ = htons(relayPort);
    relaySecret_ = secret;
    relayRegistered_ = false;
    lastRelayRegister_ = 0;
    return true;
}

bool Session::StartClient(const std::string& hostAddr, uint16_t port, const Hello& hello) {
    Stop();
    sock_ = MakeUdp(0, false, false);
    if (sock_ == kBad) return false;
    in_addr a{};
    if (inet_pton(AF_INET, hostAddr.c_str(), &a) != 1) { Stop(); return false; }
    Peer host; host.ip = a.s_addr; host.port = htons(port); host.slot = 0; host.lastHeard = NowMs();
    hostIp_ = host.ip; hostPort_ = host.port;
    peers_[0] = host;
    pendingHello_ = hello;
    lastHelloSend_ = 0;
    localSlot_ = 0;
    sessionId_ = 0;
    role_ = Role::Client;
    return true;
}

void Session::Stop() {
    if (role_ != Role::None && sock_ != kBad) {
        for (auto& [slot, p] : peers_) sendTo(p, Packet(MsgType::Bye, 0, 0, sessionId_, {}));
    }
    if (sock_ != kBad) closesocket(static_cast<SOCKET>(sock_));
    if (discoSock_ != kBad) closesocket(static_cast<SOCKET>(discoSock_));
    sock_ = discoSock_ = kBad;
    peers_.clear();
    role_ = Role::None;
    localSlot_ = 0;
    relayIp_ = 0; relayPort_ = 0; relayRegistered_ = false;
}

void Session::sendTo(const Peer& p, const std::vector<uint8_t>& bytes) {
    if (relayIp_ && p.ip == relayIp_ && IsRelayVirtualPort(p.port)) {   // a partner behind the relay
        auto f = RelayFrame(kRelayData, static_cast<uint8_t>(p.port & 0xFF), bytes.data(), bytes.size());
        sockaddr_in r{}; r.sin_family = AF_INET; r.sin_addr.s_addr = relayIp_; r.sin_port = relayPort_;
        sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0,
               reinterpret_cast<sockaddr*>(&r), sizeof(r));
        return;
    }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = p.ip; a.sin_port = p.port;
    sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
           reinterpret_cast<sockaddr*>(&a), sizeof(a));
}

void Session::sendUnreliable(Peer& p, MsgType t, const std::vector<uint8_t>& body) {
    sendTo(p, Packet(t, 0, 0, sessionId_, body));
}

void Session::sendReliable(Peer& p, MsgType t, const std::vector<uint8_t>& body) {
    uint16_t seq = p.nextSeq++;
    if (p.nextSeq == 0) p.nextSeq = 1;
    Pending pend; pend.bytes = Packet(t, kFlagReliable, seq, sessionId_, body); pend.lastSend = NowMs(); pend.tries = 1;
    sendTo(p, pend.bytes);
    p.outbox[seq] = std::move(pend);
}

Session::Peer* Session::findPeer(uint32_t ip, uint16_t port) {
    for (auto& [slot, p] : peers_) if (p.ip == ip && p.port == port) return &p;
    return nullptr;
}

Session::Peer* Session::peerBySlot(uint8_t slot) {
    auto it = peers_.find(slot);
    return it == peers_.end() ? nullptr : &it->second;
}

void Session::dropPeer(uint8_t slot) {
    if (!peers_.erase(slot)) return;
    if (cb_.onPeerLeft) cb_.onPeerLeft(slot);
    if (role_ == Role::Client && slot == 0 && hostIp_) {
        // The host left or restarted: keep knocking (Hello) until a host there welcomes us
        // again, so a partner needn't reload the mod after the host restarts the game.
        Peer host; host.ip = hostIp_; host.port = hostPort_; host.slot = 0; host.lastHeard = NowMs();
        peers_[0] = host;
        localSlot_ = 0;
        sessionId_ = 0;
        lastHelloSend_ = 0;
        return;
    }
    if (role_ == Role::Client) { role_ = Role::None; localSlot_ = 0; }
}

Announce Session::makeAnnounce() const {
    Announce a; a.sessionName = sessionName_; a.players = static_cast<uint8_t>(1 + peers_.size());
    a.maxPlayers = maxPlayers_; a.gamePort = gamePort_; a.chapter = chapter_;
    return a;
}

void Session::Poll() {
    if (role_ == Role::None) return;
    SteadyMs now = NowMs();

    if (role_ == Role::Host && relayIp_ && now - lastRelayRegister_ >= 2000) {   // register + keepalive
        auto f = RelaySecretFrame(kRelayRegister, relaySecret_);
        sockaddr_in r{}; r.sin_family = AF_INET; r.sin_addr.s_addr = relayIp_; r.sin_port = relayPort_;
        sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0, reinterpret_cast<sockaddr*>(&r), sizeof(r));
        lastRelayRegister_ = now;
    }
    if (role_ == Role::Client && localSlot_ == 0 && now - lastHelloSend_ >= kHelloEveryMs) {
        if (auto* host = peerBySlot(0)) {
            if (!relaySecret_.empty()) sendTo(*host, RelaySecretFrame(kRelayJoin, relaySecret_));   // the relay lets us through
            sendUnreliable(*host, MsgType::Hello, Body(pendingHello_));
        }
        lastHelloSend_ = now;
    }

    uint8_t buf[kMaxPacket + 64];
    for (;;) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        if (IsRelayFrame(buf, static_cast<size_t>(n))) {
            bool fromRelay = relayIp_ && from.sin_addr.s_addr == relayIp_ && from.sin_port == relayPort_;
            if (fromRelay && buf[2] == kRelayRegistered) relayRegistered_ = true;
            else if (fromRelay && buf[2] == kRelayData && buf[3] != 0)
                handlePacket(relayIp_, RelayVirtualPort(buf[3]), buf + kRelayHeader, static_cast<size_t>(n) - kRelayHeader);
            continue;   // join acks and anything else from a relay carry no coop message
        }
        handlePacket(from.sin_addr.s_addr, from.sin_port, buf, static_cast<size_t>(n));
        if (role_ == Role::None) return;
    }
    if (discoSock_ != kBad) handleDiscovery();
    now = NowMs();   // packets above may have stamped lastHeard after the first read

    std::vector<uint8_t> dead;
    for (auto& [slot, p] : peers_) {
        bool waitingForWelcome = role_ == Role::Client && localSlot_ == 0;
        if (!waitingForWelcome && now > p.lastHeard && now - p.lastHeard > kPeerTimeoutMs) { dead.push_back(slot); continue; }
        if (now - p.lastPing >= kPingEveryMs && !waitingForWelcome) {
            Writer w; w.u32(static_cast<uint32_t>(now));
            sendUnreliable(p, MsgType::Ping, w.data());
            p.lastPing = now;
        }
        SteadyMs resendAfter = p.rtt ? (p.rtt * 3 / 2 + 30) : 150;
        for (auto& [seq, pend] : p.outbox) {
            if (now - pend.lastSend < resendAfter) continue;
            if (++pend.tries > kMaxResends) { dead.push_back(slot); break; }
            sendTo(p, pend.bytes);
            pend.lastSend = now;
        }
    }
    for (uint8_t s : dead) dropPeer(s);
}

void Session::handleDiscovery() {
    uint8_t buf[256];
    for (;;) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(static_cast<SOCKET>(discoSock_), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        Reader r(buf, static_cast<size_t>(n));
        Header h;
        if (!Read(r, h) || h.type != MsgType::Discover) continue;
        auto reply = Packet(MsgType::Announce, 0, 0, 0, Body(makeAnnounce()));
        sendto(static_cast<SOCKET>(discoSock_), reinterpret_cast<const char*>(reply.data()), static_cast<int>(reply.size()), 0,
               reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
}

void Session::handlePacket(uint32_t ip, uint16_t port, const uint8_t* data, size_t len) {
    Reader r(data, len);
    Header h;
    if (!Read(r, h)) return;
    SteadyMs now = NowMs();

    Peer* p = findPeer(ip, port);

    // --- join handshake -----------------------------------------------------
    if (role_ == Role::Host && h.type == MsgType::Hello) {
        Hello hello;
        if (!Read(r, hello)) return;
        if (p) return; // already joined; the reliable Welcome is still being resent
        if (1 + peers_.size() >= maxPlayers_) {
            Writer w; w.str("Session is full");
            Peer tmp; tmp.ip = ip; tmp.port = port;
            sendTo(tmp, Packet(MsgType::Reject, 0, 0, sessionId_, w.data()));
            return;
        }
        uint8_t slot = 1;
        while (peers_.count(slot)) ++slot;
        Peer np; np.ip = ip; np.port = port; np.slot = slot; np.lastHeard = now; np.welcomed = true;
        peers_[slot] = np;
        Welcome wel; wel.slot = slot; wel.character = hello.wanted; wel.sessionId = sessionId_;
        sendReliable(peers_[slot], MsgType::Welcome, Body(wel));
        if (cb_.onPeerJoined) cb_.onPeerJoined(slot, hello);
        return;
    }
    if (!p) return;
    if (role_ == Role::Client && h.type == MsgType::Reject) {
        std::string reason = r.str();
        if (cb_.onRejected) cb_.onRejected(reason);
        Stop();
        return;
    }
    bool sidOk = h.sessionId == sessionId_ || (role_ == Role::Client && h.type == MsgType::Welcome);
    if (!sidOk) return;
    p->lastHeard = now;

    // --- reliable bookkeeping -------------------------------------------------
    if (h.type == MsgType::Ack) {
        uint16_t seq = r.u16();
        if (r.ok()) p->outbox.erase(seq);
        return;
    }
    if (h.flags & kFlagReliable) {
        Writer ack; ack.u16(h.seq);
        sendUnreliable(*p, MsgType::Ack, ack.data());
        for (uint16_t s : p->seenSeqs) if (s == h.seq) return; // duplicate
        p->seenSeqs.push_back(h.seq);
        if (p->seenSeqs.size() > 256) p->seenSeqs.pop_front();
    }

    uint8_t fromSlot = p->slot;
    switch (h.type) {
    case MsgType::Welcome: {
        Welcome w;
        if (!Read(r, w) || role_ != Role::Client) return;
        if (localSlot_ != 0) return;
        localSlot_ = w.slot;
        sessionId_ = w.sessionId;
        if (cb_.onWelcome) cb_.onWelcome(w);
        break;
    }
    case MsgType::Ping: {
        uint32_t t = r.u32();
        Writer w; w.u32(t);
        sendUnreliable(*p, MsgType::Pong, w.data());
        break;
    }
    case MsgType::Pong: {
        uint32_t t = r.u32();
        if (r.ok()) p->rtt = static_cast<uint32_t>(static_cast<uint32_t>(now) - t);
        break;
    }
    case MsgType::Bye:
        dropPeer(fromSlot);
        break;
    case MsgType::PlayerState: {
        uint8_t slot = r.u8();
        PlayerState s;
        if (!Read(r, s)) return;
        if (role_ == Role::Host) {
            slot = fromSlot;
            Writer w; w.u8(slot); Write(w, s);
            for (auto& [os, op] : peers_) if (os != fromSlot) sendUnreliable(op, MsgType::PlayerState, w.data());
        }
        if (cb_.onPlayerState) cb_.onPlayerState(slot, s);
        break;
    }
    case MsgType::Input: {
        InputFrame in;
        if (role_ == Role::Host && Read(r, in) && cb_.onInput) cb_.onInput(fromSlot, in);
        break;
    }
    case MsgType::CheckpointWritten: {
        CheckpointEvent e;
        if (role_ == Role::Client && Read(r, e) && cb_.onCheckpointWritten) cb_.onCheckpointWritten(e);
        break;
    }
    case MsgType::CheckpointRestart: {
        RestartEvent e;
        if (Read(r, e) && cb_.onCheckpointRestart) cb_.onCheckpointRestart(fromSlot, e);
        break;
    }
    case MsgType::LogText: {
        uint16_t n = r.u16();
        std::string text;
        if (r.ok() && n <= r.remaining()) { text.resize(n); for (auto& c : text) c = static_cast<char>(r.u8()); }
        if (r.ok() && role_ == Role::Host && cb_.onLogText) cb_.onLogText(fromSlot, text);
        break;
    }
    case MsgType::Paused: {
        PauseEvent e;
        if (Read(r, e) && cb_.onPaused) cb_.onPaused(fromSlot, e);
        break;
    }
    case MsgType::EnemyStates: {
        std::vector<EnemyState> v;
        if (role_ == Role::Client && Read(r, v) && cb_.onEnemyStates) cb_.onEnemyStates(v);
        break;
    }
    case MsgType::EnemyDamage: {
        EnemyDamage d;
        if (role_ == Role::Host && Read(r, d) && cb_.onEnemyDamage) cb_.onEnemyDamage(fromSlot, d);
        break;
    }
    case MsgType::Objective: {
        Objective o;
        if (role_ == Role::Client && Read(r, o) && cb_.onObjective) cb_.onObjective(o);
        break;
    }
    case MsgType::TeleportRequest:
        if (role_ == Role::Host && cb_.onTeleportRequest) cb_.onTeleportRequest(fromSlot);
        break;
    case MsgType::TeleportTo: {
        TeleportTo t;
        if (role_ == Role::Client && Read(r, t) && cb_.onTeleportTo) cb_.onTeleportTo(t);
        break;
    }
    case MsgType::CharacterSelect: {
        uint8_t slot = r.u8();
        uint8_t c = r.u8();
        if (!r.ok() || c >= static_cast<uint8_t>(CharacterId::Count)) return;
        if (role_ == Role::Host) {
            slot = fromSlot;
            Writer w; w.u8(slot); w.u8(c);
            for (auto& [os, op] : peers_) if (os != fromSlot) sendReliable(op, MsgType::CharacterSelect, w.data());
        }
        if (cb_.onCharacterSelect) cb_.onCharacterSelect(slot, static_cast<CharacterId>(c));
        break;
    }
    case MsgType::WorldEvent: {
        uint8_t slot = r.u8();
        WorldEvent e;
        if (!Read(r, e)) return;
        if (role_ == Role::Host) {
            slot = fromSlot;
            Writer w; w.u8(slot); Write(w, e);
            for (auto& [os, op] : peers_) if (os != fromSlot) sendReliable(op, MsgType::WorldEvent, w.data());
        }
        if (cb_.onWorldEvent) cb_.onWorldEvent(slot, e);
        break;
    }
    default:
        break;
    }
}

void Session::SendPlayerState(const PlayerState& s) {
    Writer w; w.u8(localSlot_); Write(w, s);
    for (auto& [slot, p] : peers_) sendUnreliable(p, MsgType::PlayerState, w.data());
}

void Session::SendInput(const InputFrame& in) {
    if (role_ != Role::Client || !connected()) return;
    sendUnreliable(peers_[0], MsgType::Input, Body(in));
}

void Session::SendEnemyStates(const std::vector<EnemyState>& v) {
    if (role_ != Role::Host) return;
    for (size_t i = 0; i < v.size(); i += kEnemiesPerPacket) {
        std::vector<EnemyState> part(v.begin() + i, v.begin() + (std::min)(v.size(), i + kEnemiesPerPacket));
        auto b = Body(part);
        for (auto& [slot, p] : peers_) sendUnreliable(p, MsgType::EnemyStates, b);
    }
}

void Session::SendCheckpointWritten(const CheckpointEvent& e) {
    if (role_ != Role::Host) return;
    auto b = Body(e);
    for (auto& [slot, p] : peers_) sendReliable(p, MsgType::CheckpointWritten, b);
}

void Session::SendCheckpointRestart(const RestartEvent& e) {
    auto b = Body(e);
    if (role_ == Role::Host) { for (auto& [slot, p] : peers_) sendReliable(p, MsgType::CheckpointRestart, b); }
    else if (connected()) sendReliable(peers_[0], MsgType::CheckpointRestart, b);
}

void Session::SendLogText(const std::string& text) {
    if (role_ != Role::Client || !connected()) return;
    std::string t = text.substr(0, 1000);
    Writer w; w.u16(static_cast<uint16_t>(t.size())); w.raw(t.data(), t.size());
    sendReliable(peers_[0], MsgType::LogText, w.data());
}

void Session::SendPaused(const PauseEvent& e) {
    auto b = Body(e);
    if (role_ == Role::Host) { for (auto& [slot, p] : peers_) sendReliable(p, MsgType::Paused, b); }
    else if (connected()) sendReliable(peers_[0], MsgType::Paused, b);
}

void Session::SendEnemyDamage(const EnemyDamage& d) {
    if (role_ != Role::Client || !connected()) return;
    sendReliable(peers_[0], MsgType::EnemyDamage, Body(d));
}

void Session::SendObjective(const Objective& o) {
    if (role_ != Role::Host) return;
    auto b = Body(o);
    for (auto& [slot, p] : peers_) sendReliable(p, MsgType::Objective, b);
}

void Session::SendTeleportRequest() {
    if (role_ != Role::Client || !connected()) return;
    sendReliable(peers_[0], MsgType::TeleportRequest, {});
}

void Session::SendTeleportTo(uint8_t slot, const TeleportTo& t) {
    if (role_ != Role::Host) return;
    if (auto* p = peerBySlot(slot)) sendReliable(*p, MsgType::TeleportTo, Body(t));
}

void Session::SendCharacterSelect(CharacterId c) {
    Writer w; w.u8(localSlot_); w.u8(static_cast<uint8_t>(c));
    for (auto& [slot, p] : peers_) sendReliable(p, MsgType::CharacterSelect, w.data());
}

void Session::SendWorldEvent(const WorldEvent& e) {
    Writer w; w.u8(localSlot_); Write(w, e);
    for (auto& [slot, p] : peers_) sendReliable(p, MsgType::WorldEvent, w.data());
}

std::vector<FoundHost> Session::Discover(uint32_t timeoutMs, uint16_t discoveryPort) {
    std::vector<FoundHost> found;
    SOCKET s = MakeUdp(0, false, true);
    if (s == kBad) return found;
    auto pkt = Packet(MsgType::Discover, 0, 0, 0, {});
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(discoveryPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    // Loopback too, so a host on this same PC is always found.
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));

    std::set<std::string> seen;
    SteadyMs end = NowMs() + timeoutMs;
    uint8_t buf[512];
    while (NowMs() < end) {
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
        timeval tv{0, 20000};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        Reader r(buf, static_cast<size_t>(n));
        Header h; Announce a;
        if (!Read(r, h) || h.type != MsgType::Announce || !Read(r, a)) continue;
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        std::string key = std::string(ip) + ":" + std::to_string(a.gamePort);
        if (!seen.insert(key).second) continue;
        found.push_back({ip, a});
    }
    closesocket(s);
    return found;
}

} // namespace coop
