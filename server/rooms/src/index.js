import { DurableObject } from "cloudflare:workers";

const LETTERS = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE_LENGTH = 6;
const MAX_OTHERS = 3;
const MAX_RECENT = 8;
const MAX_BODY = 128;
const ROOM_SECONDS = 120;
const TOUCH_SECONDS = 30;
const RECENT_SECONDS = 60;
const PLAYER_SECONDS = 330;
const SWEEP_SECONDS = 3600;
const HELLO = /^([0-9a-f]{32}) (windows|linux|macos|android) ([0-9A-Za-z.\-]{1,24})$/;
const PATH = /^\/v1\/rooms(?:\/([0-9A-HJKMNP-TV-Z]{6})(?:\/(join|relay|[0-9a-f]{32}))?)?$/;
const MAX_RELAY_BYTES = 1200;
const MAX_RELAY_PER_SECOND = 90;
const ENDPOINT = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/;

const text = (status, body = "") => new Response(body, { status, headers: { "Content-Type": "text/plain" } });
const now = () => Math.floor(Date.now() / 1000);

function validEndpoint(value) {
  const parts = ENDPOINT.exec(value);
  if (!parts) {
    return false;
  }
  const port = Number(parts[5]);
  return port > 0 && port < 65536 && parts.slice(1, 5).every((part) => Number(part) < 256);
}

function randomText(alphabet, length) {
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  return [...bytes].map((byte) => alphabet[byte % alphabet.length]).join("");
}

function endpoints(body) {
  const parts = body.trim().split(" ");
  if (parts.length < 2 || !validEndpoint(parts[0]) || !validEndpoint(parts[1])) {
    return null;
  }
  return { public: parts[0], local: parts[1], others: Number(parts[2] ?? 0) };
}

export class Census extends DurableObject {
  async hello(id, platform, version) {
    await this.ctx.storage.put(`p:${id}`, { at: now(), platform, version });
    if ((await this.ctx.storage.getAlarm()) === null) {
      await this.ctx.storage.setAlarm(Date.now() + SWEEP_SECONDS * 1000);
    }
  }

  async room(code, players) {
    const key = `r:${code}`;
    if (players <= 0) {
      await this.ctx.storage.delete(key);
      return;
    }
    const known = await this.ctx.storage.get(key);
    await this.ctx.storage.put(key, { players, since: known?.since ?? now(), at: now() });
  }

  async sweep() {
    const stale = [];
    const players = [];
    const rooms = [];
    for (const [key, value] of await this.ctx.storage.list({ prefix: "p:" })) {
      if (value.at < now() - PLAYER_SECONDS) {
        stale.push(key);
      } else {
        players.push(value);
      }
    }
    for (const [key, value] of await this.ctx.storage.list({ prefix: "r:" })) {
      if (value.at < now() - ROOM_SECONDS) {
        stale.push(key);
      } else {
        rooms.push({ code: key.slice(2), players: value.players, seconds: now() - value.since });
      }
    }
    for (let i = 0; i < stale.length; i += 128) {
      await this.ctx.storage.delete(stale.slice(i, i + 128));
    }
    return { players, rooms };
  }

  async report() {
    const { players, rooms } = await this.sweep();
    const count = (field) => players.reduce((counts, player) => ({ ...counts, [player[field]]: (counts[player[field]] ?? 0) + 1 }), {});
    rooms.sort((a, b) => b.players - a.players || b.seconds - a.seconds);
    return { players: players.length, online: rooms.reduce((sum, room) => sum + room.players, 0), platforms: count("platform"), versions: count("version"), rooms };
  }

  async alarm() {
    const { players, rooms } = await this.sweep();
    if (players.length + rooms.length > 0) {
      await this.ctx.storage.setAlarm(Date.now() + SWEEP_SECONDS * 1000);
    }
  }
}

const census = (env) => env.CENSUS.get(env.CENSUS.idFromName("all"));

export class Room extends DurableObject {
  async load() {
    this.room ??= await this.ctx.storage.get("room");
    return this.room;
  }

  async save() {
    this.room.saved = now();
    await this.ctx.storage.put("room", this.room);
  }

  async count(players) {
    if (this.room?.code) {
      try {
        await census(this.env).room(this.room.code, players);
      } catch {}
    }
  }

  async create(seen, code) {
    if (await this.load()) {
      return null;
    }
    const token = randomText("0123456789abcdef", 32);
    this.room = { code, public: seen.public, local: seen.local, token, others: 0, joiners: [], touched: now() };
    await this.save();
    await this.count(1);
    await this.ctx.storage.setAlarm(Date.now() + ROOM_SECONDS * 1000);
    return token;
  }

  async join(seen) {
    const room = await this.load();
    if (!room) {
      return text(404);
    }
    if (room.others >= MAX_OTHERS) {
      return text(409);
    }
    const oldest = now() - RECENT_SECONDS;
    room.joiners = room.joiners.filter((joiner) => joiner.at >= oldest && joiner.public !== seen.public);
    room.joiners.push({ public: seen.public, local: seen.local, at: now() });
    room.joiners = room.joiners.slice(-MAX_RECENT);
    await this.save();
    return text(200, `${room.public} ${room.local}`);
  }

