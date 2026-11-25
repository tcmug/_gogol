#include "embedding/embedder.h"
#include "config/debug.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include "embedding/model_prefixes.h"
#include "ggml.h"
#include "llama.h"

Embedder::Embedder(const std::string &model_path,
                   const std::optional<std::string> &query_prefix,
                   const std::optional<std::string> &document_prefix) {
  static bool backend_initialized = false;
  if (!backend_initialized) {
    // Silence llama.cpp's log entirely by default. llama prints verbose load
    // info (tensor assignments, graph dumps) AND a "." per percent of model
    // load via GGML_LOG_LEVEL_CONT (llama.cpp:419) — all noise for normal CLI
    // use. To see the log for debugging (e.g. to confirm GPU offload), set
    // GOGOL_LLAMA_LOG=1 in the environment.
    if (!std::getenv("GOGOL_LLAMA_LOG")) {
      llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    }
    llama_backend_init();
    backend_initialized = true;
  }

  auto mparams = llama_model_default_params();
  model_ = llama_model_load_from_file(model_path.c_str(), mparams);
  if (!model_) {
    std::cerr << "Error: failed to load model: " << model_path << "\n";
    std::exit(1);
  }

  auto cparams = llama_context_default_params();
  // INVARIANT §5 (docs/INVARIANTS.md): the embedder must be model-agnostic and llama
  // violates these constraints by ASSERTING/segfaulting, not returning an error.
  // Context size must come from the MODEL, not a hardcoded constant: a 512-token
  // model (MiniLM/bge-small) aborts if asked for 2048, and a larger model would be
  // mis-sized. Use the model's trained context length, clamped to a sane range.
  int32_t n_train = llama_model_n_ctx_train(model_);
  int32_t n_ctx = n_train > 0 ? n_train : 2048;
  if (n_ctx > 8192) n_ctx = 8192;   // cap memory for very-long-context models
  if (n_ctx < 128)  n_ctx = 128;    // floor
  cparams.n_ctx = n_ctx;
  // batch/ubatch must not exceed n_ctx (llama requires n_batch <= n_ctx for embeddings).
  cparams.n_batch = n_ctx;
  cparams.n_ubatch = n_ctx;
  cparams.n_seq_max = 8;
  cparams.embeddings = true;

  ctx_ = llama_init_from_model(model_, cparams);
  if (!ctx_) {
    std::cerr << "Error: failed to create context\n";
    std::exit(1);
  }

  n_embd_ = llama_model_n_embd(model_);
  n_ctx_ = cparams.n_ctx;

  // Resolve retrieval prefixes: an explicit override (config) wins; otherwise
  // auto-select from the model's architecture (GGUF general.architecture). The
  // prefix convention is a per-model training detail NOT stored in the GGUF, so a
  // small built-in table maps known architectures to their prefixes; an unknown
  // architecture falls back to empty (safe — degrades to raw text, no wrong prefix).
  {
    std::string arch;
    char buf[128];
    int n = llama_model_meta_val_str(model_, "general.architecture", buf, sizeof(buf));
    if (n > 0) arch.assign(buf, n);
    auto defaults = default_prefixes_for_arch(arch);
    query_prefix_    = query_prefix.value_or(defaults.first);
    document_prefix_ = document_prefix.value_or(defaults.second);
    if (!query_prefix && !document_prefix && defaults.first.empty() &&
        defaults.second.empty() && !arch.empty())
      std::cerr << "gogol: unknown embedding architecture '" << arch
                << "' — using empty retrieval prefixes (set query_prefix/"
                << "document_prefix in ~/.gogol/config if this model needs them)\n";
  }
}

Embedder::~Embedder() {
  if (ctx_)
    llama_free(ctx_);
  if (model_)
    llama_model_free(model_);
  // Note: llama_backend_free() is NOT called here.
  // It should be called once at program exit, not per Embedder instance.
}

std::vector<float> Embedder::embed(const std::string &text) {
  const llama_vocab *vocab = llama_model_get_vocab(model_);

  // Tokenize
  int n_tokens = text.size() + 16;
  std::vector<llama_token> tokens(n_tokens);
  n_tokens = llama_tokenize(vocab, text.c_str(), text.size(), tokens.data(),
                            tokens.size(), true, true);
  if (n_tokens < 0) {
    tokens.resize(-n_tokens);
    n_tokens = llama_tokenize(vocab, text.c_str(), text.size(), tokens.data(),
                              tokens.size(), true, true);
  }
  if (n_tokens <= 0)
    return {};

  // Truncate to context size
  // INVARIANT §5 (docs/INVARIANTS.md): non-causal models need the whole sequence in
  // ONE micro-batch. Cap at min(n_ctx, n_ubatch) — n_ctx alone trips the assert because
  // llama rounds it up above n_ubatch (bug 85b7b80).
  // Truncate so the sequence fits the encode. Embedding models are non-causal:
  // llama asserts n_ubatch >= n_tokens (the whole sequence must fit ONE micro-batch,
  // no chunked prefill). So cap at min(n_ctx, n_ubatch) — n_ctx alone is not enough
  // because llama may round n_ctx up above n_ubatch, tripping the encoder assert.
  int n_ctx = static_cast<int>(llama_n_ctx(ctx_));
  int n_ubatch = static_cast<int>(llama_n_ubatch(ctx_));
  int cap = n_ctx < n_ubatch ? n_ctx : n_ubatch;
  if (n_tokens > cap)
    n_tokens = cap;

  // Use llama_encode (no KV cache needed for embeddings)
  llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);

  // Clear memory before encoding to prevent buildup
  llama_memory_clear(llama_get_memory(ctx_), true);

  if (llama_encode(ctx_, batch) != 0) {
    return {};
  }

  // Get embedding
  const float *embd = llama_get_embeddings_seq(ctx_, 0);
  if (!embd) {
    embd = llama_get_embeddings(ctx_);
  }
  if (!embd)
    return {};

  // Normalize
  std::vector<float> result(embd, embd + n_embd_);
  float norm = 0;
  for (float v : result)
    norm += v * v;
  norm = std::sqrt(norm);
  if (norm > 0) {
    for (float &v : result)
      v /= norm;
  }

  return result;
}

