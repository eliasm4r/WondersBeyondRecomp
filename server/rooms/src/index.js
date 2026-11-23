import { DurableObject } from "cloudflare:workers";

const LETTERS = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE_LENGTH = 6;
const MAX_OTHERS = 3;
const MAX_RECENT = 8;
const MAX_BODY = 128;
const ROOM_SECONDS = 120;
const TOUCH_SECONDS = 30;
const RECENT_SECONDS = 60;
const PATH = /^\/v1\/rooms(?:\/([0-9A-HJKMNP-TV-Z]{6})(?:\/(join|[0-9a-f]{32}))?)?$/;
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

export class Room extends DurableObject {
  async load() {
    this.room ??= await this.ctx.storage.get("room");
    return this.room;
  }

  async save() {
    this.room.saved = now();
    await this.ctx.storage.put("room", this.room);
  }

  async create(seen) {
    if (await this.load()) {
      return null;
    }
    const token = randomText("0123456789abcdef", 32);
    this.room = { public: seen.public, local: seen.local, token, others: 0, joiners: [], touched: now() };
    await this.save();
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
    if (moved || room.touched - room.saved >= TOUCH_SECONDS) {
      await this.save();
    }
    const oldest = now() - RECENT_SECONDS;
    return text(200, room.joiners.filter((joiner) => joiner.at >= oldest).map((joiner) => `${joiner.public} ${joiner.local}`).join("\n"));
  }

  async close(token) {
    const room = await this.load();
    if (!room || room.token !== token) {
      return text(404);
    }
    this.room = undefined;
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
    this.room = undefined;
    await this.ctx.storage.deleteAll();
  }
}

const roomFor = (env, code) => env.ROOMS.get(env.ROOMS.idFromName(code));

export default {
  async fetch(request, env) {
    const match = PATH.exec(new URL(request.url).pathname);
    if (!match) {
      return text(404);
    }
    const [, code, action] = match;
    const address = request.headers.get("CF-Connecting-IP") ?? "unknown";
    const { success } = await env.REQUEST_LIMIT.limit({ key: address });
    if (!success) {
      return text(429);
    }
    if (request.method === "DELETE" && code && action && action !== "join") {
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
        const token = await roomFor(env, fresh).create(seen);
        if (token) {
          return text(201, `${fresh} ${token}`);
        }
      }
      return text(503);
    }
    if (action === "join") {
      return roomFor(env, code).join(seen);
    }
    return action ? roomFor(env, code).poll(action, seen) : text(404);
  },
};
