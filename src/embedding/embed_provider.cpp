// embed_provider.cpp — the embedder factory.
//
// Relocated out of daemon/embed_client.cpp (which no longer exists): gogol is
// daemon-less, so there is no "try daemon first" — the embedder is ALWAYS a
// LocalEmbedder that loads the GGUF in-process. Kept behind the EmbedProvider
// interface so callers (cmd_store, mcp_server) depend on the abstraction, not on
// the concrete llama.cpp Embedder.
#include "embedding/embed_provider.h"
#include "embedding/embedder.h"

#include <mutex>
#include <optional>

// --- LocalEmbedder: in-process llama.cpp model ---
class LocalEmbedder : public EmbedProvider {
public:
  explicit LocalEmbedder(const std::string &model_path,
                         const std::optional<std::string> &query_prefix = std::nullopt,
                         const std::optional<std::string> &document_prefix = std::nullopt)
      : embedder_(model_path, query_prefix, document_prefix) {}
  uint32_t dim() override { return embedder_.dim(); }
  std::vector<float> embed_query(const std::string &text) override {
    return embedder_.embed_query(text);
  }
  std::vector<float> embed_document(const std::string &text) override {
    return embedder_.embed_document(text);
  }
  std::vector<std::vector<float>>
  embed_documents_batch(const std::vector<std::string> &texts) override {
    return embedder_.embed_documents_batch(texts);
  }

private:
  Embedder embedder_;
};

std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path) {
  return std::make_unique<LocalEmbedder>(model_path);
}

std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path,
                                            const std::optional<std::string> &query_prefix,
                                            const std::optional<std::string> &document_prefix) {
  return std::make_unique<LocalEmbedder>(model_path, query_prefix, document_prefix);
}

// --- Warm process-lifetime singleton ---
// A LocalEmbedder loads the GGUF (~1-2s) on construction. A one-shot CLI pays that
// once and exits; but the MCP server handles many tool calls in ONE process, so it
// must NOT reload per call. get_shared_embedder returns a cached provider keyed by
// (model_path, prefixes) — the model loads on first use and stays warm for the
// process lifetime (this recovers the daemon's "always-warm" benefit without a daemon).
// The returned reference is owned by the cache; callers must not delete it.
EmbedProvider &get_shared_embedder(const std::string &model_path,
                                   const std::optional<std::string> &query_prefix,
                                   const std::optional<std::string> &document_prefix) {
  static std::mutex mu;
  static std::string cached_key;
  static std::unique_ptr<EmbedProvider> cached;
  std::lock_guard<std::mutex> lk(mu);
  // Key includes the prefix overrides (nullopt rendered distinctly from "") so a
  // config change re-builds the singleton.
  auto pk = [](const std::optional<std::string> &o){ return o ? "=" + *o : std::string("~auto"); };
  std::string key = model_path + "\x1f" + pk(query_prefix) + "\x1f" + pk(document_prefix);
  if (!cached || cached_key != key) {
    cached = get_embedder(model_path, query_prefix, document_prefix);
    cached_key = key;
  }
  return *cached;
}
