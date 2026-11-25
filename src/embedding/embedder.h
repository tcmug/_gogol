#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Embedder {
public:
    // Query/document prefixes are the model's RETRIEVAL HANDSHAKE (how it distinguishes
    // "the seeker" from "the sought"); they are per-model, NOT universal. Pass
    // std::nullopt to AUTO-SELECT from the model's architecture (see
    // default_prefixes_for_arch); pass a value to OVERRIDE. Empty string = no prefix.
    explicit Embedder(const std::string &model_path,
                      const std::optional<std::string> &query_prefix = std::nullopt,
                      const std::optional<std::string> &document_prefix = std::nullopt);
    ~Embedder();

    std::vector<float> embed_document(const std::string &text);
    std::vector<float> embed_query(const std::string &text);
    std::vector<std::vector<float>>
    embed_documents_batch(const std::vector<std::string> &texts);
    int dim() const;

private:
    std::vector<float> embed(const std::string &text);
    // A llama_context is NOT thread-safe: concurrent llama_encode on one context
    // corrupts its batch allocator (heap abort / SIGSEGV). The daemon runs one thread
    // per connection over a shared warm embedder, so every public entry point that
    // touches ctx_ takes this lock. embed() is private and called only under it.
    std::mutex mu_;
    struct llama_model *model_ = nullptr;
    struct llama_context *ctx_ = nullptr;
    int n_embd_ = 0;
    int n_ctx_ = 0;
    std::string query_prefix_;
    std::string document_prefix_;
};
