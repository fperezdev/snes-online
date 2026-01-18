#!/usr/bin/env node

const http = require('http');
const https = require('https');
const crypto = require('crypto');
const fs = require('fs');
const pathMod = require('path');
const dgram = require('dgram');
const os = require('os');

const ALPHANUM = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';

function nowS() {
  return Math.floor(Date.now() / 1000);
}

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(hi, v));
}

function normalizeCode(code) {
  const s = String(code || '').trim().toUpperCase();
  let out = '';
  for (const ch of s) {
    if (ALPHANUM.includes(ch)) out += ch;
  }
  return out;
}

function normalizePassword(pw) {
  const s = String(pw || '').trim();
  if (!s) return '';
  if (s.length < 4) return '';
  if (s.length > 64) return '';
  return s;
}

function hashPassword(pw) {
  const s = normalizePassword(pw);
  if (!s) return '';
  return crypto.createHash('sha256').update(s, 'utf8').digest('hex');
}

function genCode(length) {
  const len = clamp(Number(length) || 10, 8, 12);
  const bytes = crypto.randomBytes(len);
  let out = '';
  for (let i = 0; i < len; i++) {
    out += ALPHANUM[bytes[i] % ALPHANUM.length];
  }
  return out;
}

function parseArgs(argv) {
  const out = {
    host: '0.0.0.0',
    port: 8787,
    udpPort: 0,
    defaultTtl: 600,
    maxTtl: 86400,
    apiKey: '',
    logConnections: false,
    apkPath: '',
    zipPath: '',
  };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    const next = () => (i + 1 < argv.length ? argv[++i] : '');
    if (a === '--host') out.host = next();
    else if (a === '--port') out.port = Number(next()) || out.port;
    else if (a === '--udp-port') out.udpPort = Number(next()) || 0;
    else if (a === '--default-ttl') out.defaultTtl = Number(next()) || out.defaultTtl;
    else if (a === '--max-ttl') out.maxTtl = Number(next()) || out.maxTtl;
    else if (a === '--api-key') out.apiKey = next();
    else if (a === '--log-connections') out.logConnections = true;
    else if (a === '--apk' || a === '--apk-path') out.apkPath = next();
    else if (a === '--zip' || a === '--zip-path') out.zipPath = next();
  }
  out.defaultTtl = Math.max(30, out.defaultTtl | 0);
  out.maxTtl = Math.max(60, out.maxTtl | 0);
  return out;
}

function isTruthyEnv(v) {
  const s = String(v || '').trim().toLowerCase();
  return s === '1' || s === 'true' || s === 'yes' || s === 'on';
}

function parseUdpMessage(buf) {
  // Protocol:
  // - "SNO_WHOAMI1"\n         => reply: SNO_SELF1 <ip> <port>
  // - "SNO_PUNCH1 CODE"\n      => reply: SNO_WAIT or SNO_PEER1 <ip> <port>
  const s = String(buf || '').trim();
  if (!s) return null;
  const parts = s.split(/\s+/g);
  if (parts[0] === 'SNO_WHOAMI1') return { type: 'whoami' };
  if (parts[0] !== 'SNO_PUNCH1') return null;
  if (parts.length < 2) return null;
  const code = normalizeCode(parts[1]);
  if (code.length < 8 || code.length > 12) return null;
  return { type: 'peer', code };
}

function encodePeerMessage(ip, port) {
  return Buffer.from(`SNO_PEER1 ${ip} ${port}\n`);
}

function encodeSelfMessage(ip, port) {
  return Buffer.from(`SNO_SELF1 ${ip} ${port}\n`);
}

function encodeWaitMessage() {
  return Buffer.from('SNO_WAIT\n');
}

class PunchStore {
  constructor(ttlSeconds) {
    this.ttlSeconds = Math.max(5, Number(ttlSeconds) || 15);
    this.map = new Map();
  }

  delete(code) {
    return this.map.delete(code);
  }

  _purgeOne(code, now) {
    const e = this.map.get(code);
    if (!e) return;
    if (e.expiresAt <= now) this.map.delete(code);
  }

  purgeExpired() {
    const t = nowS();
    let removed = 0;
    for (const [code, e] of this.map.entries()) {
      if (e.expiresAt <= t) {
        this.map.delete(code);
        removed++;
      }
    }
    return removed;
  }