  async poll(token, seen) {
    const room = await this.load();
    if (!room || room.token !== token) {
      return text(404);
    }
    const moved = room.public !== seen.public || room.local !== seen.local;
    room.public = seen.public;
    room.local = seen.local;
    room.others = Math.min(seen.others || 0, MAX_OTHERS);
    room.touched = now();
    const grew = room.others !== room.counted;
    if (moved || grew || room.touched - room.saved >= TOUCH_SECONDS) {
      room.counted = room.others;
      await this.save();
      await this.count(1 + room.others);
    }
    const oldest = now() - RECENT_SECONDS;
    const lines = room.joiners.filter((joiner) => joiner.at >= oldest).map((joiner) => `${joiner.public} ${joiner.local}`);
    if (this.ctx.getWebSockets().length > 0) {
      lines.push("relay");
    }
    return text(200, lines.join("\n"));
  }

  async fetch(request) {
    if (request.headers.get("Upgrade") !== "websocket") {
      return text(426);
    }
    if (!(await this.load())) {
      return text(404);
    }
    if (this.ctx.getWebSockets().length > MAX_OTHERS) {
      return text(409);
    }
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  async webSocketMessage(socket, message) {
    if (typeof message === "string" || message.byteLength > MAX_RELAY_BYTES) {
      socket.close(1009, "too large");
      return;
    }
    if (!(await this.load())) {
      socket.close(1000, "room closed");
      return;
    }
    this.rates ??= new Map();
    const second = now();
    const rate = this.rates.get(socket);
    if (rate?.second === second) {
      rate.count++;
      if (rate.count > MAX_RELAY_PER_SECOND) {
        socket.close(1008, "too fast");
        return;
      }
    } else {
      this.rates.set(socket, { second, count: 1 });
    }
    for (const other of this.ctx.getWebSockets()) {
      if (other !== socket) {
        try {
          other.send(message);
        } catch {
          this.rates.delete(other);
        }
      }
    }
  }

  webSocketClose(socket) {
    this.rates?.delete(socket);
    try {
      socket.close();
    } catch {}
  }

  webSocketError(socket) {
    this.rates?.delete(socket);
  }

  closeRelay() {
    for (const socket of this.ctx.getWebSockets()) {
      try {
        socket.close(1000, "room closed");
      } catch {}
    }
  }

  async close(token) {
    const room = await this.load();
    if (!room || room.token !== token) {
      return text(404);
    }
    await this.count(0);
    this.room = undefined;
    this.closeRelay();
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();
    return text(204);
  }

  async alarm() {
    const room = await this.load();
    if (!room) {
      return;
    }
    const left = room.touched + ROOM_SECONDS - now();
    if (left > 0) {
      await this.ctx.storage.setAlarm(Date.now() + left * 1000);
      return;
    }
    await this.count(0);
    this.room = undefined;
    this.closeRelay();
    await this.ctx.storage.deleteAll();
  }
}

const roomFor = (env, code) => env.ROOMS.get(env.ROOMS.idFromName(code));

function sameText(a, b) {
  const left = new TextEncoder().encode(a);
  const right = new TextEncoder().encode(b);
  let difference = left.length ^ right.length;
  for (let i = 0; i < left.length; i++) {
    difference |= left[i] ^ (right[i % Math.max(right.length, 1)] ?? 0);
  }
  return difference === 0;
}

export default {
  async fetch(request, env) {
    const pathname = new URL(request.url).pathname;
    if (pathname === "/v1/census") {
      const key = request.headers.get("X-Key") ?? "";
      if (request.method !== "GET" || !env.CENSUS_KEY || !sameText(key, env.CENSUS_KEY)) {
        return text(404);
      }
      return Response.json(await census(env).report(), { headers: { "Cache-Control": "no-store" } });
    }
    if (pathname === "/v1/hello") {
      const address = request.headers.get("CF-Connecting-IP") ?? "unknown";
      const { success } = await env.REQUEST_LIMIT.limit({ key: address });
      const hello = request.method === "POST" && success ? HELLO.exec((await request.text()).slice(0, MAX_BODY).trim()) : null;
      if (!hello) {
        return text(success ? 400 : 429);
      }
      await census(env).hello(hello[1], hello[2], hello[3]);
      return text(204);
    }
    const match = PATH.exec(pathname);
    if (!match) {
      return text(404);
    }
    const [, code, action] = match;
    const address = request.headers.get("CF-Connecting-IP") ?? "unknown";
    const { success } = await env.REQUEST_LIMIT.limit({ key: address });
    if (!success) {
      return text(429);
    }
    if (request.method === "GET" && code && action === "relay") {
      return roomFor(env, code).fetch(request);
    }
    if (request.method === "DELETE" && code && action && action !== "join" && action !== "relay") {
      return roomFor(env, code).close(action);
    }
    if (request.method !== "POST") {
      return text(405);
    }
    const declared = Number(request.headers.get("Content-Length"));
    if (!declared || declared > MAX_BODY) {
      return text(413);
    }
    const body = await request.text();
    const seen = body.length <= MAX_BODY ? endpoints(body) : null;
    if (!seen) {
      return text(400);
    }
    if (!code) {
      const created = await env.CREATE_LIMIT.limit({ key: address });
      if (!created.success) {
        return text(429);
      }
      for (let attempt = 0; attempt < 5; attempt++) {
        const fresh = randomText(LETTERS, CODE_LENGTH);
        const token = await roomFor(env, fresh).create(seen, fresh);
        if (token) {
          return text(201, `${fresh} ${token}`);
        }
      }
      return text(503);
    }
    if (action === "join") {
      return roomFor(env, code).join(seen);
    }
    return action && action !== "relay" ? roomFor(env, code).poll(action, seen) : text(404);
  },
};
