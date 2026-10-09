cmake_minimum_required(VERSION 3.20)
project(tlou2_coop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Game-independent core: protocol, sessions, teleport rules, roster.
add_library(coop_core STATIC
    src/core/protocol.cpp
    src/core/session.cpp
    src/core/teleport.cpp
    src/core/roster.cpp
    src/core/config.cpp
)
target_include_directories(coop_core PUBLIC src)
target_link_libraries(coop_core PUBLIC ws2_32)

# coop.dll: the co-op runtime, hot-reloaded by the loader.
add_library(coop_mod SHARED
    src/mod/dllmain.cpp
    src/mod/log.cpp
    src/mod/game_bridge.cpp
    src/mod/sigscan.cpp
    src/mod/overlay.cpp
    src/mod/main_thread.cpp
)
set_target_properties(coop_mod PROPERTIES OUTPUT_NAME "coop")
target_compile_definitions(coop_mod PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(coop_mod PRIVATE coop_core ole32 user32 gdi32 sapi)

# version.dll: proxy the game loads from its folder; loads coop.dll.
add_library(coop_loader SHARED
    src/loader/loader.cpp
    src/loader/version_proxy.cpp
)
set_target_properties(coop_loader PROPERTIES OUTPUT_NAME "version")
target_compile_definitions(coop_loader PRIVATE UNICODE _UNICODE NOMINMAX)

enable_testing()
add_executable(coop_tests tests/tests.cpp)
target_link_libraries(coop_tests PRIVATE coop_core)
add_test(NAME coop_tests COMMAND coop_tests)

# Two-process LAN smoke test without the game: host + client on loopback.
add_executable(coop_lan_sim tools/lan_sim.cpp)
target_link_libraries(coop_lan_sim PRIVATE coop_core)