  upsertEndpoint(code, address, port) {
    const t = nowS();
    this._purgeOne(code, t);

    const key = `${address}:${port}`;
    let e = this.map.get(code);
    if (!e) {
      e = { a: null, b: null, expiresAt: t + this.ttlSeconds };
      this.map.set(code, e);
    }
    e.expiresAt = t + this.ttlSeconds;

    if (e.a && e.a.key === key) {
      e.a.ts = t;
      return { slot: 'a', peer: e.b };
    }
    if (e.b && e.b.key === key) {
      e.b.ts = t;
      return { slot: 'b', peer: e.a };
    }

    if (!e.a) {
      e.a = { key, address, port, ts: t };
      return { slot: 'a', peer: e.b };
    }
    if (!e.b) {
      e.b = { key, address, port, ts: t };
      return { slot: 'b', peer: e.a };
    }

    // Already has two distinct endpoints; keep the first two.
    return { slot: null, peer: null };
  }
}

function repoRoot() {
  // server.js lives in tools/room_server/
  return pathMod.resolve(__dirname, '..', '..');
}

function defaultApkPath(root) {
  return pathMod.join(root, 'platform', 'android', 'app', 'build', 'outputs', 'apk', 'release', 'app-release.apk');
}

function defaultZipPath(root) {
  const buildVs = pathMod.join(root, 'build_vs');
  try {
    const entries = fs.readdirSync(buildVs, { withFileTypes: true });
    const candidates = entries
      .filter((e) => e.isFile() && /^snes-online-.*-win64-portable\.zip$/i.test(e.name))
      .map((e) => pathMod.join(buildVs, e.name));
    if (candidates.length === 0) return pathMod.join(buildVs, 'snes-online-win64-portable.zip');

    let best = candidates[0];
    let bestMtime = fs.statSync(best).mtimeMs;
    for (let i = 1; i < candidates.length; i++) {
      const p = candidates[i];
      const m = fs.statSync(p).mtimeMs;
      if (m > bestMtime) {
        best = p;
        bestMtime = m;
      }
    }
    return best;
  } catch {
    return pathMod.join(buildVs, 'snes-online-win64-portable.zip');
  }
}

function sendFile(res, filePath, downloadName, contentType) {
  fs.stat(filePath, (err, st) => {
    if (err || !st || !st.isFile()) {
      return sendJson(res, 404, { ok: false, error: 'file_not_found', path: String(filePath) });
    }

    res.writeHead(200, {
      'Content-Type': contentType,
      'Content-Length': String(st.size),
      'Content-Disposition': `attachment; filename="${downloadName}"`,
      'Cache-Control': 'no-store',
    });

    const stream = fs.createReadStream(filePath);
    stream.on('error', () => {
      try {
        res.destroy();
      } catch {}
    });
    stream.pipe(res);
  });
}

class RoomStore {
  constructor(defaultTtlS, maxTtlS) {
    this.defaultTtlS = defaultTtlS;
    this.maxTtlS = maxTtlS;
    this.rooms = new Map();
  }

  upsert({ code, ip, localIp, port, ttlSeconds, pwHash, creatorToken }) {
    const ttl = ttlSeconds == null ? this.defaultTtlS : clamp(Number(ttlSeconds) || this.defaultTtlS, 30, this.maxTtlS);
    const t = nowS();
    const prev = this.rooms.get(code);
    const createdAt = prev ? prev.createdAt : t;
    const room = {
      code,
      ip,
      localIp: String(localIp || ''),
      port: Number(port) | 0,
      pwHash: String(pwHash || ''),
      creatorToken: String(creatorToken || (prev ? prev.creatorToken : '') || ''),
      createdAt,
      updatedAt: t,
      expiresAt: t + ttl,
    };
    this.rooms.set(code, room);
    return { ...room };
  }

  get(code) {
    const r = this.rooms.get(code);
    if (!r) return null;
    if (r.expiresAt <= nowS()) {
      this.rooms.delete(code);
      return null;
    }
    return { ...r };
  }

  getIfAuthorized(code, pwHash) {
    const r = this.get(code);
    if (!r) return { ok: false, error: 'not_found', room: null };
    if (!pwHash) return { ok: false, error: 'password_required', room: null };
    if (r.pwHash && r.pwHash !== pwHash) return { ok: false, error: 'wrong_password', room: null };
    return { ok: true, error: null, room: r };
  }

  delete(code) {
    return this.rooms.delete(code);
  }

