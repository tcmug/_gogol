# gogol — design, decisions & graveyard

One durable record of what gogol *is*, what was **proven**, what was **tried and
killed**, and what is **designed but unbuilt**. Organized by status, not chronology.
When a decision changes, move the entry between sections rather than deleting it —
the graveyard is load-bearing (it stops us re-treading dead ends).

Legend: ✅ built & validated · 🧪 tested/measured · 🪦 tried & abandoned · 📐 designed, not built · ⚖️ principle

---

## 1. Thesis & core bet ✅

> **Embeddings are coordinates. Classification is a choice of basis. Paths are a
> derived serialization of the coordinates. Identity is a content hash.**

One unified store — code chunks, doc chunks, and LLM notes are all `entry`s with a
nomic embedding, a content-hash id, and index membership. Cross-index search is the
default; scoping adds a filter. **Proven** (three falsification experiments on real
read-only indexes): content-id dedup works (identical content grows the store by 1);
cross-index ranking works without per-db fan-out; a class is just a prototype entry
embedded through the same model.

The store is **fully deterministic except one semantic proposer** (the nomic
embedding for similarity). Everything else — filtering, membership, graph, metrics —
is deterministic SQL. *Statistics propose, the agent disposes.*

---

## 2. Data model (current, built) ✅

Two SQLite stores by durability class:
- **cache.db** (`~/.gogol/cache.db`) — DERIVED / re-derivable: code+doc entries +
  embeddings, the graph, metrics, FTS. Disposable: a schema-version bump drops +
  rebuilds it on open (no data migration).
- **notes.db** (`~/.gogol/notes.db`) — the note CONTENT of record + scope + embedding.
  NOT re-derivable; a cache rebuild never touches it.

cache.db tables:

| table | holds |
|-------|-------|
| `entries` | id (content hash) · content · embedding blob · source · `idx` (repo/index name) · line/end_line · symbol · inlined per-function metrics (complexity/lines/params/returns/max_depth) |
| `entry_sources` | worktree visibility junction: (entry_id, toplevel, source) — the same content in main + a worktree is ONE `entries` row with multiple `entry_sources` rows; a query scopes to a toplevel via this join |
| `file_hashes` | per-file stat-hash — the incremental-sync gate |
| `edges` | call / import / docref graph (kind, src_sym, dst_sym, idx, file) |
| `entries_fts` | FTS5 (content, symbol) external-content over `entries` — the keyword arm, trigger-synced |
| `entries_vec` | sqlite-vec `vec0` ANN index: `(id, embedding float[768] distance_metric=cosine)` — the semantic KNN arm (replaced the old in-memory brute-force scan) |

notes.db tables:

| table | holds |
|-------|-------|
| `notes` | id (content hash) · content · embedding blob · scope_path · created |
| `notes_vec` | sqlite-vec `vec0` ANN index for notes (same pattern as `entries_vec`) |

Entry types are **derived from source**, not stored: `note` = source NULL, `doc` =
`*.md`, `code` = other. Identity ≠ address ≠ membership.

**Invariants** ⚖️
- id is a content hash and never changes; dedup is automatic.
- an entry carries a primary `idx` (repo/index name); **worktree visibility** is a
  junction (`entry_sources`): one content row, one row per (toplevel) that holds it.
  Scoping to a checkout is an `entry_sources` join; index scoping is `WHERE idx=?`.
- symbol + metrics are columns ON `entries` (attached to the chunk at chunk
  time), so entry deletion removes them inherently — no orphan cleanup, no fuzzy join.
- reads open the store **read-only** (lock-free during a concurrent sync).
- an **ignored file == a deleted file**: both are wiped from the index on sync.

---

## 3. Retrieval ✅

