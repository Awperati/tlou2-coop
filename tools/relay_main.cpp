// coop_relay.exe: M8 dedicated relay (see core/relay.h and ROADMAP "M8 dedicated server").
//   coop_relay <secret> [port] [packets per second per sender]
// The host game registers with the secret; partners join with the same secret. Prints stats every 10 s.
#include "core/relay.h"
#include "core/session.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <windows.h>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: coop_relay <secret> [port %u] [rate 300]\n", coop::kRelayDefaultPort); return 1; }
    std::string secret = argv[1];
    if (secret.size() < 8) { std::printf("use a secret of 8+ characters\n"); return 1; }
    uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : coop::kRelayDefaultPort;
    uint32_t rate = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 300;
    coop::Relay relay;
    if (!relay.Start(port, secret, rate)) { std::printf("cannot bind UDP %u\n", port); return 1; }
    std::printf("relay on UDP %u\n", port);
    uint64_t lastPrint = 0;
    for (;;) {
        relay.Poll();
        uint64_t now = coop::NowMs();
        if (now - lastPrint > 10000) {
            lastPrint = now;
            const auto& s = relay.stats();
            std::printf("host %s, partners %zu, forwarded %llu, dropped: secret %llu oversize %llu rate %llu other %llu\n",
                        relay.hasHost() ? "yes" : "no", relay.partners(), static_cast<unsigned long long>(s.forwarded),
                        static_cast<unsigned long long>(s.droppedSecret), static_cast<unsigned long long>(s.droppedOversize),
                        static_cast<unsigned long long>(s.droppedRate), static_cast<unsigned long long>(s.droppedUnknown));
            std::fflush(stdout);
        }
        Sleep(1);
    }
}
