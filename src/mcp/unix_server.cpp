#include "mcp/unix_server.h"

#include "mcp/mcp_server.h"
#include "mcp/framing.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace gogol {
namespace mcp {
namespace {

void handle_conn(int fd) {
    std::optional<std::string> raw = frame_read(fd);
    if (!raw.has_value()) {
        ::close(fd);
        return;
    }
    // Route through the ONE shared handler (parse + dispatch + error-wrap) so this
    // transport stays thin — no duplicated JSON-RPC logic. nullopt = notification (no
    // response); otherwise frame the response back.
    if (auto resp = handle_request_json(*raw)) {
        (void)frame_write(fd, *resp);
    }
    ::close(fd);
}

}  // namespace

int run_unix_socket_server(const std::string& path) {
    if (path.empty()) {
        return 1;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);

    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(srv);
        return 1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(srv);
        return 1;
    }

    if (::listen(srv, 16) < 0) {
        perror("listen");
        ::close(srv);
        return 1;
    }

    for (;;) {
        int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(srv);
            return 1;
        }
        std::thread(handle_conn, fd).detach();
    }

    ::close(srv);
    return 0;
}

}  // namespace mcp
}  // namespace gogol