Hybrid: semantic cosine (nomic, in-memory) + FTS5 keyword, fused via a **weighted
blend** ("A3"): `score = 0.6·cosine + 0.4·bm25_norm`, where bm25 (FTS5 rank) is
min-max normalized within the result set and a hit present in only one arm scores 0
for the missing arm. Then call-graph expansion of the top hits. Cosine runs **in SQL**
via the `entries_vec`/`notes_vec` `vec0` KNN index (`embedding MATCH ? ORDER BY
distance`, `similarity = 1 − distance`). Metadata filters (`--type`, `--scope`/`--ref`,
scope) are applied as a **pre-filter on the candidate id-set**: the KNN over-fetches
(`k·4`) and the allowed-id set drops non-matches before the top-k is cut — never a
post-filter on a tiny top-k (that starved rare types like notes: 299 notes lost
to 40k code/doc in the global top-k).

- `--type note|doc|code` and `--scope`/`--ref` restrict the *search space*.

The blend replaced plain RRF (k=60) after an 8-trial known-item eval: recall@1 +19pts,
MRR +0.10. RRF discards magnitude, and when cosine scores cluster tightly (observed avg
top-10 spread ~0.077) that magnitude is exactly what separates near-ties. Weights are
tunable constants (`W_SEM`/`W_KW`) in `query_hybrid`. (RRF is retired — see graveyard §9.)

---

## 4. Code intelligence ✅

Tree-sitter (TS/TSX, Python, Go, Rust, C, C++, PHP) extracts, at ingest:
- **call / import / docref graph** → `cache_edges`, navigable via `links` (kind,
  direction, depth).
- **per-function complexity** → inlined columns on `cache_entries`
  (complexity/lines/params/returns/max_depth, attached at chunk time), rankable.

Known limitation 🪦→📐: **HOF-argument call arrows** — a call passed as a lambda
argument (`wrapper(() => foo())`) attributes to the binding, not always the true
caller. Partial pass exists; full resolution deferred (common-name collisions make it
non-trivial).

---

## 5. Incremental sync ✅

Was O(whole corpus) every run (~638s) — re-embedded everything. Now proportional to
change (~3–7s):
- **stat-hash gate** (`file_hashes`): unchanged files (mtime+size match) are skipped
  entirely — no re-chunk, no embed.
- **embed-skip**: for a changed file, unchanged chunks (same content-id) refresh
  membership/location without re-embedding.
- **reconcile**: a stored file is removed if it's deleted **or** now `.gogolignore`d
  (ignored == gone).

---

## 6. Categories — term-tags 🪦 (built, measured, then REMOVED)

**Status: removed from the codebase.** This was built and it *passed* the
retrieval-signal bar (unlike projection-classification, §9) — but the whole
category/term-tag subsystem (L1/L2 extraction, the soft boost, `--categories`, the
`categorize` path) was later excised along with the classifier cleanup. No term-tag
code remains in `src`. Kept here as **knowledge** (what was measured, and why it
worked) so the mechanism isn't misremembered or blindly rebuilt. If reintroduced, it
re-enters as a *new* build, not a resurrection.

What it was — two levels:
- **L1** (per-entry, incremental, deterministic): top tf-idf distinctive terms.
- **L2** (batch): candidate terms (≥ ~0.5% of corpus) → term-vector (mean embedding
  of entries carrying the term) → agglomerative merge at a **data-derived knee**
  threshold (sweep down, cut just above where the largest group exceeds ~40% of
  terms — not a hardcoded constant).

Was used as a **soft boost** (up-rank tag-mates) + a browsable map (`--categories`).

**Measured** 🧪 (why it cleared the bar): tag-mate note pairs cohere (0.789 vs 0.658
random); a note's tag-mates were **15.5× enriched** among its true top-10 neighbors;
end-to-end boost +0.13 MRR (notes), +0.10 (docs at matched selectivity). The winning
mechanism was **lexical term-overlap**, not embedding projection — which is *why* it
worked (and why it was distinct from the falsified classification axes in §9).

