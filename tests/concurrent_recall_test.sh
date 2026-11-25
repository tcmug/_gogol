#!/usr/bin/env bash
# Regression: concurrent `recall` calls against ONE daemon must not crash it.
#
# The daemon runs one thread per unix-socket connection over a single shared warm
# embedder. A llama_context is not thread-safe, so two overlapping llama_encode calls
# used to abort the whole daemon (SIGABRT "pointer being freed was not allocated" /
# SIGSEGV in llama_batch_allocr). Embedder now serializes ctx_ access with a mutex.
#
# Fully isolated: throwaway GOGOL_HOME, never touches ~/.gogol (AGENTS.md).
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOGOL_BIN="${GOGOL_BIN:-$REPO_ROOT/build/gogol}"
MODEL="${GOGOL_MODEL:-$HOME/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf}"
THREADS="${GOGOL_CONCURRENCY:-8}"
ROUNDS="${GOGOL_ROUNDS:-5}"

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found" >&2; exit 0; }
[ -f "$MODEL" ] || { echo "SKIP: embedding model not found at $MODEL (set GOGOL_MODEL)" >&2; exit 0; }
[ -x "$GOGOL_BIN" ] || { echo "FAIL: gogol binary not found at $GOGOL_BIN" >&2; exit 1; }

SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/gogol_concurrent_test.XXXXXX")"
SVPID=""
cleanup() { [ -n "$SVPID" ] && kill "$SVPID" 2>/dev/null; wait "$SVPID" 2>/dev/null; rm -rf "$SANDBOX"; }
trap cleanup EXIT

export GOGOL_HOME="$SANDBOX/home"
mkdir -p "$GOGOL_HOME"
printf '[global]\nmodel = %s\n\n[mcp]\nenabled = true\nsocket = on\nhttp_port = 0\nidle_timeout_secs = 300\n' \
  "$MODEL" > "$GOGOL_HOME/config"

"$GOGOL_BIN" serve --port 0 >"$SANDBOX/serve.out" 2>"$SANDBOX/serve.err" &
SVPID=$!

SOCK="$GOGOL_HOME/daemon.sock"
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FAIL: daemon socket did not appear" >&2; cat "$SANDBOX/serve.err" >&2; exit 1; }

python3 - "$SOCK" "$THREADS" "$ROUNDS" <<'PY'
import json, socket, struct, sys, threading
sock_path, n, rounds = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

def call(i):
    s = socket.socket(socket.AF_UNIX); s.settimeout(120); s.connect(sock_path)
    req = {"jsonrpc": "2.0", "id": i, "method": "tools/call",
           "params": {"name": "recall", "arguments": {"text": f"concurrent embed query {i}", "scope": ""}}}
    b = json.dumps(req).encode(); s.sendall(struct.pack(">I", len(b)) + b)
    h = s.recv(4)
    if len(h) < 4: return False
    ln = struct.unpack(">I", h)[0]; d = b""
    while len(d) < ln:
        c = s.recv(ln - len(d))
        if not c: return False
        d += c
    return True

bad = 0
for _ in range(rounds):
    out = [False] * n
    def w(i):
        try: out[i] = call(i)
        except Exception: out[i] = False
    ts = [threading.Thread(target=w, args=(i,)) for i in range(n)]
    [t.start() for t in ts]; [t.join() for t in ts]
    bad += out.count(False)
sys.exit(1 if bad else 0)
PY
RC=$?
if [ $RC -ne 0 ] || ! kill -0 "$SVPID" 2>/dev/null; then
  echo "FAIL: daemon dropped/crashed under $THREADS concurrent recalls" >&2
  exit 1
fi
echo "concurrent recall OK ($THREADS threads x $ROUNDS rounds)"
