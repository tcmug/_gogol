#include "mcp/framing.h"

#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <sys/socket.h>

namespace gogol {
namespace mcp {
namespace {

// Write all `len` bytes to fd, looping on partial writes; EINTR-safe. false on error.
bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

bool frame_write(int fd, const std::string& payload) {
    if (payload.size() > kMaxFrameSize) return false;
    std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    char h[4] = {static_cast<char>((len >> 24) & 0xffu), static_cast<char>((len >> 16) & 0xffu),
                 static_cast<char>((len >> 8) & 0xffu), static_cast<char>(len & 0xffu)};
    return write_all(fd, h, 4) && write_all(fd, payload.data(), payload.size());
}

std::optional<std::string> frame_read(int fd) {
    char h[4];
    std::size_t off = 0;
    while (off < 4u) {
        ssize_t n = ::recv(fd, h + off, 4u - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return std::nullopt; }
        if (n == 0) return std::nullopt;
        off += static_cast<std::size_t>(n);
    }
    std::uint32_t len = (static_cast<std::uint32_t>(static_cast<unsigned char>(h[0])) << 24) |
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(h[1])) << 16) |
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(h[2])) << 8) |
                        static_cast<std::uint32_t>(static_cast<unsigned char>(h[3]));
    if (len > kMaxFrameSize) return std::nullopt;
    std::string payload;
    payload.resize(len);
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, const_cast<char*>(payload.data()) + got, len - got, 0);
        if (n < 0) { if (errno == EINTR) continue; return std::nullopt; }
        if (n == 0) return std::nullopt;
        got += static_cast<std::size_t>(n);
    }
    return payload;
}

}  // namespace mcp
}  // namespace gogol
