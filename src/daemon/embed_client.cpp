// embed_client.cpp — RPC client with framed + encrypted TCP support
#include "daemon/embed_client.h"
#include "daemon/rpc_v2.h"
#include "embedding/embed_provider.h"
#include "embedding/embedder.h"
#include "daemon/rpc.h"
#include "core/version.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

std::string daemon_socket_path() {
  return (fs::path(std::getenv("HOME")) / ".gogol" / "sock").string();
}

bool daemon_is_running() {
  const char *host_env = std::getenv("GOGOL_HOST");
  if (host_env && host_env[0]) {
    // Remote: try a quick TCP connect
    std::string host_str(host_env);
    auto colon = host_str.rfind(':');
    std::string host = (colon != std::string::npos) ? host_str.substr(0, colon) : host_str;
    int port = (colon != std::string::npos) ? std::atoi(host_str.substr(colon + 1).c_str()) : 9400;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in taddr{};
    taddr.sin_family = AF_INET;
    taddr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &taddr.sin_addr);
    bool ok = (connect(fd, (struct sockaddr *)&taddr, sizeof(taddr)) == 0);
    close(fd);
    return ok;
  }
  // Local: try connecting to unix socket (authoritative check)
  std::string sock = daemon_socket_path();
  if (!std::filesystem::exists(sock)) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
  bool ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  close(fd);
  return ok;
}

// --- RpcClient ---

RpcClient::RpcClient() { connect_to_daemon(); }
RpcClient::~RpcClient() { disconnect(); }

static uint8_t hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return 0;
}

bool RpcClient::connect_to_daemon() {
  const char *host_env = std::getenv("GOGOL_HOST");
  if (host_env && host_env[0]) {
    // TCP connection
    std::string host_str(host_env);
    auto colon = host_str.rfind(':');
    std::string host = (colon != std::string::npos) ? host_str.substr(0, colon) : host_str;
    int port = (colon != std::string::npos) ? std::atoi(host_str.substr(colon + 1).c_str()) : 9400;

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    struct sockaddr_in taddr{};
    taddr.sin_family = AF_INET;
    taddr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &taddr.sin_addr);
    if (connect(fd_, (struct sockaddr *)&taddr, sizeof(taddr)) != 0) {
      close(fd_); fd_ = -1; return false;
    }

    // Handshake
    const char *key_name_env = std::getenv("GOGOL_KEY_NAME");
    const char *key_hex_env = std::getenv("GOGOL_KEY");
    std::string key_name = key_name_env ? key_name_env : "";
    std::vector<uint8_t> key_bytes;
    if (key_hex_env) {
      std::string hex(key_hex_env);
      for (size_t i = 0; i + 1 < hex.size(); i += 2)
        key_bytes.push_back((hex_val(hex[i]) << 4) | hex_val(hex[i + 1]));
    }

    const uint8_t *key_ptr = (key_bytes.size() == 32) ? key_bytes.data() : nullptr;
    auto hs = client_handshake(fd_, key_name, key_ptr);
    if (!hs.ok) { close(fd_); fd_ = -1; return false; }

    if (hs.encrypted) crypto_ = hs.crypto;
    return true;
  }

  // Unix socket (framed, no encryption)
  fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_ < 0) return false;
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string path = daemon_socket_path();
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd_); fd_ = -1; return false;
  }
  // Set read timeout to avoid hanging on dead/stuck daemon
  struct timeval tv{30, 0}; // 30 seconds (add/index operations may be slow)
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Version check: send PING, verify daemon build matches this binary
  if (!check_daemon_version()) {
    close(fd_); fd_ = -1; return false;
  }
  return true;
}

bool RpcClient::check_daemon_version() {
  // Send PING, expect version string back
  MsgWriter msg;
  msg.put_u8((uint8_t)RpcCmd::PING);
  if (!send_msg(msg)) return false;

  auto reply = recv_msg();
  if (reply.empty()) return false;

  MsgReader reader(reply);
  uint8_t status_byte;
  if (!reader.get_u8(status_byte)) return false;
  if (static_cast<RpcStatus>(status_byte) != RpcStatus::OK) return false;

  std::string daemon_version;
  if (!reader.get_str(daemon_version)) return false;

  std::string my_version = GOGOL_BUILD_VERSION;
  if (daemon_version != my_version) {
    std::cerr << "Daemon is running a different build (daemon: "
              << daemon_version << ", this binary: " << my_version << ")\n"
              << "Restart with: gogol serve --stop && gogol serve\n";
    return false;
  }
  return true;
}

