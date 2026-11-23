const MAX_BYTES = 65536;
const MAX_SAVES = 9000;
const KEEP_SECONDS = 365 * 24 * 60 * 60;
const PATH = /^\/v1\/saves\/([0-9a-f]{64})$/;

const reply = (status, revision) =>
  new Response(null, { status, headers: revision === undefined ? {} : { "X-Revision": String(revision) } });

async function hashed(id) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(id));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function currentRevision(env, key) {
  const row = await env.DB.prepare("SELECT revision FROM saves WHERE key = ?").bind(key).first();
  return row ? row.revision : 0;
}

async function read(env, key) {
  const row = await env.DB.prepare("SELECT revision, data FROM saves WHERE key = ?").bind(key).first();
  if (!row) {
    return reply(404);
  }
  return new Response(new Uint8Array(row.data), {
    headers: { "Content-Type": "application/octet-stream", "X-Revision": String(row.revision) },
  });
}

async function write(request, env, key, address) {
  const expectedText = request.headers.get("X-Revision") ?? "";
  if (!/^\d{1,15}$/.test(expectedText)) {
    return reply(400);
  }
  const declared = Number(request.headers.get("Content-Length"));
  if (!declared) {
    return reply(411);
  }
  if (declared > MAX_BYTES) {
    return reply(413);
  }
  const data = await request.arrayBuffer();
  if (data.byteLength === 0 || data.byteLength > MAX_BYTES) {
    return reply(413);
  }
  const expected = Number(expectedText);
  const now = Math.floor(Date.now() / 1000);
  if (expected === 0) {
    const { success } = await env.CREATE_LIMIT.limit({ key: address });
    if (!success) {
      return reply(429);
    }
    const top = await env.DB.prepare("SELECT MAX(rowid) AS top FROM saves").first();
    if ((top?.top ?? 0) >= MAX_SAVES) {
      return reply(507);
    }
    const created = await env.DB.prepare(
      "INSERT INTO saves (key, revision, data, updated_at) VALUES (?, 1, ?, ?) ON CONFLICT (key) DO NOTHING",
    )
      .bind(key, data, now)
      .run();
    return created.meta.changes ? reply(200, 1) : reply(409, await currentRevision(env, key));
  }
  const updated = await env.DB.prepare(
    "UPDATE saves SET data = ?, revision = revision + 1, updated_at = ? WHERE key = ? AND revision = ?",
  )
    .bind(data, now, key, expected)
    .run();
  if (updated.meta.changes) {
    return reply(200, expected + 1);
  }
  const revision = await currentRevision(env, key);
  return revision ? reply(409, revision) : reply(404);
}

export default {
  async fetch(request, env) {
    const match = PATH.exec(new URL(request.url).pathname);
    if (!match) {
      return reply(404);
    }
    if (request.method !== "GET" && request.method !== "PUT") {
      return reply(405);
    }
    const address = request.headers.get("CF-Connecting-IP") ?? "unknown";
    const { success } = await env.REQUEST_LIMIT.limit({ key: address });
    if (!success) {
      return reply(429);
    }
    const key = await hashed(match[1]);
    return request.method === "GET" ? read(env, key) : write(request, env, key, address);
  },

  async scheduled(_event, env) {
    const oldest = Math.floor(Date.now() / 1000) - KEEP_SECONDS;
    await env.DB.prepare("DELETE FROM saves WHERE updated_at < ?").bind(oldest).run();
  },
};
