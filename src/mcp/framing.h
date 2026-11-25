#pragma once

#include <optional>
#include <string>

namespace gogol {
namespace mcp {

// The gogol daemon wire framing: length-prefixed JSON frames over a stream socket.
// [4-byte big-endian uint32 length][JSON payload]. ONE home for the wire contract so
// the daemon (unix_server) and the stdio shim can't drift. (combadge's TS client mirrors
// this same format in its own language — keep the three in lockstep by format, not code.)
//
// Both are EINTR-safe, bound by kMaxFrameSize (oversize is rejected WITHOUT allocating).

constexpr std::size_t kMaxFrameSize = 64u * 1024u * 1024u;  // 64 MiB frame cap

// Write len(payload)+payload to fd. Loops on partial writes. false on error/oversize.
bool frame_write(int fd, const std::string& payload);

// Read one frame: 4-byte length then that many bytes. Loops on partial reads.
// nullopt on EOF / error / oversize length (does not allocate for a bogus length).
std::optional<std::string> frame_read(int fd);

}  // namespace mcp
}  // namespace gogol
