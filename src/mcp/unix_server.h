#pragma once

#include <string>

namespace gogol {
namespace mcp {

// Listen on an AF_UNIX stream socket at `path` and accept one request per
// connection. Each request/response is a 4-byte big-endian length prefix + JSON.
// Blocks forever; returns nonzero only on a fatal bind/listen/setup failure.
int run_unix_socket_server(const std::string& path);

}  // namespace mcp
}  // namespace gogol