**Calibration (data-derived, not magic):** `kmin ≈ 0.5% of N` and `topN = 3` were
found by sweep to generalize across corpus sizes (notes N=299 → kmin 3; docs N=7304
→ kmin ~36); `1%`/`sqrt(N)`/fixed-target did not. The knee sharpens with scale.
Gate: skip categorization below ~30 candidate terms (no stable structure).

Bounds ⚖️: it was **soft boost, never a hard filter** (only ~25% of a note's true
neighbors share its tag → a hard filter would drop ~75% of relevant). Residual at
removal time: tail noise (test-note terms like `concurrency`, generic `todo`).

---

## 7. Designed, not built 📐

- **Lazy supersession / conflict resolution** — content-hash identity means a
  *revised* note gets a new id and coexists with the stale one; `recall` returns both.
  Planned fix: supersession is **declared** (`supersede(A,B)` / version lineage) or
  concluded from provenance — never blindly detected. Evidence hierarchy: source
  version identity → explicit revise() → lazy evidence. `forget` is the manual stopgap.
- **Note→code anchors** — notes carry no non-circular link to the code they document
  (dissolved as a benchmark crutch, see §9). If note-recall quality ever needs
  offline measurement, anchors are the prerequisite.
- **Term-based query→category matching** — moot while categories are removed (§6).
  The former category boost matched the query string against category *names*
  (substring); the richer idea was to match the query's *derived terms* against
  category member terms (would catch login→kirjautuminen). Only relevant if the
  category subsystem is ever rebuilt.
- **Hierarchy** — recursive L2 (categories → super-categories) in *term* space, if a
  corpus outgrows a flat category set. NOT embedding-hierarchy (falsified, §9).

---

## 8. Migration (v1 → v2) ✅

v1 is fully retired and the one-time `gogol mem --cutover` bridge has been removed
(there is no `mem` subcommand anymore). What remains is ordinary schema evolution for
existing v2 stores: `cache.db` is derived, so a schema-version mismatch simply drops +
rebuilds it on open (no data migration); `PRAGMA user_version` stamps the version, and
a read-only open self-migrates once (brief RW) if the schema is stale, then stays
lock-free. `notes.db` (content of record) is never rebuilt.

---

## 9. Graveyard — tried & killed 🪦

Do not rebuild these without a genuinely new mechanism.

- **RRF (Reciprocal Rank Fusion, k=60) as the hybrid fuser** — REPLACED by the A3
  weighted blend (§3). RRF fuses on *rank position* only, discarding score magnitude.
  On this corpus cosine scores cluster tightly (avg top-10 spread ~0.077), so the
  magnitude is exactly what separates near-ties — RRF threw it away. The weighted
  blend (`0.6·cosine + 0.4·bm25_norm`, bm25 min-max normalized in-set) won an 8-trial
  known-item eval: recall@1 +19pts, MRR +0.10. *Kept as knowledge:* rank-only fusion
  is the wrong tool when both arms produce calibrated magnitudes worth preserving.

- **Classification as a query router / scoping filter** — FALSIFIED. 56% of answers
  carry no axis at projection≥0.80, so a hard filter drops half the relevant *before
  search runs* — disqualifying regardless of survivor performance. And a CLOSED
  FAMILY: membership was embedding-projection, so *any* soft scoping collapses into
  plain vector search (routing through a centroid discards information). Verdict:
  **classifications are a map, never in the recall path.** Fully excised (FINCH
  detect, axes, ~2180 classifiers, the idle-task, `mem_simd`). The lexical **term-tag
  categories** that *did* pass the bar (§6) were later removed too (soft boost,
  `--categories`, L1/L2 extraction all gone) — kept as knowledge in §6, not in code.
  *Kept as knowledge:* FINCH found real structure; the SIMD detect kernel hit 680s→56s
  (`-ffast-math` vectorized the dot-reduction, verified exact); tf-idf cluster naming
  ~70% clean.

