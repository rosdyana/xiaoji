#!/usr/bin/env bash
# Stages the web build into build/site and publishes it to Cloudflare Pages.
#   tools/deploy.sh            stage, then deploy
#   tools/deploy.sh --stage    stage only (check build/site before publishing)
# Needs: make wasm, the chunk store (build/web/xj) and a snapshot (make snapshot).
# Wrangler asks for a Cloudflare login on first use. XIAOJI_PAGES_PROJECT overrides the project name.
set -euo pipefail
PROJECT=${XIAOJI_PAGES_PROJECT:-xiaoji}
cd "$(git rev-parse --show-toplevel)"

test -f build/wasm/xiaoji.wasm || { echo "build/wasm is missing: run make wasm first"; exit 1; }
test -f build/web/xj/manifest-xiaoji-v2.json || { echo "the chunk store is missing: run make build/web/xj/manifest-xiaoji-v2.json"; exit 1; }
test -f build/web/xj/snapshot.json || { echo "no snapshot: run make snapshot first"; exit 1; }

rm -rf build/site && mkdir -p build/site/xj
# The page: everything in web/ but the dev server.
for f in web/*; do
  case "$(basename "$f")" in serve.mjs) continue ;; esac
  cp -R "$f" build/site/
done
cp build/wasm/xiaoji.mjs build/wasm/xiaoji.wasm build/site/
cp image/xiaoji.rc build/site/xiaoji.rc
for f in build/web/manifest-xiaoji.json; do test -f "$f" && cp "$f" build/site/; done

# The chunk store, pruned to the packs and chunks the current manifests name (old snapshots leave stale packs).
python3 -I tools/site-prune.py build/web/xj build/site/xj

# Limits of Cloudflare Pages: 20,000 files, 25 MiB per file.
n=$(find build/site -type f | wc -l | tr -d ' ')
big=$(find build/site -type f -size +25M | head -1)
test "$n" -le 20000 || { echo "build/site has $n files (limit 20000)"; exit 1; }
test -z "$big" || { echo "$big is over 25 MiB"; exit 1; }

CSP="default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"

cat > build/site/_headers <<EOF
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Content-Type-Options: nosniff
  Referrer-Policy: strict-origin-when-cross-origin
  Content-Security-Policy: $CSP

/xj/packs/*
  Cache-Control: public, max-age=31536000, immutable

/xj/chunks/*
  Cache-Control: public, max-age=31536000, immutable

/fonts/*
  Cache-Control: public, max-age=31536000, immutable
EOF

echo "staged build/site: $n files, $(du -sh build/site | cut -f1)"
[ "${1:-}" = "--stage" ] && exit 0
npm exec --yes -- wrangler@4 pages deploy build/site --project-name="$PROJECT" --branch=main --commit-dirty=true
