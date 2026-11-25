#include "mcp/daemon_lock.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace gogol {
namespace mcp {

DaemonLock::DaemonLock(const std::string& lock_path) {
    if (lock_path.empty()) {
        return;
    }

    fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        return;
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        locked_ = true;
        return;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        ::close(fd_);
        fd_ = -1;
        locked_ = false;
        return;
    }

    ::close(fd_);
    fd_ = -1;
    locked_ = false;
}

DaemonLock::~DaemonLock() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mcp
}  // namespace gogol
