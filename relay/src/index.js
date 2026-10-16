// PSP Homebrew Store relay: a small, separate Cloudflare Worker for a few friends' PSPs.
//
// It serves the signed catalogue (catalogue.txt + catalogue.sig), the store app's own update
// (app/EBOOT.PBP), and plain-HTTP passthrough downloads of files from archive.org, but ONLY files
// listed in the signed catalogue. It is not an open proxy. The PSP can't do modern TLS, so it
// talks plain HTTP to this Worker and the Worker fetches archive.org over HTTPS.
//
// Rate limit: the RATE_LIMITER binding (per client IP), with a per-isolate fallback.

const STATIC = new Set(["/catalogue.txt", "/catalogue.sig", "/app/EBOOT.PBP"]);
const CATALOGUE_TTL_MS = 5 * 60 * 1000;
let catalogue = { at: 0, files: null };
const fallbackHits = new Map();

function text(status, body, extra = {}) {
  return new Response(body + "\n", {
    status,
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store", ...extra },
  });
}

async function allowed(env, ip) {
  if (env.RATE_LIMITER) {
    const { success } = await env.RATE_LIMITER.limit({ key: ip });
    return success;
  }
  const now = Date.now();
  const hits = (fallbackHits.get(ip) || []).filter((t) => now - t < 60000);
  hits.push(now);
  fallbackHits.set(ip, hits);
  if (fallbackHits.size > 5000) fallbackHits.clear();
  return hits.length <= 60;
}

async function listedFiles(env, request) {
  if (catalogue.files && Date.now() - catalogue.at < CATALOGUE_TTL_MS) return catalogue.files;
  const res = await env.ASSETS.fetch(new Request(new URL("/catalogue.txt", request.url)));
  if (!res.ok) throw new Error("catalogue missing");
  const files = new Map();
  for (const line of (await res.text()).split("\n")) {
    const f = line.split("\t");
    if (f[0] === "E" && f.length >= 9) files.set(`${f[1]}\n${f[5]}`, { size: Number(f[6]) });
  }
  catalogue = { at: Date.now(), files };
  return files;
}

function isArchiveOrg(url) {
  const u = new URL(url);
  return u.protocol === "https:" && (u.hostname === "archive.org" || u.hostname.endsWith(".archive.org"));
}

async function passthrough(env, request, path) {
  // /dl/<id>/<file path inside the item>, each part URL-encoded by the app
  const parts = path.slice("/dl/".length).split("/");
  if (parts.length < 2) return text(400, "bad download path");
  let id, file;
  try {
    id = decodeURIComponent(parts[0]);
    file = parts.slice(1).map(decodeURIComponent).join("/");
  } catch {
    return text(400, "bad download path");
  }
  const entry = (await listedFiles(env, request)).get(`${id}\n${file}`);
  if (!entry) return text(404, "not in the catalogue");

  const upstream = `https://archive.org/download/${encodeURIComponent(id)}/${file.split("/").map(encodeURIComponent).join("/")}`;
  const get = () => fetch(upstream, {
    method: request.method,
    headers: { "user-agent": "PSP-Homebrew-Store-relay/1.0 (personal, few users)" },
    redirect: "follow",
    cf: { cacheEverything: true, cacheTtl: 86400 },
  });
  let res = await get();
  if (res.status >= 500 || res.status === 429) {
    // archive.org's storage nodes sometimes fail one request; try once more
    await new Promise((r) => setTimeout(r, 1500));
    res = await get();
  }
  if (!isArchiveOrg(res.url)) return text(502, "upstream redirected away from archive.org");
  if (res.status === 429 || res.status === 503) return text(503, "archive.org is busy, try again in a minute", { "retry-after": "60" });
  if (!res.ok) return text(502, `archive.org answered ${res.status}`);
  const headers = new Headers({
    "content-type": "application/octet-stream",
    "cache-control": "public, max-age=86400, no-transform",
  });
  const len = res.headers.get("content-length");
  if (len) headers.set("content-length", len);
  return new Response(res.body, { status: 200, headers });
}

export default {
  async fetch(request, env) {
    if (request.method !== "GET" && request.method !== "HEAD") return text(405, "GET only", { allow: "GET, HEAD" });
    const url = new URL(request.url);
    const ip = request.headers.get("cf-connecting-ip") || "local";

    if (!(await allowed(env, ip))) return text(429, "slow down", { "retry-after": "60" });

    if (url.pathname === "/") return text(200, "PSP Homebrew Store relay. Point the PSP app at this address.");
    if (STATIC.has(url.pathname)) {
      const res = await env.ASSETS.fetch(request);
      if (!res.ok) return text(404, "not published yet");
      const out = new Response(res.body, res);
      out.headers.set("cache-control", "no-cache");
      return out;
    }
    if (url.pathname.startsWith("/dl/")) {
      try {
        return await passthrough(env, request, url.pathname);
      } catch (e) {
        return text(502, "relay error");
      }
    }
    return text(404, "not found");
  },
};
