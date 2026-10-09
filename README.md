# TLOU2 Co-op

LAN co-op for *The Last of Us Part II Remastered* (PC). Two players play the story at once.
Player 2 takes the chapter's companion: Dina, Lev, Jesse, and so on.
Fall too far behind and you **hold T** (or Back on a controller) to teleport to your partner, Dying Light style.

> **Status: early.** The network layer, teleport rules, roster, overlay and loader work and are tested.
> The game link is not mapped yet, so in game the mod connects and shows prompts but cannot move characters.
> See [docs/ROADMAP.md](docs/ROADMAP.md).

## Install

1. Build (below), or take `version.dll` from a release.
2. Copy `build\version.dll` into the game folder, next to `tlou-ii.exe`.
3. Copy the `config` folder there too and rename it `tlou2-coop`.
4. Host: set `Mode = host` in `tlou2-coop\coop.ini`. Partner: `Mode = client`.
5. Play in **borderless or windowed** mode so the prompt overlay shows.

Uninstall: delete `version.dll` and the `tlou2-coop` folder.

## Settings ([config/coop.ini](config/coop.ini))

| Setting | What it does |
|---|---|
| `HostAddress` | Leave empty to find the host on the LAN automatically |
| `Character` | Who you play when the chapter has no story companion (`random` allowed) |
| `HoldKey`, `HoldPad` | Teleport key and controller button |
| `HoldSeconds`, `FarDistance` | How long to hold, and how far before the prompt shows |
| `PressInsteadOfHold` | One press instead of a hold |
| `SpeakPrompts`, `SoundCues` | Spoken prompts and sound cues |

Chapter-to-companion mapping and character loadouts are in [config/roster.ini](config/roster.ini).

## Build

Needs Visual Studio 2022 Build Tools with C++.

```bat
build.cmd
```

This builds `build\version.dll`, runs the unit tests, and builds `coop_lan_sim.exe`.

## Test the network without the game

```bat
build\coop_lan_sim.exe host
build\coop_lan_sim.exe client
```

Run them on one PC or two on the same LAN. The client wanders off, holds teleport, and lands behind the host.

## Layout

| Path | Contents |
|---|---|
| `src/core` | Game-free code: protocol, UDP session, teleport, roster, INI |
| `src/mod` | The DLL: loader proxy, game bridge, overlay, main loop |
| `config` | `coop.ini`, `roster.ini`, `offsets.ini` |
| `tests`, `tools` | Unit tests, LAN simulator, IDA scripts |

No game files are in this repo. You need your own copy of the game.