  purgeExpired() {
    const t = nowS();
    let removed = 0;
    for (const [code, r] of this.rooms.entries()) {
      if (r.expiresAt <= t) {
        this.rooms.delete(code);
        removed++;
      }
    }
    return removed;
  }

  stats() {
    return { rooms: this.rooms.size };
  }
}

function sendJson(res, status, body) {
  const data = Buffer.from(JSON.stringify(body));
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': String(data.length),
    'Cache-Control': 'no-store',
  });
  res.end(data);
}

function readJson(req) {
  return new Promise((resolve) => {
    let buf = '';
    req.on('data', (chunk) => {
      buf += chunk;
      if (buf.length > 1024 * 64) req.destroy();
    });
    req.on('end', () => {
      if (!buf) return resolve({});
      try {
        resolve(JSON.parse(buf));
      } catch {
        resolve({});
      }
    });
  });
}

function clientIp(req) {
  // Prefer X-Forwarded-For if present (common in proxy setups).
  const xff = String(req.headers['x-forwarded-for'] || '').trim();
  if (xff) {
    const first = xff.split(',')[0].trim();
    if (first) return first;
  }

  // Otherwise use the direct socket address.
  const ra = req.socket.remoteAddress || '';
  // Node can expose IPv4 as ::ffff:1.2.3.4
  return ra.startsWith('::ffff:') ? ra.slice(7) : ra;
}

function isPrivateOrLoopbackIp(ip) {
  const s = String(ip || '').trim();
  if (!s) return true;
  const v = s.startsWith('::ffff:') ? s.slice(7) : s;

  // IPv6 loopback / private / link-local.
  if (v === '::1') return true;
  if (v.startsWith('fc') || v.startsWith('fd')) return true; // fc00::/7
  if (v.startsWith('fe80:')) return true; // link-local

  // IPv4.
  const parts = v.split('.').map((x) => Number(x));
  if (parts.length !== 4 || parts.some((n) => !Number.isFinite(n) || n < 0 || n > 255)) return false;
  const [a, b] = parts;
  if (a === 127) return true;
  if (a === 10) return true;
  if (a === 169 && b === 254) return true;
  if (a === 192 && b === 168) return true;
  if (a === 172 && b >= 16 && b <= 31) return true;
  return false;
}

function isLoopbackIp(ip) {
  const s = String(ip || '').trim();
  if (!s) return false;
  const v = s.startsWith('::ffff:') ? s.slice(7) : s;
  return v === '::1' || v.startsWith('127.');
}

function sanitizeClientLocalIp(v) {
  const s0 = String(v || '').trim();
  if (!s0) return '';
  const s = s0.startsWith('::ffff:') ? s0.slice(7) : s0;
  // Only accept private (never public) and avoid loopback.
  if (!isPrivateOrLoopbackIp(s)) return '';
  if (isLoopbackIp(s)) return '';
  return s;
}

function bestServerLanIpv4() {
  try {
    const ifs = os.networkInterfaces();
    for (const key of Object.keys(ifs)) {
      for (const e of ifs[key] || []) {
        if (!e || e.family !== 'IPv4') continue;
        const addr = String(e.address || '').trim();
        if (!addr) continue;
        if (addr.startsWith('127.')) continue;
        if (isPrivateOrLoopbackIp(addr) && !isLoopbackIp(addr)) return addr;
      }
    }
  } catch {}
  return '';
}

let g_cachedPublicIp = '';
let g_cachedPublicIpAtS = 0;

function fetchPublicIpBestEffort() {
  return new Promise((resolve) => {
    // Cache for 5 minutes.
    const t = nowS();
    if (g_cachedPublicIp && (t - g_cachedPublicIpAtS) < 300) return resolve(g_cachedPublicIp);

    const req = https.get('https://api.ipify.org', { timeout: 2500, headers: { 'User-Agent': 'snes-online-room-server/1.0' } }, (res) => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (d) => {
        buf += d;
        if (buf.length > 128) res.destroy();
      });
      res.on('end', () => {
        const ip = String(buf || '').trim();
        if (ip && ip.length <= 64) {
          g_cachedPublicIp = ip;
          g_cachedPublicIpAtS = nowS();
          return resolve(ip);
        }
        return resolve('');
      });
    });
    req.on('timeout', () => {
      try { req.destroy(); } catch {}
      resolve('');
    });
    req.on('error', () => resolve(''));
  });
}

