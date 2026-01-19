# Room code rendezvous server (legacy / optional)

This server is **no longer required** for netplay in the default configuration.

Netplay now supports a **Direct Connect** flow where Player 1 generates a shareable **Connection Code** using STUN and Player 2 pastes it to join.

Keep using this room server only if you want the older room-code rendezvous workflow.

This is a tiny HTTP + UDP helper that lets two players rendezvous using a short code (8–12 chars, letters+numbers).

- The game calls `POST /rooms/connect` right before starting a match.
- The server assigns roles (Player 1 = first connector) and publishes the host endpoint.
- The game then connects peers directly (this is not a relay).

## Run (Node.js)
From the repo root:

- `node tools/room_server/server.js --port 8787`

Or (Windows PowerShell helper):

- `powershell -ExecutionPolicy Bypass -File tools/room_server/run_room_server.ps1 -Port 8787`

Optional API key (recommended if exposed to the public internet):

- `node tools/room_server/server.js --port 8787 --api-key YOUR_SECRET`

PowerShell helper with API key:

- `powershell -ExecutionPolicy Bypass -File tools/room_server/run_room_server.ps1 -Port 8787 -ApiKey YOUR_SECRET`

Enable connection logs:

- `node tools/room_server/server.js --port 8787 --log-connections`

## Hosting on Render (free tier) + downloads

Render web services are HTTP/TCP-first and **do not provide public UDP ingress**. This means:
- The HTTP API (Room Code rendezvous) will work.
- Any server-side UDP punch/WHOAMI helper may not work reliably from the public internet when hosted on Render.

Good news: the game can discover its public (NAT-mapped) UDP port via **STUN**, so the room server can be hosted as HTTP-only (like Render) and still support internet play in many NAT scenarios.

If you still want to host the HTTP room server on Render, the recommended setup is:
- Host the APK/ZIP somewhere static (GitHub Releases, S3/R2, etc.)
- Configure this server to **redirect** `/download/apk` and `/download/zip` to those URLs.

Environment variables (recommended on Render):
- `SNO_APK_URL` = direct HTTPS URL to your APK
- `SNO_ZIP_URL` = direct HTTPS URL to your Windows portable ZIP

Or CLI flags:
- `--apk-url https://...`
- `--zip-url https://...`

## Firewall / Router
If you want people on the internet to reach it, you must:

- Allow inbound TCP on the chosen port in Windows Firewall.
- (Optional / legacy) Allow inbound UDP on the chosen port in Windows Firewall (only needed for `SNO_WHOAMI1` / punch helper).
- Port-forward that TCP port on your router to this PC.
- (Optional / legacy) Port-forward that UDP port too, only if you rely on the UDP helper.

## API
All responses are JSON.

### Health
- `GET /health`

### Game-start connect (create/join)
- `POST /rooms/connect`

Body:
```json
{
  "code": "AB12CD34EF",
  "password": "YOUR_ROOM_PASSWORD",
  "port": 7000,
  "localIp": "192.168.1.50",
  "creatorToken": "(only Player 1 sends this on finalize)",
  "ttlSeconds": 600
}
```

Response (shape):
```json
{
  "ok": true,
  "created": true,
  "role": 1,
  "waiting": true,
  "creatorToken": "...",
  "youIp": "203.0.113.5",
  "room": { "code": "AB12CD34EF", "ip": "203.0.113.5", "localIp": "192.168.1.50", "port": 0, "expiresAt": 1730000000 }
}
```

Notes:

- Player 1 is the first connector and receives `creatorToken`.
- If Player 1 didn’t provide `port`, it creates a pending room (`port=0`). The client should then discover its public port (via STUN) and call again with `{ port, creatorToken }` to finalize.
- Older clients may rely on the server-side UDP `SNO_WHOAMI1` helper for port discovery, but this is not recommended for HTTP-only hosts.
- The server may store both a public IP (`room.ip`) and a best-effort LAN IP (`room.localIp`). `room.localIp` is only returned to clients whose `youIp` matches the room’s public IP (same NAT), so two devices on the same home network can prefer LAN routing.
- After Player 2 receives the host endpoint, the server deletes the room/punch state.

### Legacy endpoints
For debugging/backwards-compatibility, the server also supports `POST /rooms` and `PUT/GET/DELETE /rooms/{CODE}`.