- **LSA / SVD as an embedding-free semantic layer** — FALSIFIED (N=504). hit@5 0.29
  vs BM25 0.70, CIs disjoint; behind raw TF-IDF too. SVD compression destroys the
  distinctive terms that carry retrieval — a mechanism failure, not a tuning one. Raw
  TF-IDF cosine also retired (adds nothing over BM25).

- **Incremental FINCH clustering** — FALSIFIED. 89% recall / 70% precision vs a full
  recompute; global topology doesn't incrementalize. Always full-recompute (the SIMD
  kernel made it cheap). Projection-type work incrementalizes; topology-type does not. ⚖️

- **Quality-estimation via embedding** — FALSIFIED. Embeddings can't measure code
  quality/complexity; that signal comes from AST metrics, not vectors.

- **Content-normalization before embedding** (stripping markdown markup) — NOT worth
  it: 0.670 vs 0.674 baseline. Pollution wasn't hurting retrieval.

- **Note→code anchors as measurement infra** — DISSOLVED. Invented to manufacture
  offline ground truth for a note→code eval, then realized its product (verified
  relations) duplicates what the agent loop already does for free by reading a note
  before following its reference. In an agentic loop, offline precomputed trust is
  usually redundant with online in-context verification. ⚖️

- **rel_path / path projection / rebase-without-reindex** — explored, DECLINED.
  `source` is the staleness + edge-cleanup join key; a derived/projected-path model
  sprawled across entries+edges+metrics for little concrete gain. **Reindex is the
  accepted price for a dir move.** (The good part — index-as-first-class-table + the
  typed junction — was kept; the projection was not.)

- **Term-expansion table (identifier→related-identifiers, mined from nomic geometry)** —
  FALSIFIED via a gated P1→P2 probe (ran the plan instead of trusting the prior
  "projection collapses" conclusion — correct call, the measurement is now airtight).
  P1 (identifier-neighbor probe, distractor-negatives, pre-registered kill): WEAK PASS —
  21% distractor-leak, 1/6 inverted; nomic *appears* to have real single-token geometry
  at top-k. P2 (build the 467-symbol table with a data-derived noise floor = median+3·MAD
  per symbol, no magic constant): DECISIVE KILL. Once the noise band is honestly gated,
  the 1753 surviving neighbors decompose into **50% lexical** (feature_flag→feature_flags,
  testing→tests — which FTS/BM25 already does for free) and **49% "semantic"** that is
  actually NOISE (husky→hero, terraform→forms, infra→invoice — spurious prefix/phonetic
  coincidences, not domain relations). The genuine bridges hoped for (settle_orders→
  orderHash) sat in the 0.60-0.71 band the robust floor correctly flagged as
  indistinguishable from noise → gated out. So P3's A/B was unnecessary: vs BM25 the table
  is redundant (lexical half) or harmful (noise half), and the note→code semantic class it
  was built for is EMPTY after honest gating. **P1 nearly fooled us (top-k looked real);
  P2's noise-gating caught it — which is why the plan front-loaded the cheap probe but
  didn't stop there.** Confirms the term-granularity form of "a relatedness idea must use
  a NON-projection signal or it collapses into the vector search you already have" — now
  proven, not just reasoned. (term-tags passed this bar precisely because they're lexical
  tf-idf, not geometric.)

