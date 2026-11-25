#!/usr/bin/env bash
# cli_verbs_test.sh — scripted end-to-end coverage for the restructured CLI verb
# set + the MCP surface, run in FULL isolation (never touches the live ~/.gogol).
#
# What it asserts (STANDING RULE: no test => it's broken):
#   1. CLI subcommands index|recall|get|delve|links|metrics|remember|forget|status
#      all run and produce non-error output on a tiny seeded git repo.
#   2. delve output actually composes definitions + callers + callees + imports
#      (not an empty shell) on real indexed code.
#   3. MCP tools/list (JSON-RPC over stdio) includes get + delve and does NOT
#      include the renamed-away `retrieve`.
#   4. MCP tools/call delve returns the composed structure.
#   5. Removed surfaces are GONE: the `mem` subcommand and its legacy flags
#      (--cutover / --sync / --categorize / --resync-disk) all fail.
#   6. OUTPUT SHAPE: every read verb emits ONE wrapped JSON object (never NDJSON /
#      one-object-per-line). recall/literal/metrics/links/related/delve/get all checked.
#   7. SCOPE ISOLATION (positive AND negative): a repo-scoped query finds that repo's
#      symbols AND a foreign symbol scoped to the wrong repo returns EMPTY (scoping
#      actually bites, not just "returns something"). Plus the parent-dir union.
#
# Isolation: GOGOL_HOME points the store (cache.db/notes.db) at a temp dir; HOME
# points config resolution at a temp dir with a minimal isolated config; the real
# model is reused via GOGOL_MODEL. Nothing under the developer's ~/.gogol is read
# or written.
#
# Bounded runs: gtimeout (this macOS box has no `timeout`). Any hang => failure.
set -u

# --- locate the built binary + a model (skip cleanly if prerequisites absent) ---
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOGOL_BIN="${GOGOL_BIN:-$REPO_ROOT/build/gogol}"
TIMEOUT="${GOGOL_TEST_TIMEOUT:-60}"

if ! command -v gtimeout >/dev/null 2>&1; then
    echo "SKIP: gtimeout not found (brew install coreutils). CLI verb script not run." >&2
    exit 0
fi
if [ ! -x "$GOGOL_BIN" ]; then
    echo "FAIL: gogol binary not found at $GOGOL_BIN" >&2
    exit 1
fi

# Model: explicit env wins, else the conventional live location (read-only use).
MODEL="${GOGOL_MODEL:-$HOME/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf}"
if [ ! -f "$MODEL" ]; then
    echo "SKIP: embedding model not found at $MODEL (set GOGOL_MODEL). Model-dependent CLI verbs not run." >&2
    exit 0
fi

# --- isolated sandbox: store (GOGOL_HOME) + config (HOME) + a seeded git repo ---
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/gogol_verbs_test.XXXXXX")"
cleanup() { rm -rf "$SANDBOX"; }
trap cleanup EXIT

export GOGOL_HOME="$SANDBOX/store"
export GOGOL_MODEL="$MODEL"
FAKE_HOME="$SANDBOX/home"
mkdir -p "$GOGOL_HOME" "$FAKE_HOME/.gogol"
# Minimal isolated config. gogol reads the config from gogol_home()/config (GOGOL_HOME),
# so the isolated config goes THERE (not $FAKE_HOME/.gogol) — GOGOL_HOME isolates config
# too. A copy under $FAKE_HOME is harmless but unused.
cat > "$GOGOL_HOME/config" <<EOF
model = $MODEL
[mcp]
enabled = true
EOF

# Tiny seeded git repo with a call graph + an import, so delve/links/metrics have
# real structure to find. Functions are intentionally >5 lines so the tree-sitter
# chunker keeps them as AST function chunks (a sub-5-line node is merged/discarded
# and the file falls back to the window chunker, which carries no heading) — the
# symbol-mapping fix keys the symbol table off the chunk HEADING, which only AST
# chunks populate.
REPO="$SANDBOX/repo"
mkdir -p "$REPO/src"
cat > "$REPO/src/orders.ts" <<'EOF'
import { persist } from "./db";
import { validate } from "./validate";

export function processOrder(o: any): boolean {
  console.log("processing order");
  if (!validate(o)) {
    return false;
  }
  const stored = persist(o);
  return stored;
}

