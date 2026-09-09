#include "embedding/embedder.h"
#include "config/debug.h"

#include <cmath>
#include <iostream>

#include "ggml.h"
#include "llama.h"

Embedder::Embedder(const std::string &model_path) {
  static bool backend_initialized = false;
  if (!backend_initialized) {
    llama_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);
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
  cparams.n_ctx = 2048;
  cparams.n_batch = 4096;
  cparams.n_ubatch = 4096;
  cparams.n_seq_max = 8;
  cparams.embeddings = true;

  ctx_ = llama_init_from_model(model_, cparams);
  if (!ctx_) {
    std::cerr << "Error: failed to create context\n";
    std::exit(1);
  }

  n_embd_ = llama_model_n_embd(model_);
  n_ctx_ = cparams.n_ctx;
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
  int n_ctx = llama_n_ctx(ctx_);
  if (n_tokens > n_ctx)
    n_tokens = n_ctx;

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
  return embed("search_document: " + text);
}

std::vector<float> Embedder::embed_query(const std::string &text) {
  return embed("search_query: " + text);
}

std::vector<std::vector<float>>
Embedder::embed_documents_batch(const std::vector<std::string> &texts) { DBG("embed_batch: %zu texts", texts.size());
  if (texts.empty())
    return {};

  const llama_vocab *vocab = llama_model_get_vocab(model_);

  // Tokenize all texts
  struct SeqInfo {
    std::vector<llama_token> tokens;
  };
  std::vector<SeqInfo> seqs;
  seqs.reserve(texts.size());

  for (auto &text : texts) {
    std::string prefixed = "search_document: " + text;
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
