// worker.js - HAWK 370 release relay (Cloudflare Worker "hawk370-throttle-releases")
//
// Relays the binary files of GitHub Releases and adds the CORS header that
// GitHub's download CDN does not send (Access-Control-Allow-Origin), so the
// web installer (docs/index.html on GitHub Pages) may read the bytes.
//
// Route:
//   GET /<tag>/<filename>   ->  https://github.com/<OWNER>/<REPO>/releases/download/<tag>/<filename>
//
// Deploy: see README.md in this folder (wrangler deploy).

const OWNER = "bitsundbolts";
const REPO  = "hawk370-throttle";

// Only the installer page may load the files through this worker.
const ALLOWED_ORIGIN = "https://bitsundbolts.github.io";

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return new Response(null, { headers: corsHeaders() });
    }
    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405, headers: corsHeaders() });
    }

    const [, rawTag, rawFile] = url.pathname.split("/");
    if (!rawTag || !rawFile) {
      return new Response("Usage: /<tag>/<filename>", { status: 400, headers: corsHeaders() });
    }

    const tag  = decodeURIComponent(rawTag);
    const file = decodeURIComponent(rawFile);

    // Release files never change once published, so cache them at the edge:
    // repeat installs don't hit GitHub at all.
    const cacheKey = new Request(url.toString(), request);
    const cache = caches.default;
    const cached = await cache.match(cacheKey);
    if (cached) return cached;

    const upstream = `https://github.com/${OWNER}/${REPO}/releases/download/${encodeURIComponent(tag)}/${encodeURIComponent(file)}`;
    const ghResponse = await fetch(upstream, { redirect: "follow" });

    if (!ghResponse.ok) {
      return new Response(`Upstream error: ${ghResponse.status}`, {
        status: ghResponse.status,
        headers: corsHeaders(),
      });
    }

    const headers = new Headers(ghResponse.headers);
    headers.delete("content-disposition");   // no "Save as" dialog
    for (const [key, value] of Object.entries(corsHeaders())) headers.set(key, value);
    headers.set("Cache-Control", "public, max-age=86400, immutable");

    const response = new Response(ghResponse.body, { status: 200, headers });
    ctx.waitUntil(cache.put(cacheKey, response.clone()));
    return response;
  },
};

function corsHeaders() {
  return {
    "Access-Control-Allow-Origin": ALLOWED_ORIGIN,
    "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  };
}