export function retryOrder(o: any): boolean {
  console.log("retrying order");
  const result = processOrder(o);
  return result;
}
EOF
cat > "$REPO/src/db.ts" <<'EOF'
export function persist(o: any): boolean {
  console.log("persisting");
  return true;
}
EOF
cat > "$REPO/src/validate.ts" <<'EOF'
export function validate(o: any): boolean {
  console.log("validating");
  return o != null;
}
EOF
( cd "$REPO" && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -qm init )

PASS=0
FAIL=0
ok()   { echo "  ok   $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL $1" >&2; FAIL=$((FAIL+1)); }

# one_json_object <output> <label> — assert the output is a SINGLE wrapped JSON object,
# not NDJSON (one bare {...} per line). The shape contract every read verb must meet so
# a consumer can `json.load` the whole stdout. Counts top-level `{` at line start: a
# wrapped object has exactly one; NDJSON has one per row. Blank output fails.
one_json_object() {
    local out="$1" label="$2"
    local braces; braces=$(printf '%s' "$out" | grep -c '^{')
    if [ -n "$out" ] && [ "$braces" = "1" ]; then ok "$label emits one wrapped JSON object"
    else bad "$label NOT one JSON object (leading-{ lines=$braces; got: $out)"; fi
}

# run gogol under isolation, bounded. Prints combined stdout+stderr.
G() { HOME="$FAKE_HOME" gtimeout "$TIMEOUT" "$GOGOL_BIN" "$@" 2>&1; }

echo "[cli verbs — isolated store=$GOGOL_HOME]"

# 0) LAZY-INDEX CHOKEPOINT: a brand-new repo queried for the FIRST time by a
#    non-recall read command must self-index (regression guard). Historically only
#    recall triggered the on-demand refresh, so a first-ever delve/links/metrics/
#    literal in a never-queried repo silently returned empty. ensure_scope_indexed()
#    centralises the refresh before dispatch, so EVERY read command self-populates.
#    We use a SEPARATE fresh repo (REPO2) and hit it with metrics as the first touch
#    (no prior index/recall), then assert it found this repo's function.
REPO2="$SANDBOX/repo2"
mkdir -p "$REPO2/src"
cat > "$REPO2/src/calc.ts" <<'EOF'
export function tangle(x: number): number {
  if (x > 0) {
    if (x > 1) { return x * 2; }
    return x + 1;
  }
  return 0;
}
EOF
( cd "$REPO2" && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -qm init )
OUT="$(cd "$REPO2" && G metrics)"
if echo "$OUT" | grep -q 'tangle' && echo "$OUT" | grep -q '^{"functions":\['; then
    ok "metrics self-indexes a never-queried repo (lazy-index chokepoint)"
else
    bad "metrics lazy-index on first touch (got: $OUT)"
fi
# And delve on the SAME now-indexed repo composes structure (no explicit index ran).
OUT="$(cd "$REPO2" && G delve tangle)"
if echo "$OUT" | grep -q '"definitions":\[{'; then
    ok "delve resolves a definition after lazy-index (no explicit index step)"
else
    bad "delve after lazy-index (got: $OUT)"
fi

# 0b) REPO SELECTION by --scope (PATH) and --ref (SHA) — the retired-index replacement.
#     `tangle` lives ONLY in repo2. Standing in REPO (a DIFFERENT repo), select repo2
#     by absolute path and by root-SHA and confirm the symbol is found — i.e. --scope/
#     --ref pick the repo, not the CWD. (Guards the --index retirement.)
#     NOTE: we scope by PATH and SHA, not bare repo NAME. A bare name only resolves for
#     a PATH-keyed idx; since identity is the root SHA, repo_tag(idx)==the SHA, so a
#     bare name can't match a SHA-keyed index — path/ref are the reliable selectors.
REPO2_SHA="$(cd "$REPO2" && git rev-list --max-parents=0 HEAD)"
OUT="$(cd "$REPO" && G delve tangle --scope "$REPO2")"
if echo "$OUT" | grep -q '"definitions":\[{'; then
    ok "delve --scope <path> selects another repo (not CWD)"
else
    bad "delve --scope path (got: $OUT)"
fi
OUT="$(cd "$REPO" && G delve tangle --ref "$REPO2_SHA")"
if echo "$OUT" | grep -q '"definitions":\[{'; then
    ok "delve --ref <sha> selects another repo by portable identity"
else
    bad "delve --ref sha (got: $OUT)"
fi
OUT="$(cd "$REPO" && G literal tangle --scope "$REPO2")"
if echo "$OUT" | grep -q 'tangle'; then
    ok "literal --scope <path> selects another repo"
else
    bad "literal --scope path (got: $OUT)"
