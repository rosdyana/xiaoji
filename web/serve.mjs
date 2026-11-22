// Dev server for the web page (M4). Node only, no dependencies.
//
//   node web/serve.mjs [--port N] [--host H] [--coop]
//
// URL layout:
//   /manifest.json          build/web/manifest.json (make build/web/manifest.json)
//   /rootfs/<guest path>    build/rootfs/<guest path> (lazy files, Range requests)
//   /<name>                 web/<name>, else build/wasm/<name>
//
// --coop adds Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers. Off by
// default: the engine does not use SharedArrayBuffer yet. Turn it on when threads land.
// Caching is off (Cache-Control: no-store) for development.

import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { stat, realpath } from 'node:fs/promises';
import { dirname, extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..');
export const DIRS = {
  web: join(HERE),
  wasm: join(REPO, 'build', 'wasm'),
  manifest: join(REPO, 'build', 'web', 'manifest.json'),
  rootfs: join(REPO, 'build', 'rootfs'),
};

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
};

function contentType(path) {
  return TYPES[extname(path).toLowerCase()] ?? 'application/octet-stream';
}

// Maps a request path to a file under one of the roots, or null. Refuses anything that
// leaves its root, including through symlinks.
async function locate(urlPath) {
  let p;
  try {
    p = decodeURIComponent(urlPath);
  } catch {
    return null;
  }
  if (p === '/manifest.json') return { file: DIRS.manifest, root: DIRS.manifest };
  if (p.startsWith('/rootfs/')) return within(DIRS.rootfs, p.slice('/rootfs'.length));
  if (p === '/') p = '/index.html';
  const name = p.slice(1);
  if (!name || name.includes('/')) return null;
  for (const root of [DIRS.web, DIRS.wasm]) {
    const hit = await within(root, '/' + name);
    if (hit) return hit;
  }
  return null;
}

async function within(root, rel) {
  const target = normalize(join(root, rel));
  if (target !== root && !target.startsWith(root + sep)) return null;
  let real;
  try {
    real = await realpath(target);
  } catch {
    return null;
  }
  const rootReal = await realpath(root);
  if (real !== rootReal && !real.startsWith(rootReal + sep)) return null;
  return { file: real, root: rootReal };
}

// Parses a single "bytes=a-b", "bytes=a-" or "bytes=-n" range. Returns null for a
// missing header, {start,end} for a satisfiable range, or 'bad' otherwise.
function parseRange(header, size) {
  if (!header) return null;
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m || (m[1] === '' && m[2] === '')) return 'bad';
  let start, end;
  if (m[1] === '') {
    const n = Number(m[2]);
    if (n === 0) return 'bad';
    start = Math.max(0, size - n);
    end = size - 1;
  } else {
    start = Number(m[1]);
    end = m[2] === '' ? size - 1 : Math.min(Number(m[2]), size - 1);
  }
  if (size === 0 || start >= size || start > end) return 'bad';
  return { start, end };
}

function send(res, status, headers, body) {
  res.writeHead(status, headers);
  res.end(body);
}

export function createHandler({ coop = false, log = () => {} } = {}) {
  return async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const common = {
      'Cache-Control': 'no-store',
      'Accept-Ranges': 'bytes',
    };
    if (coop) {
      common['Cross-Origin-Opener-Policy'] = 'same-origin';
      common['Cross-Origin-Embedder-Policy'] = 'require-corp';
    }
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      return send(res, 405, { ...common, Allow: 'GET, HEAD' }, 'method not allowed\n');
    }
    const hit = await locate(url.pathname);
    if (!hit) {
      log(`404 ${req.method} ${url.pathname}`);
      return send(res, 404, { ...common, 'Content-Type': 'text/plain; charset=utf-8' }, 'not found\n');
    }
    let st;
    try {
      st = await stat(hit.file);
    } catch {
      return send(res, 404, common, 'not found\n');
    }
    if (!st.isFile()) return send(res, 404, common, 'not found\n');
    const size = st.size;
    const headers = {
      ...common,
      'Content-Type': contentType(hit.file),
      'Last-Modified': st.mtime.toUTCString(),
    };
    const range = parseRange(req.headers.range, size);
    if (range === 'bad') {
      return send(res, 416, { ...common, 'Content-Range': `bytes */${size}` }, '');
    }
    if (range) {
      const len = range.end - range.start + 1;
      headers['Content-Range'] = `bytes ${range.start}-${range.end}/${size}`;
      headers['Content-Length'] = String(len);
      log(`206 ${req.method} ${url.pathname} ${range.start}-${range.end}`);
      res.writeHead(206, headers);
      if (req.method === 'HEAD') return res.end();
      return createReadStream(hit.file, { start: range.start, end: range.end }).pipe(res);
    }
    headers['Content-Length'] = String(size);
    log(`200 ${req.method} ${url.pathname} ${size}`);
    res.writeHead(200, headers);
    if (req.method === 'HEAD') return res.end();
    return createReadStream(hit.file).pipe(res);
  };
}

// Starts the server and resolves to { server, port, url, close() }. Port 0 picks a free port.
export function startServer({ port = 8080, host = '127.0.0.1', coop = false, log } = {}) {
  const handler = createHandler({ coop, log });
  const server = createServer((req, res) => {
    handler(req, res).catch((err) => {
      if (!res.headersSent) send(res, 500, { 'Cache-Control': 'no-store' }, 'server error\n');
      console.error('serve:', err);
    });
  });
  return new Promise((resolveStart, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      const addr = server.address();
      const url = `http://${host}:${addr.port}/`;
      resolveStart({
        server,
        port: addr.port,
        url,
        close: () => new Promise((r) => server.close(() => r())),
      });
    });
  });
}

function parseArgs(argv) {
  const opts = { port: 8080, host: '127.0.0.1', coop: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') opts.port = Number(argv[++i]);
    else if (a === '--host') opts.host = argv[++i];
    else if (a === '--coop') opts.coop = true;
    else throw new Error(`unknown argument: ${a}`);
  }
  return opts;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  const opts = parseArgs(process.argv.slice(2));
  const srv = await startServer({ ...opts, log: (line) => console.log(line) });
  console.log(`serving ${srv.url}${opts.coop ? ' (COOP/COEP on)' : ''}`);
}
