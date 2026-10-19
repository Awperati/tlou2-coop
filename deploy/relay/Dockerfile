# coop_relay for Linux (amd64 or arm64, e.g. a Raspberry Pi 5 on Debian). Build from the repo root:
#   docker build -f deploy/relay/Dockerfile -t tlou2-coop-relay .
#   docker buildx build --platform linux/arm64 -f deploy/relay/Dockerfile -t tlou2-coop-relay:arm64 .
# Only the relay is built: src/core/relay.cpp, tools/relay_main.cpp and a small time shim (the game-side
# session code is Windows-only). The relay holds no game state and forwards UDP between a host and partners.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src/core/relay.h src/core/relay.cpp src/core/session.h src/core/protocol.h src/core/clocksync.h src/core/
COPY tools/relay_main.cpp tools/relay_linux_shim.cpp tools/
RUN g++ -std=c++20 -O2 -Wall -Wextra -Isrc -o /coop_relay tools/relay_main.cpp src/core/relay.cpp tools/relay_linux_shim.cpp

FROM debian:bookworm-slim
COPY --from=build /coop_relay /usr/local/bin/coop_relay
RUN useradd --system --no-create-home relay
USER relay
EXPOSE 47830/udp
# RELAY_SECRET (8+ characters) comes from the env file; RELAY_RATE is packets per second per sender.
ENTRYPOINT ["/bin/sh", "-c", "exec coop_relay \"$RELAY_SECRET\" 47830 \"${RELAY_RATE:-300}\""]
