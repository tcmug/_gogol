// embed_dispatcher.cpp — Single-threaded embed queue implementation
#include "embedding/embed_dispatcher.h"
#include "embedding/embedder.h"
#include <future>

EmbedDispatcher::EmbedDispatcher(Embedder& embedder)
    : embedder_(embedder), worker_([this]{ run(); }) {}

EmbedDispatcher::~EmbedDispatcher() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

uint32_t EmbedDispatcher::dim() {
    return embedder_.dim();
}

size_t EmbedDispatcher::queue_size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

std::vector<float> EmbedDispatcher::embed_sync(const std::string& text, bool is_query) {
    std::promise<std::vector<float>> promise;
    auto future = promise.get_future();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        Job job;
        job.text = text;
        job.is_query = is_query;
        job.callback = [&promise](std::vector<float> vec) {
            promise.set_value(std::move(vec));
        };
        queue_.push(std::move(job));
    }
    cv_.notify_one();
    return future.get();
}

std::vector<float> EmbedDispatcher::embed_query(const std::string& text) {
    return embed_sync(text, true);
}

std::vector<float> EmbedDispatcher::embed_document(const std::string& text) {
    return embed_sync(text, false);
}

std::vector<std::vector<float>> EmbedDispatcher::embed_documents_batch(const std::vector<std::string>& texts) {
    std::promise<std::vector<std::vector<float>>> promise;
    auto future = promise.get_future();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        Job job;
        job.is_batch = true;
        job.batch_texts = texts;
        job.batch_callback = [&promise](std::vector<std::vector<float>> vecs) {
            promise.set_value(std::move(vecs));
        };
        queue_.push(std::move(job));
    }
    cv_.notify_one();
    return future.get();
}

void EmbedDispatcher::embed_async(const std::string& text,
                                  std::function<void(std::vector<float>)> cb,
                                  bool is_query) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Job job;
        job.text = text;
        job.is_query = is_query;
        job.callback = std::move(cb);
        queue_.push(std::move(job));
    }
    cv_.notify_one();
}

void EmbedDispatcher::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]{ return !queue_.empty() || stop_; });
            if (stop_ && queue_.empty()) break;
            job = std::move(queue_.front());
            queue_.pop();
        }

        if (job.is_batch) {
            auto results = embedder_.embed_documents_batch(job.batch_texts);
            if (job.batch_callback) job.batch_callback(std::move(results));
        } else {
            std::vector<float> vec;
            if (job.is_query)
                vec = embedder_.embed_query(job.text);
            else
                vec = embedder_.embed_document(job.text);
            if (job.callback) job.callback(std::move(vec));
        }
    }
}
