// embed_dispatcher.h — Single-threaded embed queue
// All embedding goes through this dispatcher. The llama.cpp context is not
// thread-safe, so one worker thread processes jobs sequentially.
#pragma once
#include "embedding/embed_provider.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class Embedder;

class EmbedDispatcher : public EmbedProvider {
public:
    explicit EmbedDispatcher(Embedder& embedder);
    ~EmbedDispatcher();

    // EmbedProvider interface (all blocking, routed through worker thread)
    uint32_t dim() override;
    std::vector<float> embed_query(const std::string& text) override;
    std::vector<float> embed_document(const std::string& text) override;
    std::vector<std::vector<float>> embed_documents_batch(const std::vector<std::string>& texts) override;

    // Async: embed text, call cb with result on the worker thread.
    // Use for add (client already got response).
    void embed_async(const std::string& text,
                     std::function<void(std::vector<float>)> cb,
                     bool is_query = false);

    // Queue depth (for status/debugging)
    size_t queue_size() const;

private:
    struct Job {
        std::string text;
        bool is_query = false;
        bool is_batch = false;
        std::vector<std::string> batch_texts;
        std::function<void(std::vector<float>)> callback;
        std::function<void(std::vector<std::vector<float>>)> batch_callback;
    };

    std::vector<float> embed_sync(const std::string& text, bool is_query);

    void run();

    Embedder& embedder_;
    std::queue<Job> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
};
