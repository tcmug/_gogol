#!/usr/bin/env bash
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOGOL_BIN="${GOGOL_BIN:-$REPO_ROOT/build/gogol}"
PORT="${GOGOL_HTTP_PORT:-7799}"

if ! command -v curl >/dev/null 2>&1; then
  echo "SKIP: curl not found" >&2
  exit 0
fi
if [ ! -x "$GOGOL_BIN" ]; then
  echo "FAIL: gogol binary not found at $GOGOL_BIN" >&2
  exit 1
fi

# No live model/no real store needed for parity: the HTTP transport is just a wire
# wrapper around process_request. We only require the binary itself to start and
# answer JSON-RPC requests over HTTP.
MODEL="${GOGOL_MODEL:-$HOME/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf}"
if [ ! -f "$MODEL" ]; then
  echo "SKIP: embedding model not found at $MODEL (set GOGOL_MODEL)" >&2
  exit 0
fi

SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/gogol_http_test.XXXXXX")"
cleanup() { kill "$SVPID" 2>/dev/null || true; wait "$SVPID" 2>/dev/null || true; rm -rf "$SANDBOX"; }
trap cleanup EXIT

export GOGOL_HOME="$SANDBOX/store"
mkdir -p "$GOGOL_HOME"
cat > "$GOGOL_HOME/config" <<EOF
model = $MODEL
[mcp]
enabled = true
EOF

"$GOGOL_BIN" serve --port "$PORT" >/tmp/gogol_http_test.out 2>/tmp/gogol_http_test.err &
SVPID=$!
sleep 1

# Basic HTTP response checks: GET / should return HTML and POST /mcp should return
# a JSON-RPC response containing tools.
HTML="$(curl -fsS "http://127.0.0.1:$PORT/" 2>/dev/null)"
if ! printf '%s' "$HTML" | grep -q 'gogol'; then
  echo "FAIL: GET / did not return the status page" >&2
  exit 1
fi

LIST_JSON="$(curl -fsS -XPOST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' 2>/dev/null)"
if ! printf '%s' "$LIST_JSON" | grep -q '"tools"'; then
  echo "FAIL: tools/list via HTTP did not return JSON-RPC tools" >&2
  exit 1
fi

# A small concurrency burst: ensure several requests finish cleanly.
for i in $(seq 1 10); do
  curl -fsS -XPOST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":'$i',"method":"tools/list","params":{}}' >/dev/null || {
    echo "FAIL: concurrent HTTP request $i failed" >&2
    exit 1
  }
done

echo "http serve parity OK"