void RpcClient::disconnect() {
  if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

bool RpcClient::send_msg(const MsgWriter& msg) {
  if (crypto_.active)
    return crypto_frame_write(fd_, crypto_, msg.data().data(), (uint32_t)msg.data().size());
  return frame_write(fd_, msg.data());
}

std::vector<uint8_t> RpcClient::recv_msg() {
  if (crypto_.active) return crypto_frame_read(fd_, crypto_);
  return frame_read(fd_);
}

// --- Command implementations ---

std::string RpcClient::cmd_query(const QueryRequest& req, std::vector<QueryResultEntry>& results) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg;
  msg.put_u8((uint8_t)RpcCmd::QUERY);
  msg.put_str(req.query); msg.put_str(req.index);
  msg.put_u32((uint32_t)(int32_t)req.type_filter); // -1 = all
  msg.put_u32((uint32_t)req.top_k); msg.put_u32((uint32_t)req.show_lines);
  msg.put_u8(req.scores ? 1 : 0);
  if (!send_msg(msg)) return "Send failed";
  auto resp_data = recv_msg();
  if (resp_data.empty()) return "Read failed";
  MsgReader resp(std::move(resp_data));
  uint8_t status; resp.get_u8(status);
  if (status != 0) { std::string err; resp.get_str(err); return err; }
  uint32_t count; resp.get_u32(count);
  results.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    resp.get_str(results[i].index);
    resp.get_str(results[i].path);
    resp.get_u32(results[i].line);
    resp.get_str(results[i].chunk);
    resp.get_u8(results[i].proto);
    resp.get_str(results[i].status);
    resp.get_float(results[i].score);
    resp.get_float(results[i].cosine);
    resp.get_str(results[i].snippet);
  }
  return "";
}

std::string RpcClient::cmd_add(const AddRequest& req) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::ADD);
  msg.put_u8((uint8_t)req.type);
  msg.put_str(req.index); msg.put_str(req.path);
  msg.put_str(req.content); msg.put_str(req.sources);
  if (!send_msg(msg)) return "Send failed";
  auto r = recv_msg(); if (r.empty()) return "Read failed";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  std::string m; resp.get_str(m); return (st==0) ? m : "Error: " + m;
}

std::string RpcClient::cmd_rm(const EntryRef& ref) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::RM);
  msg.put_u8((uint8_t)ref.type); msg.put_str(ref.index); msg.put_str(ref.path);
  if (!send_msg(msg)) return "Send failed";
  auto r = recv_msg(); if (r.empty()) return "Read failed";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  std::string m; resp.get_str(m); return (st==0) ? m : "Error: " + m;
}

std::string RpcClient::cmd_get(const EntryRef& ref, int max_lines) {
  if (fd_ < 0) return "";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::GET);
  msg.put_u8((uint8_t)ref.type); msg.put_str(ref.index); msg.put_str(ref.path);
  msg.put_u32((uint32_t)max_lines);
  if (!send_msg(msg)) return "";
  auto r = recv_msg(); if (r.empty()) return "";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  std::string m; resp.get_str(m);
  if (st != 0) { std::cerr << m << "\n"; return ""; }
  return m;
}

std::string RpcClient::cmd_list(EntryType type, bool has_type, const std::string& index,
                                std::vector<ListResultEntry>& entries) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::LIST);
  msg.put_u8(has_type ? 1 : 0);
  msg.put_u8((uint8_t)type);
  msg.put_str(index);
  if (!send_msg(msg)) return "Send failed";
  auto r = recv_msg(); if (r.empty()) return "Read failed";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  if (st != 0) { std::string err; resp.get_str(err); return err; }
  uint32_t count; resp.get_u32(count);
  entries.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    resp.get_u8(entries[i].proto);
    resp.get_str(entries[i].index);
    resp.get_str(entries[i].path);
    resp.get_u32(entries[i].line);
    resp.get_str(entries[i].chunk);
  }
  return "";
}

std::string RpcClient::cmd_index(const IndexRequest& req) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::INDEX);
  uint8_t flags = (req.force ? 0x01 : 0x00);
  msg.put_str(req.index); msg.put_u8(flags);
  if (!send_msg(msg)) return "Send failed";
  auto r = recv_msg(); if (r.empty()) return "Read failed";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  std::string m; resp.get_str(m); return m;
}

std::string RpcClient::cmd_status(const std::string& index, bool& indexing, std::string& indexing_name,
                                  int& progress, int& total, int& elapsed_sec) {
  if (fd_ < 0) return "Not connected";
  MsgWriter msg; msg.put_u8((uint8_t)RpcCmd::STATUS); msg.put_str(index);
  if (!send_msg(msg)) return "Send failed";
  auto r = recv_msg(); if (r.empty()) return "Read failed";
  MsgReader resp(std::move(r)); uint8_t st; resp.get_u8(st);
  if (st != 0) { std::string err; resp.get_str(err); return err; }
  uint8_t idx_flag; resp.get_u8(idx_flag);
  indexing = (idx_flag != 0);
  resp.get_str(indexing_name);
  uint32_t p, t, e;
  resp.get_u32(p); progress = p;
  resp.get_u32(t); total = t;
  resp.get_u32(e); elapsed_sec = e;
  return "";
}

// --- LocalEmbedder (fallback) ---

class LocalEmbedder : public EmbedProvider {
public:
  explicit LocalEmbedder(const std::string &model_path) : embedder_(model_path) {}
  uint32_t dim() override { return embedder_.dim(); }
  std::vector<float> embed_query(const std::string &text) override { return embedder_.embed_query(text); }
  std::vector<float> embed_document(const std::string &text) override { return embedder_.embed_document(text); }
  std::vector<std::vector<float>> embed_documents_batch(const std::vector<std::string> &texts) override { return embedder_.embed_documents_batch(texts); }
private:
  Embedder embedder_;
};

std::unique_ptr<EmbedProvider> get_embedder(const std::string &model_path) {
  return std::make_unique<LocalEmbedder>(model_path);
}
