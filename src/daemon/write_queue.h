// write_queue.h — Single-threaded writer for all index mutations.
//
// Every mutation of daemon index state (index, add, rm, watcher reindex) is
// submitted here as a job and executed on one owner thread. This makes the
// mutable_copy -> mutate -> publish sequence race-free by construction: since
// only the writer thread ever mutates, the snapshot a job copies is always the
// latest published one, so there is no lost-update window between concurrent
// writers. Readers remain lock-free via immutable snapshots (see DaemonState).
//
// Embedding still goes through EmbedDispatcher (sole owner of the llama
// context); the writer thread blocks on those embed jobs. The two threads do
// not deadlock — the dispatcher serializes writer embeds with query embeds.
#pragma once
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

class WriteQueue {
public:
  WriteQueue() : worker_([this] { run(); }) {}

  ~WriteQueue() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  WriteQueue(const WriteQueue &) = delete;
  WriteQueue &operator=(const WriteQueue &) = delete;

  // Fire-and-forget: enqueue a mutation, return immediately.
  void submit(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      queue_.push(std::move(job));
    }
    cv_.notify_one();
  }

  // Enqueue and block until the job has run, returning its result.
  // Use for operations whose result the client needs synchronously (e.g. rm).
  template <typename T> T submit_wait(std::function<T()> job) {
    std::promise<T> prom;
    auto fut = prom.get_future();
    submit([job = std::move(job), &prom]() mutable {
      try {
        prom.set_value(job());
      } catch (...) {
        prom.set_exception(std::current_exception());
      }
    });
    return fut.get();
  }

  size_t depth() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return queue_.size();
  }

private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || stop_; });
        if (stop_ && queue_.empty()) return;
        job = std::move(queue_.front());
        queue_.pop();
      }
      // Jobs are expected to contain their own try/catch (handlers convert
      // failures to responses/log lines). This guard is a last-resort backstop
      // so one bad job can't take down the writer thread / process.
      try {
        job();
      } catch (const std::exception &ex) {
        fprintf(stderr, "[write-queue] job failed: %s\n", ex.what());
      } catch (...) {
        fprintf(stderr, "[write-queue] job failed: unknown\n");
      }
    }
  }

  std::queue<std::function<void()>> queue_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
};