fi
# --scope by bare NAME (repo2's dir basename) must resolve for a SHA-keyed index — the
# name lives in the checkout PATH (ref_sightings), matched via index_locations(), NOT in
# the SHA idx. Regression guard: before the fix a bare name never matched a SHA-keyed
# repo and silently fell through to a parent-dir union.
REPO2_NAME="$(basename "$REPO2")"
OUT="$(cd "$REPO" && G delve tangle --scope "$REPO2_NAME")"
if echo "$OUT" | grep -q '"definitions":\[{'; then
    ok "delve --scope <bare name> selects a SHA-keyed repo (name->path resolution)"
else
    bad "delve --scope bare name on SHA-keyed idx (got: $OUT)"
fi
# And the bare name ISOLATES: tangle is in repo2 only, so metrics --scope repo2-name
# from REPO must show calc.ts and NOT orders.ts.
OUT="$(cd "$REPO" && G metrics --scope "$REPO2_NAME")"
if echo "$OUT" | grep -q 'calc.ts' && ! echo "$OUT" | grep -q 'orders.ts'; then
    ok "metrics --scope <bare name> isolates the SHA-keyed repo (no leak)"
else
    bad "metrics --scope bare name isolation (got: $OUT)"
fi
# NEGATIVE scope isolation: `tangle` lives ONLY in repo2. Querying it scoped to REPO's
# PATH (where it does NOT exist) must return EMPTY — proves scoping actually bites, not
# that the query just happens to return rows. links call-graph + metrics both exclude
# the foreign repo.
OUT="$(cd "$REPO" && G links tangle --kind call --scope "$REPO")"
if echo "$OUT" | grep -q '"count":0'; then
    ok "links scoped to the WRONG repo returns empty (scope isolation)"
else
    bad "links scope isolation leaked (got: $OUT)"
fi
OUT="$(cd "$REPO" && G metrics --scope "$REPO")"
if echo "$OUT" | grep -q 'orders.ts' && ! echo "$OUT" | grep -q 'calc.ts'; then
    ok "metrics --scope <repo path> excludes the other repo (scope isolation)"
else
    bad "metrics scope isolation leaked (got: $OUT)"
fi
# --index must be GONE: delve rejects it (CLI11 unknown option).
OUT="$(cd "$REPO2" && G delve tangle --index whatever 2>&1)"; RC=$?
if [ $RC -ne 0 ]; then ok "delve --index rejected (relic removed)"; else bad "delve --index still accepted"; fi

# 0b2) BAD --scope PATH is an ERROR, not a silent whole-store union (the `ls <arg>` rule).
#      A path-shaped scope (absolute or containing '/') that doesn't exist on disk must
#      fail with exit!=0; a BARE NAME miss must NOT error (name lookup falls through).
OUT="$(cd "$REPO" && G metrics --scope /no/such/path 2>&1)"; RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q 'does not exist'; then
    ok "--scope <nonexistent abs path> errors (not a silent union)"
else
    bad "--scope bad abs path (rc=$RC got: $OUT)"
fi
OUT="$(cd "$REPO" && G delve tangle --scope bogus/sub/dir 2>&1)"; RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q 'does not exist'; then
    ok "--scope <nonexistent relative path> errors"
else
    bad "--scope bad relative path (rc=$RC got: $OUT)"
fi
OUT="$(cd "$REPO" && G metrics --scope nosuchreponame 2>&1)"; RC=$?
if [ $RC -eq 0 ]; then ok "--scope <bare name miss> does NOT error (name lookup)"; else bad "bare-name miss errored (rc=$RC got: $OUT)"; fi

# 0c) PARENT-DIR UNION: from the SANDBOX root (a non-git parent of repo + repo2), a
#     read verb must union BOTH child repos — not fall back to the global store or a
#     single repo. metrics from the parent surfaces functions from BOTH repos.
OUT="$(cd "$SANDBOX" && G metrics --limit 50)"
if echo "$OUT" | grep -q 'orders.ts' && echo "$OUT" | grep -q 'calc.ts'; then
    ok "metrics from a non-git parent unions child repos"
else
    bad "parent-dir union metrics (got: $OUT)"
fi

# 1) index the seeded repo (configless git-identity indexing).
OUT="$(cd "$REPO" && G index .)"
if echo "$OUT" | grep -q '"index"' && echo "$OUT" | grep -q '"entries"'; then
    ok "index seeds the store"
else
    bad "index (got: $OUT)"; echo "$OUT" >&2
fi

