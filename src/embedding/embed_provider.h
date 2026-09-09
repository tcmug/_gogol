// embed_provider.h — Abstract embedding interface
// LocalEmbedder loads model directly. RemoteEmbedder connects to daemon via socket.
// get_embedder() tries daemon first, falls back to local.
#pragma once
#include <cstdint>
#include <memory>
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

// Resolve embedder: try daemon socket first, fall back to local model load
std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path);

// Check if daemon is running (can connect to socket)
bool daemon_is_running();

// Socket path
std::string daemon_socket_path();
