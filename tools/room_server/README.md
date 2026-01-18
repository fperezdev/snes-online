# Room code rendezvous server (runs on your PC)

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

## Firewall / Router
If you want people on the internet to reach it, you must:

- Allow inbound TCP on the chosen port in Windows Firewall.
- Allow inbound UDP on the chosen port in Windows Firewall (used for `SNO_WHOAMI1` / punch helper).
- Port-forward that TCP+UDP port on your router to this PC.

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
- If Player 1 didn’t provide `port`, it creates a pending room (`port=0`), does UDP `SNO_WHOAMI1`, then calls again with `{ port, creatorToken }` to finalize.
- The server may store both a public IP (`room.ip`) and a best-effort LAN IP (`room.localIp`). `room.localIp` is only returned to clients whose `youIp` matches the room’s public IP (same NAT), so two devices on the same home network can prefer LAN routing.
- After Player 2 receives the host endpoint, the server deletes the room/punch state.

### Legacy endpoints
For debugging/backwards-compatibility, the server also supports `POST /rooms` and `PUT/GET/DELETE /rooms/{CODE}`.
