# snes-online

Cross-platform C++ core for a SNES emulator with Libretro core hosting and optional GGPO rollback netcode.

## Layout
- `include/snesonline/*`: public headers
- `src/*`: core implementation
- `platform/android`: Android NDK JNI bridge
- `platform/ios`: iOS Objective-C++ bridge

## Build (desktop skeleton)
Prereqs (Windows):
- CMake
- MSVC toolchain (Visual Studio 2022 Build Tools with "Desktop development with C++")

If `cmake` isn't on PATH, you can invoke it directly as `C:\Program Files\CMake\bin\cmake.exe`.

```bash
cmake -S . -B build
cmake --build build
```

Recommended Windows build (Visual Studio generator):
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build_vs -G "Visual Studio 17 2022" -A x64
& "C:\Program Files\CMake\bin\cmake.exe" --build build_vs -j
```

## Optional GGPO
Configure with:
```bash
cmake -S . -B build -DSNESONLINE_ENABLE_GGPO=ON
```

By default, enabling `SNESONLINE_ENABLE_GGPO` will fetch and build the recommended GGPO implementation (`pond3r/ggpo`, MIT license) via CMake FetchContent.

Advanced: if you want to use a different GGPO fork or a prebuilt GGPO library, configure with `-DSNESONLINE_FETCH_GGPO=OFF` and provide `ggponet.h` + the GGPO library via your toolchain.

Note: [src/NetplaySession.cpp](src/NetplaySession.cpp) uses a typical GGPO API flow, but you still need to wire real remote IP/port and your transport/session creation details.

## Libretro core
The core is loaded dynamically by `LibretroCore` (symbol-based). Provide your core binary (e.g., Snes9x/bsnes Libretro) and call `EmulatorEngine::instance().initialize(corePath, romPath)` from your platform layer.

## Android
`platform/android/native-lib.cpp` exposes JNI APIs to feed input (axis/key) and run a native 60fps loop.

## iOS
`platform/ios/*` contains an Objective-C++ poller using `GameController.framework` that runs off the main thread.

## Windows (SDL2 runner)
This repo includes an optional Windows desktop runner that opens a window, plays audio, and supports keyboard + game controller input via SDL2.

Configuration window:
- Run with `--config` to edit settings and exit, or press `F1` while the app is running.
- Settings are saved to `%APPDATA%\snes-online\config.ini`.

Separate config tool (Windows):
- Build produces `snesonline_config.exe`, which opens the configuration window without starting the emulator.

Configure/build:
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DSNESONLINE_BUILD_WINDOWS_APP=ON
& "C:\Program Files\CMake\bin\cmake.exe" --build build_vs -j
```

Build Release:
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build_vs --config Release -j
```

Run (from the repo root):
```powershell
.\build_vs\platform\windows\Debug\snesonline_win.exe --core .\cores\snes9x_libretro.dll --rom C:\path\to\game.sfc
```

Get the Snes9x Libretro core DLL (Windows x86_64) into `cores/`:
```powershell
.\scripts\get_snes9x_core.ps1
```

If PowerShell blocks scripts on your machine, run it with a process-scoped bypass (does not change system policy):
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
.\scripts\get_snes9x_core.ps1
```

Then run using:
```powershell
.\build_vs\platform\windows\Debug\snesonline_win.exe --core .\cores\snes9x_libretro.dll --rom C:\path\to\game.sfc
```

Release run (from the repo root):
```powershell
.\build_vs\platform\windows\Release\snesonline_win.exe --core .\cores\snes9x_libretro.dll --rom C:\path\to\game.sfc
```

## Netplay (Room Code, cross-platform)

This project supports **peer-to-peer UDP lockstep netplay** using a **Room Code** server for rendezvous.

Key points:
- Room Code netplay is **room-only** (no Direct IP UI).
- A **room password is required**.
- The connection is established **at game start** and only exists while the game is running.
- The **first device to start the game** becomes **Player 1** (host) and performs a UDP WHOAMI punch to learn its public port.
- The second device becomes **Player 2** and connects using the host endpoint returned by the room server.
- This is still P2P UDP (not a relay). Hard NAT / CGNAT may still require a VPN overlay.

LAN optimization:
- If both players are behind the same public IP (same NAT), the server may return a best-effort `room.localIp` so the client can prefer LAN routing.

### Windows (portable/desktop)
1) Open the configuration UI (`--config` or `F1`).
2) Set:
	- **Local UDP Port** (default 7000)
	- **Room Server URL** (default: `http://snesonline.freedynamicdns.net:8787`)
	- **Room Code** (8–12 letters/numbers)
	- **Room Password** (required)
3) Start the game on both devices. The first starter becomes Player 1 automatically.

### Android
1) In the config screen set Room Server URL, Room Code, Room Password, and Local UDP Port.
2) Tap **Start**. The app will connect to the room before gameplay begins.

### Internet play notes
- If you can’t connect (mobile networks / CGNAT), the usual fix is to use a VPN overlay like Tailscale/ZeroTier on both devices.
- Ensure your firewall allows inbound UDP on your **Local UDP Port**.

## Room server

The room server is a small Node.js HTTP+UDP service used for matchmaking.

Run locally (from the repo root):
```powershell
node .\tools\room_server\server.js --host 0.0.0.0 --port 8787
```

Endpoints:
- `GET /health`
- `POST /rooms/connect` (game-start create/join; password required)
- `GET /download/apk` and `GET /download/zip`

Security/behavior:
- The server stores the creator’s public IP (`room.ip`) from the HTTP request and the discovered public UDP port (WHOAMI).
- The server may accept a best-effort client LAN IPv4 (`room.localIp`) and only returns it to clients that share the same public IP.
- Player 1 receives a `creatorToken` and must provide it when finalizing the room after WHOAMI.
- The room password is hashed server-side.

## Netplay (GGPO) (experimental / desktop-only)
To enable netplay you must build with `-DSNESONLINE_ENABLE_GGPO=ON`.

By default, the build will fetch/build GGPO automatically (so `#include <ggponet.h>` works and GGPO symbols link).

GGPO netplay is not integrated with the Room Code server; it uses direct IP/port.

Once enabled, you can run two instances and point each at the other peer:

PC A:
```powershell
\.\build_vs\platform\windows\Debug\snesonline_win.exe --core .\cores\snes9x_libretro.dll --rom C:\path\to\game.sfc --netplay --player 1 --local-port 7000 --remote-ip <PC_B_IP> --remote-port 7000
```

PC B:
```powershell
\.\build_vs\platform\windows\Debug\snesonline_win.exe --core .\cores\snes9x_libretro.dll --rom C:\path\to\game.sfc --netplay --player 2 --local-port 7000 --remote-ip <PC_A_IP> --remote-port 7000
```

Notes:
- If you’re testing on the same machine, you must use two different `--local-port` values (e.g. 7000 and 7001) and point `--remote-port` at the other instance.
- Example (same PC):
	- Instance A: `--player 1 --local-port 7000 --remote-ip 127.0.0.1 --remote-port 7001`
	- Instance B: `--player 2 --local-port 7001 --remote-ip 127.0.0.1 --remote-port 7000`
- NAT/port-forwarding/firewall rules can block peer-to-peer sessions.

Internet play (outside your LAN):
- Easiest: use a VPN overlay like Tailscale/ZeroTier on both devices, then use the VPN IP as `--remote-ip`.
- Without VPN: you generally need to forward UDP `--local-port` on the host/router, and the other player uses the host’s public IP.
- Some ISPs/mobile connections use carrier-grade NAT and won’t allow port forwarding; VPN overlay is the usual workaround.
- You can use a hostname (e.g. dynamic DNS); snes-online resolves it to IPv4 automatically.

## Windows installer (NSIS)
You can generate a Windows installer `.exe` that lets the user pick an install folder and includes a `roms/` directory.

Prereqs:
- Install NSIS (so `makensis.exe` is available)

Build + package:
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build_vs -G "Visual Studio 17 2022" -A x64 -DSNESONLINE_BUILD_WINDOWS_APP=ON
& "C:\Program Files\CMake\bin\cmake.exe" --build build_vs --config Release -j
& "C:\Program Files\CMake\bin\cmake.exe" --install build_vs --config Release --prefix "${PWD}\build_vs\_install"

# Creates the installer in build_vs (typically build_vs\_CPack_Packages\... and/or build_vs\snes-online-*.exe)
Push-Location build_vs
cpack -G NSIS
Pop-Location
```

SDL2:
- If SDL2 isn't installed, CMake will fetch SDL2 automatically when `SNESONLINE_FETCH_SDL2=ON` (default).
