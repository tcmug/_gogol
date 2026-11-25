// embed_provider.h — Abstract embedding interface.
// gogol is daemon-less: the embedder always loads the model in-process
// (LocalEmbedder). get_embedder() builds a fresh one; get_shared_embedder()
// returns a process-lifetime warm singleton (for the MCP server's many calls).
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class EmbedProvider {
public:
    virtual ~EmbedProvider() = default;
    virtual uint32_t dim() = 0;
    virtual std::vector<float> embed_query(const std::string &text) = 0;
    virtual std::vector<float> embed_document(const std::string &text) = 0;
    virtual std::vector<std::vector<float>>
    embed_documents_batch(const std::vector<std::string> &texts) = 0;
};

// Build a fresh in-process embedder (loads the GGUF; ~1-2s). One-shot CLI use.
std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path);
// With explicit per-model retrieval prefixes (from GlobalConfig). std::nullopt for a
// prefix = AUTO-SELECT from the model architecture; a value overrides.
std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path,
                                            const std::optional<std::string> &query_prefix,
                                            const std::optional<std::string> &document_prefix);

// Process-lifetime warm singleton keyed by (model_path, prefixes): loads once,
// stays resident across calls. Use in the long-lived MCP server so tool calls
// don't reload the model each time. The reference is cache-owned. std::nullopt
// prefixes auto-select from the model architecture.
EmbedProvider &get_shared_embedder(const std::string &model_path,
                                   const std::optional<std::string> &query_prefix = std::nullopt,
                                   const std::optional<std::string> &document_prefix = std::nullopt);