- **RM3 / pseudo-relevance-feedback query expansion** — FALSIFIED at Step 0 (pre-registered
  defaults k=10/m=10/λ=0.5, one run, no tuning). **−0.040** (0.532→0.492 hit@5) on the
  reproducible harness. AUDITABLE why (RM3's whole pitch): the reproducible query
  construction on this corpus is body-excerpt→find-its-own-chunk, so the query ALREADY
  contains the answer's exact terms; PRF then harvests expansion terms from the *other*
  top-k chunks and injects competing vocabulary, diluting the exact-match signal → true
  chunk drops. RM3 is corpus-native/live/deterministic/free — and still loses, because
  gogol's measurable task has NO expansion headroom (it's already a lexical-match task
  BM25 aces at 0.53). The class RM3 WOULD help (short/vocab-gap queries) scores 0.02
  (heading-only) — too sparse to be a viable measurement base for anything.
  **Harness-reproducibility finding (reframes all expansion arms):** the recorded "~0.70"
  baseline was a CURATED-corpus body-excerpt number; rebuilt on the live store it is
  **0.53** (body-excerpt) / 0.02 (heading-only) — and Step 0 proved the excerpt task has
  no headroom for expansion of ANY kind. This removes the shared-contract footing for
  doc2query (doc-side expansion can't help a query-already-matches-chunk task; risks the
  index-bloat dilution) and SPLADE (was already guilty by domination). Lesson reinforced:
  **Step 0 before code caught a 27× baseline error** (heading-only reconstruction gave
  0.026) — the harness had to be reproduced before any arm could run.

- **sqlite-vec for all-pairs 1-NN** — ⤴️ REINSTATED (no longer a tombstone; see §2/§3).
  Originally removed: at the time it was EXACT but SLOWER than a blocked in-memory GEMM
  at this scale, and its only consumer (detect KNN) had been retired. It was later
  brought back as the **primary semantic arm** — `entries_vec`/`notes_vec` `vec0`
  indexes (`float[768] distance_metric=cosine`), schema v2. The original "kept as
  knowledge" note is now the actual design: vec0 filtered-KNN lets SQL do
  `embedding MATCH ? ORDER BY distance` and compose the metadata pre-filter on the
  candidate id-set — cosine now lives IN SQL, not in a C++ matrix loop.

---

## 10. Load-bearing lessons ⚖️

- **Test the premise before building the consumer.** Classification, LSA, anchors,
  projection — each looked reasonable and each died on contact with a cheap
  pre-registered experiment. Measure the signal, size the lift, name the kill
  condition first.
- **Right mechanism beats more machinery.** Term-tags (lexical) passed the exact bar
  embedding-projection axes failed. When a scoping/relatedness idea returns, it must
  use a *non-projection* signal or it collapses into the vector search you already have.
- **No magic constants** — derive thresholds from the data (the category knee, the
  0.5%-of-N kmin, min-support). A fixed count that works at one corpus size breaks at
  another.
- **Soft over hard in an agentic loop** — boosts (recoverable, self-correcting) over
  filters (fatal, drop-the-answer). The loop reads before proceeding; design for that.
- **Incremental where topology allows, full-recompute where it doesn't.** Know which.
- **Deterministic core, one proposer.** Keep the smart/statistical surface minimal and
  policed by the consumer, not by benchmarks.

---

## 11. Embedding model — swept, two designated fallbacks 🧪

The embedder is now model-agnostic (two bugs fixed: `n_ctx` read from the model, and
token truncation to `min(n_ctx, n_ubatch)` — the encoder requires the whole sequence
in one micro-batch). A/A sweep on gogol src (each model embeds the corpus + query in
its OWN dim, self-ranked), N=5 code queries, hit@5:

| model | dim | ms/chunk | vs nomic | hit@5 |
|-------|-----|----------|----------|-------|
| minilm-22M | 384 | 6 | 6× faster | **0/5** |
| bge-small-33M | 384 | 11 | 3.4× | **0/5** |
| **bge-base-109M** | 768 | 21 | **1.8× faster** | 3/5 |
| nomic-137M (current default) | 768 | 37 | 1× | 3/5 |

