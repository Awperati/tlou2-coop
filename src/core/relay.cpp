#include "core/relay.h"
#include "core/session.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <algorithm>
#include <mutex>

namespace coop {

namespace {
constexpr uint64_t kPartnerTimeoutMs = 30000;
constexpr uint64_t kHostTimeoutMs = 30000;

bool SecretMatches(const uint8_t* d, size_t n, const std::string& secret) {
    if (n < kRelayHeader + 1) return false;
    size_t len = d[kRelayHeader];
    if (n != kRelayHeader + 1 + len || len != secret.size()) return false;
    return std::equal(secret.begin(), secret.end(), d + kRelayHeader + 1);
}
} // namespace

bool IsRelayFrame(const uint8_t* d, size_t n) { return n >= kRelayHeader && d[0] == 'R' && d[1] == 'L'; }

std::vector<uint8_t> RelayFrame(uint8_t kind, uint8_t tag, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> f = {'R', 'L', kind, tag};
    f.insert(f.end(), payload, payload + n);
    return f;
}

std::vector<uint8_t> RelaySecretFrame(uint8_t kind, const std::string& secret) {
    std::vector<uint8_t> f = {'R', 'L', kind, 0, static_cast<uint8_t>(std::min<size_t>(secret.size(), 255))};
    f.insert(f.end(), secret.begin(), secret.begin() + f[4]);
    return f;
}

bool RelayTestSend(uint16_t port, const std::vector<uint8_t>& bytes, int count) {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bool ok = true;
    for (int i = 0; i < count; ++i)
        ok &= sendto(s, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == static_cast<int>(bytes.size());
    closesocket(s);
    return ok;
}

bool Relay::Start(uint16_t port, const std::string& secret, uint32_t ratePerSec) {
    Stop();
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL off = FALSE; DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closesocket(s); return false; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    sock_ = static_cast<uintptr_t>(s);
    secret_ = secret;
    rate_ = ratePerSec ? ratePerSec : 1;
    return true;
}

void Relay::Stop() {
    if (sock_ != ~uintptr_t(0)) closesocket(static_cast<SOCKET>(sock_));
    sock_ = ~uintptr_t(0);
    hostIp_ = 0; hostPort_ = 0;
    partners_.clear(); byTag_.clear(); buckets_.clear();
}

bool Relay::allow(const Addr& a, uint64_t now) {
    Bucket& b = buckets_[a];
    double cap = 2.0 * rate_;
    if (b.last == 0) b.tokens = cap;
    else b.tokens = std::min(cap, b.tokens + (now - b.last) * rate_ / 1000.0);
    b.last = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

void Relay::send(const Addr& to, const uint8_t* d, size_t n) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = to.ip; a.sin_port = to.port;
    sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(d), static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
}
void Relay::send(const Addr& to, const std::vector<uint8_t>& bytes) { send(to, bytes.data(), bytes.size()); }

void Relay::Poll() {
    if (sock_ == ~uintptr_t(0)) return;
    uint64_t now = NowMs();
    uint8_t buf[kRelayMaxFrame + 512];
    for (;;) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        Addr src{from.sin_addr.s_addr, from.sin_port};
        if (static_cast<size_t>(n) > kRelayMaxFrame) { ++stats_.droppedOversize; continue; }
        if (!allow(src, now)) { ++stats_.droppedRate; continue; }
        bool fromHost = hostIp_ && src.ip == hostIp_ && src.port == hostPort_;
        if (IsRelayFrame(buf, n)) {
            uint8_t kind = buf[2], tag = buf[3];
            if (kind == kRelayRegister) {
                if (!SecretMatches(buf, n, secret_)) { ++stats_.droppedSecret; continue; }
                if (!fromHost) { hostIp_ = src.ip; hostPort_ = src.port; }   // first host, or the host restarted
                hostHeard_ = now;
                send(src, RelayFrame(kRelayRegistered, 0, nullptr, 0));
            } else if (kind == kRelayJoin) {
                if (!SecretMatches(buf, n, secret_)) { ++stats_.droppedSecret; continue; }
                auto it = partners_.find(src);
                if (it == partners_.end()) {
                    uint8_t t = 1;
                    while (t < 255 && byTag_.count(t)) ++t;
                    if (t == 255) { ++stats_.droppedUnknown; continue; }
                    it = partners_.emplace(src, Partner{t, now}).first;
                    byTag_[t] = src;
                }
                it->second.lastHeard = now;
                send(src, RelayFrame(kRelayJoined, it->second.tag, nullptr, 0));
            } else if (kind == kRelayData && fromHost) {
                auto t = byTag_.find(tag);
                if (t == byTag_.end()) { ++stats_.droppedUnknown; continue; }
                hostHeard_ = now;
                send(t->second, buf + kRelayHeader, n - kRelayHeader);
                ++stats_.forwarded;
            } else {
                ++stats_.droppedUnknown;
            }
            continue;
        }
        // A raw coop packet: only from a partner that joined with the secret, and only with a host.
        auto it = partners_.find(src);
        if (it == partners_.end()) { ++stats_.droppedSecret; continue; }
        if (!hostIp_) { ++stats_.droppedUnknown; continue; }
        it->second.lastHeard = now;
        send(Addr{hostIp_, hostPort_}, RelayFrame(kRelayData, it->second.tag, buf, n));
        ++stats_.forwarded;
    }
    if (now - lastSweep_ > 1000) {
        lastSweep_ = now;
        for (auto it = partners_.begin(); it != partners_.end();) {
            if (now - it->second.lastHeard > kPartnerTimeoutMs) { byTag_.erase(it->second.tag); it = partners_.erase(it); }
            else ++it;
        }
        if (hostIp_ && now - hostHeard_ > kHostTimeoutMs) { hostIp_ = 0; hostPort_ = 0; }
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            if (now - it->second.last > 60000) it = buckets_.erase(it); else ++it;
        }
    }
}

} // namespace coop