# 2) status — JSON store object with an entries count > 0 after indexing.
OUT="$(cd "$REPO" && G status)"
if echo "$OUT" | grep -q '"entries"'; then ok "status reports store"; else bad "status (got: $OUT)"; fi

# 3) recall — semantic search returns at least one JSON hit (no error).
OUT="$(cd "$REPO" && G recall "process an order" --top 5)"
if echo "$OUT" | grep -q '"id"' && ! echo "$OUT" | grep -q '"error"'; then
    ok "recall returns hits"
else
    bad "recall (got: $OUT)"
fi

# 4) delve — deep-dive processOrder over REAL indexed code. After the symbol-mapping
#    fix, the symbol->entry map (cache_symbols) is keyed off the chunk HEADING (the
#    real function name from tree-sitter), NOT the enriched embed text's leading
#    directory token. So delve now composes BOTH the call graph (callers/callees) AND
#    the `definitions` array (ids_for_symbol -> the defining chunk). We assert all
#    three are non-empty here: retryOrder calls processOrder (caller); processOrder
#    calls validate + persist (callees); and processOrder resolves to its own
#    definition. This is the regression guard for the symbol-mapping bug over the
#    real ingest pipeline (the unit test test_store_symbol_from_heading guards the
#    extraction in isolation).
OUT="$(cd "$REPO" && G delve processOrder)"
if echo "$OUT" | grep -q '"symbol":"processOrder"' \
   && echo "$OUT" | grep -q '"callers":\[{' \
   && echo "$OUT" | grep -q '"callees":\[{' \
   && echo "$OUT" | grep -q '"definitions":\[{' \
   && echo "$OUT" | grep -q 'retryOrder' \
   && echo "$OUT" | grep -q 'validate'; then
    ok "delve composes definitions + callers + callees over real ingest"
else
    bad "delve composition (got: $OUT)"
fi

# 5) links — call graph in-edges to processOrder include retryOrder (its caller).
#    Output must be a SINGLE wrapped JSON object {"node","links":[...],"count"},
#    NOT newline-delimited objects (the shape bug fixed alongside metrics).
OUT="$(cd "$REPO" && G links processOrder --kind call --direction in)"
if echo "$OUT" | grep -q 'retryOrder'; then ok "links finds caller"; else bad "links (got: $OUT)"; fi
if echo "$OUT" | grep -q '^{"node":' && echo "$OUT" | grep -q '"links":\[' \
   && echo "$OUT" | grep -q '"count":' && [ "$(echo "$OUT" | grep -c '^{')" = "1" ]; then
    ok "links emits one wrapped JSON object (not NDJSON)"
else
    bad "links output shape (got: $OUT)"
fi

# 6) metrics — complexity ranking. Two regressions guarded here:
#    (a) SHAPE: one wrapped object {"functions":[...],"count":N}, not NDJSON (was a
#        stream of bare {...} lines).
#    (b) CWD SCOPE: run with NO filter and NO --index from inside $REPO, it must rank
#        ONLY this repo's functions (processOrder/retryOrder), scoped to the CWD's idx
#        — not the whole store. We assert the known functions appear and the output is
#        a single valid object.
OUT="$(cd "$REPO" && G metrics orders.ts)"
if echo "$OUT" | grep -q '^{"functions":\[' && echo "$OUT" | grep -q '"count":' \
   && [ "$(echo "$OUT" | grep -c '^{')" = "1" ]; then
    ok "metrics emits one wrapped JSON object (not NDJSON)"
else
    bad "metrics output shape (got: $OUT)"
fi
OUT="$(cd "$REPO" && G metrics)"
if echo "$OUT" | grep -q 'processOrder' && ! echo "$OUT" | grep -q '"error"'; then
    ok "metrics (no filter) scopes to the CWD repo"
else
    bad "metrics CWD scope (got: $OUT)"
fi

# 7) remember — author a note, capture its id; forget — remove it by id.
OUT="$(cd "$REPO" && G remember "orders flow: processOrder validates then persists")"
NOTE_ID="$(echo "$OUT" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
if echo "$OUT" | grep -q '"type":"note"' && [ -n "$NOTE_ID" ]; then
    ok "remember stores a note"
else
    bad "remember (got: $OUT)"
fi
if [ -n "$NOTE_ID" ]; then
    OUT="$(cd "$REPO" && G get "$NOTE_ID")"
    if echo "$OUT" | grep -q '"type":"note"'; then ok "get fetches the note by id"; else bad "get (got: $OUT)"; fi
    OUT="$(cd "$REPO" && G forget "$NOTE_ID")"
    if echo "$OUT" | grep -q '"ok":true'; then ok "forget removes the note"; else bad "forget (got: $OUT)"; fi
