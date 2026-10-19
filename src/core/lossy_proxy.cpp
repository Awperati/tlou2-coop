#include "core/lossy_proxy.h"
#include "core/session.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <mutex>

namespace coop {

bool LossyProxy::Start(uint16_t listenPort, const std::string& targetAddr, uint16_t targetPort,
                       int dropPct, int reorderPct, uint32_t delayMs, uint32_t seed) {
    Stop();
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL off = FALSE; DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(listenPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closesocket(s); return false; }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    in_addr t{};
    if (inet_pton(AF_INET, targetAddr.c_str(), &t) != 1) { closesocket(s); return false; }
    sock_ = static_cast<uintptr_t>(s);
    targetIp_ = t.s_addr; targetPort_ = htons(targetPort);
    clientIp_ = 0; clientPort_ = 0;
    drop_ = dropPct; reorder_ = reorderPct; delayMs_ = delayMs; seed_ = seed ? seed : 1;
    blackout_ = false; held_.clear(); stats_ = {};
    return true;
}

void LossyProxy::Stop() {
    if (sock_ != ~uintptr_t(0)) { closesocket(static_cast<SOCKET>(sock_)); sock_ = ~uintptr_t(0); }
    held_.clear();
}

void LossyProxy::Send(bool toTarget, const std::vector<uint8_t>& b) {
    sockaddr_in to{}; to.sin_family = AF_INET;
    to.sin_addr.s_addr = toTarget ? targetIp_ : clientIp_;
    to.sin_port = toTarget ? targetPort_ : clientPort_;
    if (!to.sin_port) return;
    sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    ++stats_.forwarded;
}

void LossyProxy::Poll() {
    if (sock_ == ~uintptr_t(0)) return;
    uint64_t now = NowMs();
    for (;;) {
        uint8_t buf[2048];
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(static_cast<SOCKET>(sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        ++stats_.seen;
        bool fromTarget = from.sin_addr.s_addr == targetIp_ && from.sin_port == targetPort_;
        if (!fromTarget && (from.sin_addr.s_addr != clientIp_ || from.sin_port != clientPort_)) {
            clientIp_ = from.sin_addr.s_addr; clientPort_ = from.sin_port;   // the (latest) client
        }
        if (blackout_ || static_cast<int>(Rand() % 100) < drop_) { ++stats_.dropped; continue; }
        std::vector<uint8_t> b(buf, buf + n);
        if (static_cast<int>(Rand() % 100) < reorder_) { held_.push_back({now + delayMs_, !fromTarget, std::move(b)}); ++stats_.delayed; continue; }
        Send(!fromTarget, b);
    }
    while (!held_.empty() && held_.front().due <= now) { Send(held_.front().toTarget, held_.front().bytes); held_.pop_front(); }
}

} // namespace coop