std::vector<float> Embedder::embed_document(const std::string &text) {
  std::scoped_lock lk(mu_);
  return embed(document_prefix_ + text);
}

std::vector<float> Embedder::embed_query(const std::string &text) {
  std::scoped_lock lk(mu_);
  return embed(query_prefix_ + text);
}

std::vector<std::vector<float>>
Embedder::embed_documents_batch(const std::vector<std::string> &texts) { DBG("embed_batch: %zu texts", texts.size());
  if (texts.empty())
    return {};
  std::scoped_lock lk(mu_);

  const llama_vocab *vocab = llama_model_get_vocab(model_);

  // Tokenize all texts
  struct SeqInfo {
    std::vector<llama_token> tokens;
  };
  std::vector<SeqInfo> seqs;
  seqs.reserve(texts.size());

  for (auto &text : texts) {
    std::string prefixed = document_prefix_ + text;
    int n = prefixed.size() + 16;
    std::vector<llama_token> tokens(n);
    n = llama_tokenize(vocab, prefixed.c_str(), prefixed.size(), tokens.data(),
                       tokens.size(), true, true);
    if (n < 0) {
      tokens.resize(-n);
      n = llama_tokenize(vocab, prefixed.c_str(), prefixed.size(),
                         tokens.data(), tokens.size(), true, true);
    }
    if (n <= 0) {
      seqs.push_back({{}});
      continue;
    }
    // Truncate per-sequence to model max (2048 for nomic)
    if (n > 2048)
      n = 2048;
    tokens.resize(n);
    seqs.push_back({std::move(tokens)});
  }

  // Process in batches that fit in context
  std::vector<std::vector<float>> results(texts.size());
  size_t i = 0;

  while (i < seqs.size()) {
    // Pack sequences until we fill the context (max 8 seqs to match n_seq_max)
    int total_tokens = 0;
    size_t batch_end = i;
    while (batch_end < seqs.size() && batch_end - i < 8) {
      int seq_len = seqs[batch_end].tokens.size();
      if (seq_len == 0) {
        batch_end++;
        continue;
      }
      if (total_tokens + seq_len > n_ctx_)
        break;
      total_tokens += seq_len;
      batch_end++;
    }
    if (batch_end == i) {
      // Single sequence exceeds n_ctx — skip it with a warning
      if (i < seqs.size() && !seqs[i].tokens.empty()) {
        fprintf(stderr, "[ERR] chunk too large (%zu tokens > %d ctx), skipping\n",
                seqs[i].tokens.size(), n_ctx_);
      }
      i++;
      continue;
    } // skip oversized

    // Build batch with seq_ids
    int n_seq = batch_end - i;
    llama_batch batch = llama_batch_init(total_tokens, 0, n_seq);
    int pos = 0;
    for (size_t s = i; s < batch_end; s++) {
      auto &toks = seqs[s].tokens;
      if (toks.empty())
        continue;
      for (size_t t = 0; t < toks.size(); t++) {
        batch.token[pos] = toks[t];
        batch.pos[pos] = t;
        batch.n_seq_id[pos] = 1;
        batch.seq_id[pos][0] = s - i;
        batch.logits[pos] = 1; // need embeddings for all tokens (pooling)
        pos++;
      }
    }
    batch.n_tokens = pos;

    // Clear memory before encoding to prevent buildup
    llama_memory_clear(llama_get_memory(ctx_), true);

    if (llama_encode(ctx_, batch) == 0) {
      // Extract per-sequence embeddings
      for (size_t s = i; s < batch_end; s++) {
        if (seqs[s].tokens.empty())
          continue;
        const float *embd = llama_get_embeddings_seq(ctx_, s - i);
        if (embd) {
          std::vector<float> vec(embd, embd + n_embd_);
          float norm = 0;
          for (float v : vec)
            norm += v * v;
          norm = std::sqrt(norm);
          if (norm > 0)
            for (float &v : vec)
              v /= norm;
          results[s] = std::move(vec);
        }
      }
    } else {
      fprintf(stderr, "[ERR] llama_encode failed (batch %zu-%zu, %d tokens)\n",
              i, batch_end, total_tokens);
    }

    llama_batch_free(batch);
    i = batch_end;
  }

  return results;
}

int Embedder::dim() const { return n_embd_; }