else
    bad "get/forget skipped (no note id)"
fi

# 8) OUTPUT SHAPE across ALL read verbs + related/get coverage. Every read verb must
#    emit ONE wrapped JSON object so a consumer can json.load the whole stdout; a verb
#    that streams one bare {...} per line (NDJSON) is a shape regression (the class of
#    bug fixed for metrics + links, and found lingering in `related`). We check
#    recall/literal/metrics/links/related/delve/get — the full read surface.
echo "[output shape — every read verb is one JSON object]"
one_json_object "$(cd "$REPO" && G recall "process an order" --top 3)"        "recall"
one_json_object "$(cd "$REPO" && G literal processOrder)"                       "literal"
one_json_object "$(cd "$REPO" && G metrics)"                                    "metrics"
one_json_object "$(cd "$REPO" && G links processOrder --kind call)"             "links"
one_json_object "$(cd "$REPO" && G delve processOrder)"                         "delve"

# related — needs a real entry id. Pull one from recall, then `related` it. Assert the
# wrapped-object shape (the NDJSON holdout) AND that it returns semantically-near rows.
REL_ID="$(cd "$REPO" && G recall "process an order" --top 1 | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | head -1)"
if [ -n "$REL_ID" ]; then
    OUT="$(cd "$REPO" && G related "$REL_ID" --top 3)"
    one_json_object "$OUT" "related"
    if echo "$OUT" | grep -q '"results":\[' && echo "$OUT" | grep -q '"count":'; then
        ok "related returns a results array with a count"
    else
        bad "related shape/content (got: $OUT)"
    fi
    # get on a CODE entry (previously only notes were get-tested) — one object, type=code.
    OUT="$(cd "$REPO" && G get "$REL_ID")"
    one_json_object "$OUT" "get(code)"
    if echo "$OUT" | grep -q '"type":"code"'; then ok "get fetches a code entry by id"; else bad "get code (got: $OUT)"; fi
else
    bad "related/get(code) skipped (no recall id)"
fi

# --- Removed-surface assertions: legacy flags + the mem subcommand are GONE ---
echo "[removed surfaces]"
# The `mem` subcommand itself must be gone (CLI11 rejects an unknown subcommand).
OUT="$(cd "$REPO" && G mem 2>&1)"; RC=$?
if [ $RC -ne 0 ]; then ok "mem subcommand removed (exit $RC)"; else bad "mem subcommand still runs"; fi
for flag in --cutover --sync --categorize --categories --resync-disk; do
    OUT="$(cd "$REPO" && G mem "$flag" 2>&1)"; RC=$?
    if [ $RC -ne 0 ]; then ok "mem $flag rejected (exit $RC)"; else bad "mem $flag still accepted"; fi
done

# --- MCP surface: tools/list rename + delve; tools/call delve composition ---
echo "[mcp surface]"
MCP_REQ='{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
LIST="$(cd "$REPO" && printf '%s\n' "$MCP_REQ" | HOME="$FAKE_HOME" gtimeout "$TIMEOUT" "$GOGOL_BIN" mcp 2>/dev/null)"
if echo "$LIST" | grep -q '"name":"get"'; then ok "mcp tools/list has get"; else bad "mcp missing get (got: $LIST)"; fi
if echo "$LIST" | grep -q '"name":"delve"'; then ok "mcp tools/list has delve"; else bad "mcp missing delve"; fi
if ! echo "$LIST" | grep -q '"name":"retrieve"'; then ok "mcp tools/list drops retrieve"; else bad "mcp still advertises retrieve"; fi

# tools/call delve over MCP — must return the composed structure (callees non-empty).
CALL_REQ='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"delve","arguments":{"symbol":"processOrder"}}}'
CALL="$(cd "$REPO" && printf '%s\n' "$CALL_REQ" | HOME="$FAKE_HOME" gtimeout "$TIMEOUT" "$GOGOL_BIN" mcp 2>/dev/null)"
if echo "$CALL" | grep -q 'callees' && echo "$CALL" | grep -q 'processOrder'; then
    ok "mcp tools/call delve composes structure"
else
    bad "mcp delve call (got: $CALL)"
fi

echo ""
echo "cli_verbs: $PASS passed, $FAIL failed."
[ $FAIL -eq 0 ]