async function main() {
  const args = parseArgs(process.argv);
  const apiKey = (args.apiKey || '').trim() || null;
  const logConnections = Boolean(args.logConnections || isTruthyEnv(process.env.SNO_LOG_CONNECTIONS) || isTruthyEnv(process.env.ROOM_SERVER_LOG_CONNECTIONS));
  const store = new RoomStore(args.defaultTtl, args.maxTtl);
  const punch = new PunchStore(15);

  const root = repoRoot();
  const apkPath = (args.apkPath || '').trim() || defaultApkPath(root);
  const zipPath = (args.zipPath || '').trim() || defaultZipPath(root);

  setInterval(() => store.purgeExpired(), 10_000).unref();
  setInterval(() => punch.purgeExpired(), 5_000).unref();

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
    const path = url.pathname;

    if (req.method === 'GET' && path === '/health') {
      return sendJson(res, 200, { ok: true, ts: nowS(), ...store.stats() });
    }

    if (req.method === 'GET' && path === '/download/apk') {
      return sendFile(res, apkPath, 'snes-online.apk', 'application/vnd.android.package-archive');
    }

    if (req.method === 'GET' && path === '/download/zip') {
      const name = pathMod.basename(zipPath) || 'snes-online-win64-portable.zip';
      return sendFile(res, zipPath, name, 'application/zip');
    }

    const requireAuth = () => {
      if (!apiKey) return true;
      return req.headers['x-api-key'] === apiKey;
    };

    const roomPasswordHashFromHeaders = () => {
      const raw = req.headers['x-room-password'] || req.headers['x-room-pass'] || '';
      return hashPassword(raw);
    };

    // New: game-start connect.
    // POST /rooms/connect
    // Body: { code?, codeLength?, password, port?, ttlSeconds? }
    // Behavior:
    // - First device becomes Player 1.
    //   - If it doesn't know its public port yet, call without port to create a pending room (port=0).
    //   - Then it does UDP WHOAMI and calls again with {port} to finalize.
    // - Second device becomes Player 2.
    //   - Calls without port and receives host endpoint when available; otherwise poll until port!=0.
    if (req.method === 'POST' && path === '/rooms/connect') {
      const body = await readJson(req);
      const pwHash = hashPassword(body.password || '');
      if (!pwHash) return sendJson(res, 400, { ok: false, error: 'password_required' });

      const reqIp = clientIp(req);
      let youIp = reqIp;
      if (isPrivateOrLoopbackIp(reqIp)) {
        const pub = await fetchPublicIpBestEffort();
        if (pub) youIp = pub;
      }

      const requested = normalizeCode(body.code || '');
      const code = requested || genCode(body.codeLength);
      if (code.length < 8 || code.length > 12) return sendJson(res, 400, { ok: false, error: 'invalid_code' });

      const existing = store.get(code);
      if (existing) {
        if (existing.pwHash && existing.pwHash !== pwHash) return sendJson(res, 403, { ok: false, error: 'wrong_password' });

        // Allow the creator to finalize the room by setting the port.
        const port = Number(body.port ?? 0);
        const token = String(body.creatorToken || '');
        const isCreator = token && existing.creatorToken && token === existing.creatorToken;
        const canFinalize = isCreator && existing.port === 0 && Number.isFinite(port) && port >= 1 && port <= 65535;
        if (canFinalize) {
          const bodyLocalIp = sanitizeClientLocalIp(body.localIp);
          const nextLocalIp = (!existing.localIp && bodyLocalIp) ? bodyLocalIp : existing.localIp;
          const room = store.upsert({
            code,
            ip: existing.ip,
            localIp: nextLocalIp,
            port,
            ttlSeconds: body.ttlSeconds,
            pwHash,
            creatorToken: existing.creatorToken,
          });
          if (logConnections) {
            console.log(`[connect] finalize code=${code} by=${reqIp} youIp=${youIp} port=${port} role=1 waiting=false`);
          }
          const exposeLocal = youIp && room.ip && youIp === room.ip;
          return sendJson(res, 200, {
            ok: true,
            created: false,
            role: 1,
            waiting: false,
            youIp,
            room: { code: room.code, ip: room.ip, localIp: exposeLocal ? room.localIp : '', port: room.port, expiresAt: room.expiresAt },
          });
        }

        const waiting = existing.port === 0;
        if (logConnections) {
          const role = isCreator ? 1 : 2;
          console.log(`[connect] join code=${code} by=${reqIp} youIp=${youIp} role=${role} waiting=${waiting} host=${existing.ip}:${existing.port || 0}`);
        }
        const exposeLocal = youIp && existing.ip && youIp === existing.ip;
        const role = isCreator ? 1 : 2;
        const clearAfter = role === 2 && !waiting && existing.port !== 0;
        const payload = {
          ok: true,
          created: false,
          role,
          waiting,
          youIp,
          room: { code: existing.code, ip: existing.ip, localIp: exposeLocal ? String(existing.localIp || '') : '', port: existing.port, expiresAt: existing.expiresAt },
        };

        if (clearAfter) {
          store.delete(code);
          punch.delete(code);
          if (logConnections) {
            console.log(`[connect] clear code=${code} (player2_received_endpoint)`);
          }
        }
        return sendJson(res, 200, payload);
      }

      // Room doesn't exist: first connector becomes Player 1.
      // Always store the request-derived IP (never trust a client-provided local/LAN IP).
      const creatorToken = crypto.randomBytes(16).toString('hex');

      // Public IP: prefer the request IP if it's already public; otherwise best-effort server public IP.
      const ip = youIp;

      // Local IP: if creator came from a LAN address, keep it; if they came from loopback, use a best-effort LAN IP.
      let localIp = '';
      const bodyLocalIp = sanitizeClientLocalIp(body.localIp);
      if (bodyLocalIp) {
        localIp = bodyLocalIp;
      }
      if (isPrivateOrLoopbackIp(reqIp)) {
        if (!localIp) {
          if (isLoopbackIp(reqIp)) localIp = bestServerLanIpv4();
          else localIp = reqIp;
        }
      }
      const port = Number(body.port ?? 0);
      const finalPort = Number.isFinite(port) && port >= 1 && port <= 65535 ? port : 0;
      const room = store.upsert({ code, ip, localIp, port: finalPort, ttlSeconds: body.ttlSeconds, pwHash, creatorToken });
      if (logConnections) {
        console.log(`[connect] create code=${code} by=${reqIp} youIp=${youIp} ip=${ip} localIp=${localIp || '-'} port=${finalPort} role=1 waiting=${finalPort === 0}`);
      }
      return sendJson(res, 200, {
        ok: true,
        created: true,
        role: 1,
        waiting: finalPort === 0,
        creatorToken,
        youIp,
        room: { code: room.code, ip: room.ip, localIp: room.localIp, port: room.port, expiresAt: room.expiresAt },
      });
    }

    if (req.method === 'POST' && path === '/rooms') {
      // Legacy endpoint retained for debugging; requires auth if configured.
      if (!requireAuth()) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
      const body = await readJson(req);

      const pwHash = hashPassword(body.password || '');
      if (!pwHash) return sendJson(res, 400, { ok: false, error: 'password_required' });

      const requested = normalizeCode(body.code || '');
      const code = requested || genCode(body.codeLength);
      if (code.length < 8 || code.length > 12) return sendJson(res, 400, { ok: false, error: 'invalid_code' });

      const port = Number(body.port ?? 7000);
      if (!Number.isFinite(port) || port < 1 || port > 65535) return sendJson(res, 400, { ok: false, error: 'invalid_port' });

      const reqIp = clientIp(req);
      let ip = reqIp;
      if (isPrivateOrLoopbackIp(reqIp)) {
        const pub = await fetchPublicIpBestEffort();
        if (pub) ip = pub;
      }
      let localIp = '';
      if (isPrivateOrLoopbackIp(reqIp)) {
        if (isLoopbackIp(reqIp)) localIp = bestServerLanIpv4();
        else localIp = reqIp;
      }
      const creatorToken = crypto.randomBytes(16).toString('hex');
      const room = store.upsert({ code, ip, localIp, port, ttlSeconds: body.ttlSeconds, pwHash, creatorToken });
      return sendJson(res, 200, { ok: true, room: { code: room.code, ip: room.ip, localIp: room.localIp, port: room.port, expiresAt: room.expiresAt } });
    }

    if (path.startsWith('/rooms/')) {
      const code = normalizeCode(path.slice('/rooms/'.length));
      if (code.length < 8 || code.length > 12) return sendJson(res, 400, { ok: false, error: 'invalid_code' });

      if (req.method === 'GET') {
        const pwHash = roomPasswordHashFromHeaders();
        const g = store.getIfAuthorized(code, pwHash);
        if (!g.ok) {
          const status = g.error === 'wrong_password' ? 403 : g.error === 'password_required' ? 400 : 404;
          return sendJson(res, status, { ok: false, error: g.error });
        }
        const room = g.room;
        return sendJson(res, 200, { ok: true, room: { code: room.code, ip: room.ip, localIp: room.localIp, port: room.port, expiresAt: room.expiresAt } });
      }

      if (req.method === 'PUT') {
        if (!requireAuth()) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
        const body = await readJson(req);

        const pwHash = hashPassword(body.password || '');
        if (!pwHash) return sendJson(res, 400, { ok: false, error: 'password_required' });

        const port = Number(body.port ?? 7000);
        if (!Number.isFinite(port) || port < 1 || port > 65535) return sendJson(res, 400, { ok: false, error: 'invalid_port' });

        const reqIp = clientIp(req);
        let ip = reqIp;
        if (isPrivateOrLoopbackIp(reqIp)) {
          const pub = await fetchPublicIpBestEffort();
          if (pub) ip = pub;
        }
        let localIp = '';
        if (isPrivateOrLoopbackIp(reqIp)) {
          if (isLoopbackIp(reqIp)) localIp = bestServerLanIpv4();
          else localIp = reqIp;
        }
        const room = store.upsert({ code, ip, localIp, port, ttlSeconds: body.ttlSeconds, pwHash });
        return sendJson(res, 200, { ok: true, room: { code: room.code, ip: room.ip, localIp: room.localIp, port: room.port, expiresAt: room.expiresAt } });
      }

      if (req.method === 'DELETE') {
        if (!requireAuth()) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
        const deleted = store.delete(code);
        return sendJson(res, 200, { ok: true, deleted });
      }
    }

    return sendJson(res, 404, { ok: false, error: 'not_found' });
  });

  server.listen(args.port, args.host, () => {
    console.log(`Room server listening on http://${args.host}:${args.port}`);
    console.log('Endpoints: POST /rooms/connect, PUT/GET/DELETE /rooms/{CODE}, GET /health');
    console.log('Downloads: GET /download/apk, GET /download/zip');
    console.log(`UDP punch: send "SNO_WHOAMI1" or "SNO_PUNCH1 CODE" to udp://${args.host}:${args.udpPort || args.port}`);
    if (apiKey) console.log('Auth: requires header X-API-Key');
  });

  // UDP rendezvous / punch-through helper.
  const udpPort = args.udpPort || args.port;
  const udp = dgram.createSocket('udp4');
  udp.on('error', (e) => {
    console.error('UDP server error:', e);
  });

  udp.on('message', (msg, rinfo) => {
    const m = parseUdpMessage(msg);
    if (!m) return;

    if (m.type === 'whoami') {
      if (logConnections) {
        console.log(`[udp] whoami from=${rinfo.address}:${rinfo.port}`);
      }
      try {
        udp.send(encodeSelfMessage(rinfo.address, rinfo.port), rinfo.port, rinfo.address);
      } catch {}
      return;
    }

    // Only allow punching for an existing, non-expired room code.
    const room = store.get(m.code);
    if (!room) {
      if (logConnections) {
        console.log(`[udp] punch code=${m.code} from=${rinfo.address}:${rinfo.port} -> noroom`);
      }
      try {
        udp.send(Buffer.from('SNO_NOROOM\n'), rinfo.port, rinfo.address);
      } catch {}
      return;
    }

    if (logConnections) {
      console.log(`[udp] punch code=${m.code} from=${rinfo.address}:${rinfo.port}`);
    }

    const res = punch.upsertEndpoint(m.code, rinfo.address, rinfo.port);
    if (res.peer) {
      const data = encodePeerMessage(res.peer.address, res.peer.port);
      try {
        udp.send(data, rinfo.port, rinfo.address);
      } catch {}
      // Also notify the peer immediately.
      try {
        udp.send(encodePeerMessage(rinfo.address, rinfo.port), res.peer.port, res.peer.address);
      } catch {}
    } else {
      try {
        udp.send(encodeWaitMessage(), rinfo.port, rinfo.address);
      } catch {}
    }
  });

  udp.bind(udpPort, args.host, () => {
    console.log(`UDP punch helper listening on udp://${args.host}:${udpPort}`);
  });
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
