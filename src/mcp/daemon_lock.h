#pragma once

#include <string>

namespace gogol {
namespace mcp {

class DaemonLock {
public:
    explicit DaemonLock(const std::string& lock_path);
    ~DaemonLock();

    bool has_lock() const { return fd_ >= 0 && locked_; }

    DaemonLock(const DaemonLock&) = delete;
    DaemonLock& operator=(const DaemonLock&) = delete;

private:
    int fd_ = -1;
    bool locked_ = false;
};

}  // namespace mcp
}  // namespace gogol
