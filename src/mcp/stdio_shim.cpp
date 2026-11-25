#include "mcp/stdio_shim.h"
#include "mcp/mcp_server.h"   // daemon_build_id
#include "mcp/framing.h"      // frame_write / frame_read (shared wire contract)

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

namespace gogol {
namespace mcp {
namespace {

// Open a fresh AF_UNIX connection to `path`. Returns fd>=0 on success, -1 if the socket
// isn't accepting (no daemon, or mid-shutdown). Caller closes the fd.
int dial(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) { ::close(fd); return -1; }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Spawn `gogol serve` detached (double-fork so it reparents to init and outlives the
// shim). Best-effort: we don't track the child; the daemon owns its own lifecycle
// (flock singleton + idle-timeout). Returns true if the spawn was launched.
bool spawn_daemon() {
    // Resolve our own executable so the spawned daemon is the SAME binary/version (the
    // version handshake defends the mismatch case, but spawning the same path avoids it).
    // Fall back to PATH "gogol" if self-path is unavailable.
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // child: detach from the shim's session, double-fork, exec the daemon.
        setsid();
        pid_t pid2 = ::fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);  // first child exits; grandchild reparents to init
        // grandchild: fully detach from the shim's I/O. CRITICAL — if we inherit the
        // shim's stdin/stdout/stderr (which are the agent's pipes), the long-lived daemon
        // holds those pipe ends open forever, so the agent's reader never sees EOF and
        // hangs. Redirect all three to /dev/null before exec.
        {
            int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                ::dup2(devnull, 0);
                ::dup2(devnull, 1);
                ::dup2(devnull, 2);
                if (devnull > 2) ::close(devnull);
            }
        }
        // exec `gogol serve --port 0`. port 0 = NO HTTP — the spawned daemon serves ONLY
        // the unix socket (it reads socket=on from config). Without --port 0 it would
        // inherit the default HTTP port and could collide with another server.
        execlp("gogol", "gogol", "serve", "--port", "0", static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }
    // parent: reap the first child (which exits immediately).
    int st = 0;
    ::waitpid(pid, &st, 0);
    return true;
}

// Connect to the daemon, spawning it + grace-retrying if the socket isn't accepting.
// The ONE place the connect-or-spawn dance lives (was inlined 3x). Returns fd>=0 or -1.
int dial_or_spawn(const std::string& path) {
    int fd = dial(path);
    if (fd >= 0) return fd;
    spawn_daemon();
    // grace window: poll-connect for up to ~5s (daemon start + bind).
    for (int i = 0; i < 50 && fd < 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fd = dial(path);
    }
    return fd;
}

// Version handshake over a FRESH connection: send our build id; a stale daemon (built
// from older source) replies match:false and self-exits. On mismatch, wait for the
// socket to die, then the caller re-dials (dial_or_spawn) a fresh daemon. Returns true
// if the daemon matched (or the probe was inconclusive — fail open, the pump will surface
// any real error); false only means "mismatch detected, daemon is exiting".
bool verify_version(const std::string& path) {
    int fd = dial(path);
    if (fd < 0) return true;  // nothing to verify against; dial_or_spawn handles absence
    std::string probe = "{\"jsonrpc\":\"2.0\",\"id\":\"__v\",\"method\":\"__daemon_version\","
                        "\"params\":{\"client_build\":\"" + daemon_build_id() + "\"}}";
    bool mismatch = false;
    if (frame_write(fd, probe)) {
        if (auto reply = frame_read(fd)) {
            mismatch = reply->find("\"match\":false") != std::string::npos;
        }
    }
    ::close(fd);  // daemon closes after the one response
    if (!mismatch) return true;
    // Stale daemon is exiting; wait for the socket to stop accepting before the caller respawns.
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int t = dial(path);
        if (t < 0) break;        // socket dead -> daemon gone
        ::close(t);
    }
    return false;
}

}  // namespace

int run_stdio_shim(const std::string& socket_path) {
    if (socket_path.empty()) return 1;

    // Connect (spawning the daemon if needed), then version-check: a stale daemon self-
    // exits on mismatch, so we re-dial a fresh one. At most one respawn cycle.
    int sock = dial_or_spawn(socket_path);
    if (sock < 0) {
        std::cerr << "gogol: could not reach or start the daemon at " << socket_path << "\n";
        return 1;  // caller may fall back to in-process stdio
    }
    ::close(sock); sock = -1;  // dial_or_spawn just proves reachability; verify on a fresh conn
    if (!verify_version(socket_path)) {
        // mismatch: stale daemon exited; bring up a fresh one.
        if (dial_or_spawn(socket_path) < 0) {
            std::cerr << "gogol: daemon respawn after version mismatch failed\n";
            return 1;
        }
    }

    // Pump: each stdin line = one JSON-RPC request. Open a connection, frame the request,
    // read the framed response, write it to stdout, close. One connection per request
    // (the daemon does one req/resp per accept); dial_or_spawn per line makes a daemon
    // idle-exit/restart mid-session transparent.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        int fd = dial_or_spawn(socket_path);
        if (fd < 0) { std::cerr << "gogol: daemon unreachable\n"; return 1; }
        if (!frame_write(fd, line)) { ::close(fd); continue; }
        auto resp = frame_read(fd);
        ::close(fd);
        if (resp) { std::cout << *resp << "\n"; std::cout.flush(); }
    }
    return 0;
}

std::optional<std::string> probe_daemon(const std::string& socket_path,
                                       const std::string& request) {
    if (socket_path.empty()) return std::nullopt;
    int fd = dial(socket_path);           // NON-spawning: -1 if no daemon is listening
    if (fd < 0) return std::nullopt;
    std::optional<std::string> resp;
    if (frame_write(fd, request)) resp = frame_read(fd);
    ::close(fd);
    return resp;
}

}  // namespace mcp
}  // namespace gogol
