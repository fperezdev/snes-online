snes-online (Portable)

This folder contains a portable build of snes-online.

1) Configure (network/settings)

Run:
  snesonline_config.exe

This opens the configuration window and saves settings to:
  %APPDATA%\snes-online\config.ini

Important fields:
- Local UDP Port: your local UDP port (make sure your firewall allows it)
- Role: Host (Player 1) or Join (Player 2)
- Remote IP/Port: used when joining (Player 2)
- Connection Code: share/paste to connect

How netplay works (Direct Connect / Connection Code):
- Netplay connects at game start and only exists while the game is running.
- Host clicks "Get Connection Info" to generate a Connection Code (copied to clipboard).
- Join pastes the code and clicks "Join From Code".

2) ROMs folder

Put your ROM files in:
  roms\

3) Run the emulator

Run:
  snesonline_win.exe

How to start a game:
- Double-click snesonline_win.exe and pick a ROM when prompted, or
- Drag-and-drop a ROM file onto snesonline_win.exe

Libretro core:
- The core DLL should be in:
    cores\
  (for example cores\snes9x_libretro.dll)

Optional netplay:
If "Enable netplay" is checked in the configuration, snesonline_win.exe will automatically start in netplay mode.

Netplay protocol note:
- Direct Connect netplay uses peer-to-peer UDP lockstep (cross-platform).
- A Windows-only GGPO (rollback) mode exists but is separate/experimental.

Command line overrides:
- You can still override the local UDP port with:
  --local-port <port>

Notes:
- Both players must run the same ROM and core.
- If you're testing on the same PC, use two different local ports (e.g. 7000 and 7001) and point each instance's remote port at the other.

==============================
Remote / Internet Netplay Guide
==============================

To play with someone outside your local network, you must solve NAT routing.
There are two reliable options:

A) Recommended: VPN overlay (Tailscale / ZeroTier)
  - Works even if port-forwarding is impossible (CGNAT, campus Wi‑Fi, mobile hotspot).
  - No router changes required.

B) Manual port-forwarding (router)
  - Works without VPN, but requires access to the host router.

Direct Connect note:
- This is not a relay. The actual game traffic is still peer-to-peer UDP.

----------------------------------------
A) Step-by-step: VPN overlay (recommended)
----------------------------------------

1) Install a VPN overlay on both PCs
  - Tailscale (easy) or ZeroTier (also common).

2) Join the same VPN network
  - Verify both devices appear online in the VPN UI.

3) Pick a UDP port
  - Choose a fixed "Local Port" on each PC.
  - Recommended: both use 7000 (or any unused UDP port).

4) Configure both devices using snesonline_config.exe
  - Enable netplay: ON
  - Local UDP Port: 7000
  - Host: click Get Connection Info and share the Connection Code
  - Join: paste the Connection Code and click Join From Code

6) Allow firewall prompts
  - On first run, Windows may ask to allow network access.
  - Allow private networks at minimum (VPN adapters usually count as private).

7) Launch on both PCs
  - Run snesonline_win.exe on both PCs.
  - Both must load the same ROM.

----------------------------------------
B) Step-by-step: Port-forwarding (no VPN)
----------------------------------------

Terminology:
- "Host": the PC that will forward a port on its router.
- "Client": the other PC that connects to the host.

Safety / best practices (recommended):
- One port-forward maps to one internal device/app at a time. You cannot reliably "share" the same forwarded UDP port (e.g. 7000) between multiple hosts behind the same router.
- Forward UDP only (not TCP) and only the one port you need.
- Prefer enabling the forward only when playing, and disable it afterwards.
- If your router supports it, restrict the forward to the other player's public IP (source IP restriction).
- Keep Windows Defender Firewall enabled; only allow the app on the needed network profile.
- Port-forwarding exposes that UDP service to the internet. The common risks are random traffic/DoS or triggering bugs via malformed packets. If you want the safest option, use a VPN overlay (Tailscale/ZeroTier).

1) Pick a UDP port (host)
  - Example: 7000
  - This value must match "Local Port" on the host.

2) Configure the host router (host)
  - Add a port-forward rule:
     Protocol: UDP
     External port: 7000
     Internal IP: host PC LAN IP (example: 192.168.1.50)
     Internal port: 7000
  - Tip: set a DHCP reservation for the host PC so its LAN IP does not change.

3) Configure Windows Firewall (host)
  - Ensure UDP 7000 is allowed for snesonline_win.exe.
  - If Windows prompts, allow it.

4) (Optional) Find the host Public IP (debugging)
  - Not needed for normal use. The Host's Connection Code already contains the host endpoint.
  - If you need it for debugging, use a "what is my IP" website or your router WAN status.

5) Configure both devices using snesonline_config.exe
  - Enable netplay: ON
  - Local UDP Port: 7000
  - Host: click Get Connection Info and share the Connection Code
  - Join: paste the Connection Code and click Join From Code

Important note (peer-to-peer UDP):
- For reliable internet play without a VPN, it often works best if BOTH players can accept inbound UDP (port-forward on both routers).
- If you can't (or don't want to) port-forward on both sides, use a VPN overlay.

7) Launch on both PCs
  - Run snesonline_win.exe on both PCs.
  - Both must load the same ROM.

----------------------------------------
Troubleshooting checklist (internet play)
----------------------------------------

If it won't connect:
- Use VPN overlay (Tailscale/ZeroTier) first; it avoids most NAT problems.
- Confirm the Join side pasted the correct Connection Code.
- Confirm both sides are using the same ROM + same core.
- If using port-forwarding:
  - Confirm the port-forward is UDP (not TCP).
  - Confirm the router forwards to the correct internal IP.
  - Confirm the host PC LAN IP didn’t change.
  - Confirm the ISP is not using CGNAT (common on some ISPs and nearly all mobile). If CGNAT: use VPN overlay.
- If using a hostname / dynamic DNS:
  - It must resolve to the host public IPv4.

Quick sanity test:
- Test on the same PC first using 127.0.0.1 and different local ports.
- Then test on the same LAN using the LAN IPs.
- Then move to VPN/internet.

Port-forwarding sanity test (advanced):
- On the host PC (forwarded machine), run a UDP listener on the forwarded port:
  powershell -NoProfile -Command "$u=New-Object Net.Sockets.UdpClient(7000); $ep=New-Object Net.IPEndPoint([Net.IPAddress]::Any,0); 'UDP 7000 listo'; while($true){$b=$u.Receive([ref]$ep); \"RX de $ep : $([Text.Encoding]::ASCII.GetString($b))\" }"
- From an external network, send a test packet to the host public IP:
  powershell -NoProfile -Command "$u=New-Object Net.Sockets.UdpClient; $b=[Text.Encoding]::ASCII.GetBytes('ping'); $u.Send($b,$b.Length,'<HOST_PUBLIC_IP>',7000) | Out-Null; 'enviado'"
- If the host receives packets, the host router forward + firewall are OK.
- If you see packets arriving from the other player but from a different source port (for example "RX de <peer_ip>:10510"), that usually means their NAT is rewriting ports and they may need their own port-forward (or use VPN).

Notes:
- Use your own legally obtained ROMs.
