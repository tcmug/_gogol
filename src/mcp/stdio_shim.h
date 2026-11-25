#pragma once

#include <optional>
#include <string>

namespace gogol {
namespace mcp {

// The stdio SHIM: what `gogol mcp` becomes in daemon mode. The agent speaks line-framed
// JSON-RPC on stdin/stdout (standard MCP stdio); the shim forwards each request to the
// warm daemon over the unix socket (length-prefixed frames) and writes each response
// back to stdout. It holds no model, no store — it's a thin pipe to the daemon.
//
// connect_or_spawn: if the socket isn't accepting, spawn the daemon (`gogol serve`,
// detached) and retry-connect within a grace window, so the first `gogol mcp` of a
// session transparently brings the daemon up.
//
// Returns a process exit code. A fatal inability to reach/spawn the daemon returns
// nonzero (the caller may then fall back to in-process stdio).
int run_stdio_shim(const std::string& socket_path);

// NON-spawning probe: dial the daemon socket WITHOUT ever spawning one, send a single
// framed JSON-RPC request, and return the raw response payload. std::nullopt means no
// daemon is listening (socket absent / not accepting). Used by observability paths
// (`gogol status`, a future TUI/webstatus) that want live daemon state IF a daemon is
// already up, but must NOT boot a 375MB daemon just to glance. Caller parses the reply.
std::optional<std::string> probe_daemon(const std::string& socket_path,
                                        const std::string& request);

}  // namespace mcp
}  // namespace gogol
