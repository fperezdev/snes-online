# snes-online (Portable)

This folder is a portable build of **snes-online**.

## 1) Configure (network/settings)

Run:
- `snesonline_config.exe`

This opens the configuration window and saves settings to:
- `%APPDATA%\snes-online\config.ini`

Important fields:
- **Local UDP Port**: your local UDP port (make sure your firewall allows it)
- **Role**: Host (Player 1) or Join (Player 2)
- **Remote IP/Port**: used when joining (Player 2)
- **Connection Code**: share/paste to connect

Netplay uses a **Connection Code** (Direct Connect via STUN):
- Host clicks **Get Connection Info** to generate a Connection Code (copied to clipboard).
- Join pastes it and clicks **Join From Code**.
- Start the game on both devices.

## 2) Put ROMs

Use the `roms/` folder inside this portable folder.

## 3) Run

Run:
- `snesonline_win.exe`

How to start a game:
- Double-click `snesonline_win.exe` and pick a ROM when prompted, or
- Drag-and-drop a ROM file onto `snesonline_win.exe`

Libretro core:
- The core DLL should be in `cores/` (for example `cores/snes9x_libretro.dll`).

Optional netplay:
- Start the game with `--netplay` (and optionally `--local-port` to override the saved settings).

Notes:
- This is still peer-to-peer UDP (not a relay). If you can’t connect due to NAT/CGNAT, use a VPN overlay (Tailscale/ZeroTier).
- Connection happens at game start and only exists while the game is running.