**Findings:**
- **Dimension is the quality cliff, not param count.** 384-d models are 3–6× faster but
  retrieve NOTHING on code (0/5) — the drop from 768-d collapses the signal. **Rule:
  dim ≥ 768. The 384-d family is CLOSED** (cliff).
  **⚠️ CORRECTION (N=20 via sg, bootstrap): the dim-cliff is FALSIFIED — it was an N=5
  artifact.** Re-tested on 20 vocab-gap known-answer queries over the backend repo (RAW
  chunks, correct prefixes), bge-small (384-d) scored 15/20 hit@3 — the BEST raw number,
  not 0/5. Bootstrap (5000 resamples): all four embedders (nomic-Q4 12, nomic-Q8 14,
  bge-base 14, bge-small 15) are WITHIN NOISE of each other (95% CIs ≈[0.4,0.9]; every
  pairwise diff crosses zero). So: (a) 384-d does NOT collapse — the "0/5" was the tiny-N
  isolated sweep, same failure mode as the bge "tie". (b) The four embedders retrieve
  EQUIVALENTLY on raw code chunks; the only beyond-noise gaps are BM25 (5/20, worse) and
  SPEED (bge-small ~4×, bge-base ~2.6× faster than nomic). Retire the "dim≥768 bar" as a
  quality rule — it was never measured beyond N=5. CAVEAT: raw-chunk sg, NOT enriched
  gogol; bge's 512 ctx still truncates enriched chunks, so the nomic-enriched vs bge-raw
  fork (the one that decides gogol's actual default) is still unrun.
- **`bge-base` sweep "tie" was an ARTIFACT — real-store verification FAILED.** The A/A
  sweep (isolated, N=5, same wrong prefix on both sides) showed 3/5 == nomic. But a
  full real-store re-index + real queries was DEGENERATE: `embedder.cpp` ranked #1 for
  every query at ~0.41, scores then cliffed to ~0.05, the right code never in top-5.
  ROOT CAUSE: the embedder HARDCODES nomic's instruction prefixes (`search_document: `
  / `search_query: `, embedder.cpp); bge models need a DIFFERENT prefix (`Represent this
  sentence for searching relevant passages: `). Wrong prefix → miscalibrated space →
  broken ranking. The isolated A/A masked it (both sides shared the wrong prefix, so
  relative order partially held); real queries exposed it. Restored nomic from backup.
  **LESSON: N=5 isolated self-comparison cannot catch systematic prefix
  miscalibration — model verification MUST be a full real-store re-index + real queries.**
- **bge-base as a speed fallback is now GATED behind a prerequisite:** make the embedder
  PREFIX model-aware (per-model query/passage instructions) — a third model-agnostic gap
  alongside n_ctx and n_ubatch. Until then, bge-base (or any non-nomic model) is NOT a
  drop-in — the ~1.8× speedup is real but unusable without correct prefixes.

**UPDATE — bge-base FALSIFIED as a fallback (real-store verification, twice).** After
building model-agnostic prefixes, re-verified bge-base on the FULL real store with its
CORRECT prefix (`Represent this sentence...: `, doc empty) AND correct pooling (its gguf
declares CLS, applied by llama). STILL degenerate (0/5 — short files like mem_db_path/
embedder.h rank #1 for unrelated queries; real code never surfaces). Ruled out prefix
(verified applied) and pooling (verified CLS from metadata). Residual cause likely the
enriched-chunk × 512-context interaction (gogol's long enriched chunks truncate at
bge's 512 limit, keeping only the preamble). VERDICT: bge-base is NOT a viable drop-in
for gogol's store, regardless of the isolated sweep's tie. The prefix fix (commit) is a
real model-agnostic improvement but was NOT sufficient. **nomic remains the default and
the only verified-good model.** LESSON (proven twice): isolated N=5 self-comparison
CANNOT validate a model — only a full real-store re-index + real queries can; the sweep
said "tie," reality said "broken."

**CORRECTION (via `sg`, a third venue) — the real-store failure was the ENRICHMENT/512
confound, NOT the model.** Tested bge-base through `sg --backend embed` (ad-hoc, RAW
chunks, correct prefixes) on `services/backend/checkout+pricing-engine`, N=5 vocab-gap,
same chunks as nomic: **bge-base 5/5 (all top-2, HEALTHY score spread 0.717→0.629, no
cliff) vs nomic-Q4 3/5.** bge did NOT degenerate on raw chunks — it beat nomic. The
reconciling mechanism, now confirmed at the metadata level: **bge context_length = 512,
nomic = 2048.** gogol's ENRICHED embed text (dir-context + sibling headings + code +
split identifiers) exceeds 512 tokens → bge truncates to the enrichment preamble, losing
the code → the real-store degeneracy. sg embeds raw chunks that fit in 512 → bge works.
So the "falsified" verdict mis-attributed a PIPELINE (enrichment sized for nomic's 2048)
failure to the MODEL. **bge-base is a competent retriever; it fails only on nomic-sized
enriched text.** Reopens bge as a candidate (NOT a decision — N=5): usable as an sg
backend on raw chunks today; to use in gogol proper would need enrichment trimmed to
<512 tok (or 512-aware). Meta-lesson SHARPENED: "isolated N=5 can't validate" was right,
but the real-store test ALSO had a confound (enrichment) — **every venue has a confound;
the fix is to test across venues and reconcile, which is how the true cause surfaced.**

### Keeping nomic fast — speed-lever checklist (DEMAND-GATED: cold-cache reindex is a
### ~2-min one-time event, below the bar; run these ONLY if the trigger fires —
### frequent fresh indexes / a fast-growing repo). Ordered, with what's already known:

1. **Batching — WORKLOAD-DEPENDENT (not universally "spent"; corrected by hypothesis
   testing).** Two regimes, both measured on nomic/M3 Pro:
   - **Large chunks (~373 tok, e.g. code): ~1.0×.** COMPUTE-bound — verified by H1: a
     ~2-tok query takes 5.2 ms, a ~370-tok chunk takes 35.0 ms (7× scaling with tokens).
     So a batch of ~5 chunks does the same ~5×370 tok of attention as 5 sequential calls;
     packing changes packaging, not FLOPs (n_ubatch=n_ctx ⇒ one micro-batch). No win.
   - **Small chunks (~40 tok, e.g. notes/terms/short docs): ~2.3×.** FIXED-COST-bound —
     verified by H5: 40 small chunks ran 5.60 ms/chunk sequential vs 2.39 ms/chunk batched
     (2.34×). The ~5 ms/call fixed cost (setup + memory_clear + GPU sync, per H1) amortizes
     across a packed batch when compute per chunk is small.
   The earlier "1.0×, spent" was measured ONLY on the real store's large code chunks and
   wrongly stated as universal. gogol's corpus is code-DOMINANT (35.8k code vs ~5k
   note/doc), so the BLENDED reindex win from batching is small — but it is NOT zero, and
   a notes-heavy or small-doc index WOULD benefit ~2×. **Pipeline audited (do NOT "fix"):**
   `indexer.cpp` (disk) ALREADY batches (`embed_documents_batch`). `resync_disk_index` +
   `ingest_notes_from` embed one-at-a-time, BUT: (a) resync's chunks are large code
   (batch=1.0×) AND it has an embed-skip gate (unchanged id ⇒ no embed), so batching it
   gains ~nothing and would fight the skip logic; (b) note-ingest IS small-chunk (would be
   ~2.3×) but it's ~302 notes ingested rarely ⇒ ~1s saved once, below the bar. So the
   finding is real but NOT actionable — leave the code, keep the mechanism documented.
   (The `min(n_ctx,n_ubatch)` fix lives in this layer. H2 tested + FALSIFIED:
   `llama_memory_clear` is NOT removable —
   dropping it crashes repeated encodes; it's load-bearing, and cheap (~within the 5 ms).)
2. **Quantization — already at the fast end.** nomic runs Q4_K_M (80MB). Going to Q8
   is *bigger/slower* (wrong direction); going below Q4 risks retrieval wobble. So
   there's no quant headroom left downward without quality risk. Effectively spent.
3. **Pooling/context-overhead config audit — AUDITED, clean.** ctx params checked:
   pooling defers to gguf metadata correctly (nomic=mean); n_seq_max=8 over-allocates
   memory (not speed) for the single-embed path (harmless); n_ctx=trained is correct.
   No prompt-cache/cont-batching overhead. No free speed here.
4. **GPU/BLAS — AUDITED + VERIFIED, already MAXED.** Not just "default n_gpu_layers=-1
   should offload all" — confirmed via llama's actual load report on the M3 Pro:
   `offloaded 13/13 layers to GPU`, `MTL0_Mapped model buffer = 79.49 MiB` (the whole Q4
   model on Metal); the 12.58 MiB left CPU-mapped is only the token-embedding lookup
   table (a gather, not a compute layer — correct to leave). nomic is FULLY GPU-offloaded;
   37ms/chunk IS the GPU number. Nothing to gain — proven at the layer level.
5. **Cross-model index/query (bge-index + nomic-query) — CLOSED.** Different models =
   incompatible geometries even at same dim (this is *why* bge-base failed). Not allowed.

**Ledger note (audited):** ALL five levers are now spent or already-optimal — batching
(1.0× large / 2.3× small — workload-dependent), quant (already Q4), ctx config (clean), GPU (already full-offload),
cross-model (closed). **nomic's 37ms/chunk is the genuine hardware floor**, fully
GPU-offloaded and correctly configured. The ONLY remaining speedups are a different
model (bge-base — falsified) or architectural (embed less: prefilter / lazy / coarser
chunks). Since cold-reindex is a ~2-min one-time event (below the bar), the honest
answer is: leave it. There is no cheap win left to find — this was verified, not assumed.

**Nomic model-FAMILY evaluated — verdict: nothing changes (demand-gated).** Surveyed the
family for a better nomic: (a) **nomic-v1.5 (MRL / Matryoshka)** = "nomic made resizable"
— the one genuine new lever, but it optimizes STORAGE/QUERY cost (truncatable dims), a
cost gogol doesn't have: ~3k chunks × 768-d ≈ 9MB of vectors, the vec0 KNN scan is
trivial. No demand. (b) **v2-moe** = quality-tier shopping with no demand signal (nomic
already retrieves correctly). (c) **distills** = the falsified small-model family in
miniature (dim cliff below 768). **The one pre-approved footnote:** *if the corpus ever
grows ~100× (→ ANN scan / memory pressure becomes real), nomic-v1.5 MRL truncation is the
designated storage lever — but retest the dim cliff at 512-d before adopting* (MRL trades
dims for cost; must confirm 512-d still clears the retrieval bar on the real store, per
the twice-proven lesson that isolated N=5 can't validate — full re-index + real queries).
Until that trigger, the family verdict is: **stay on nomic-Q4 as-is.**

- Speed already has an answer, so any NEW model must be justified by **quality
  headroom, not speed**, and only above the dim≥768 bar.

**`jina-embeddings-v2-base-code` = the QUALITY probe (designated, not yet run).** 768-d,
CODE-trained — the one candidate that could plausibly *beat* nomic on the identifier/
jargon axis rather than tie it. It's the only unrun experiment with upside, because
it tests a *different axis* (code-training) rather than redrawing at the general-
embedder well. IF the semantic path ever visibly misses things it shouldn't → probe
this first, same harness, one evening. Demand-gated (a loop-trace question, not model-
shopping); N=5 directional is fine here (0/5 or 5/5 is informative; another 3/5 is not).

**Closed — do not model-shop:** the 384-d family (cliff); other 768-d GENERAL embedders
(e5-base, gte-base — would tie nomic/bge-base like bge-base did; N=5 can't distinguish
768-d generals, so testing them is motion without information); instruction-tuned
retrievers (model-call complexity, no need); multimodal (wrong problem); MTEB-carousel
chasing. Rule: no swaps below the evidence bar, and the current bar can't rank 768-d
generals — so only a *different axis* (code-trained: jina-code) or a *firmed-up
demand signal* justifies a new probe.

---

*The full dated arc and empirical core-bet detail are preserved in git history on
`mem-v2-foundation`.*
