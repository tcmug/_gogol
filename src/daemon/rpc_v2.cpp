// rpc_v2.cpp — Framed protocol implementation
#include "daemon/rpc_v2.h"

#include <cstring>
#include <fcntl.h>
#include <map>
#include <unistd.h>

// Bring in monocypher for encryption
extern "C" {
#include "monocypher.h"
}

// --- Raw I/O helpers ---

static bool raw_write(int fd, const void *buf, size_t len) {
  const char *p = static_cast<const char *>(buf);
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

static bool raw_read(int fd, void *buf, size_t len) {
  char *p = static_cast<char *>(buf);
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

// --- Framed I/O ---

bool frame_write(int fd, const uint8_t *data, uint32_t len) {
  if (!raw_write(fd, &len, 4)) return false;
  if (len > 0 && !raw_write(fd, data, len)) return false;
  return true;
}

bool frame_write(int fd, const std::vector<uint8_t> &buf) {
  return frame_write(fd, buf.data(), (uint32_t)buf.size());
}

std::vector<uint8_t> frame_read(int fd) {
  uint32_t len;
  if (!raw_read(fd, &len, 4)) return {};
  if (len == 0) return {};
  if (len > 64 * 1024 * 1024) return {}; // 64MB sanity limit
  std::vector<uint8_t> buf(len);
  if (!raw_read(fd, buf.data(), len)) return {};
  return buf;
}

// --- MsgWriter ---

void MsgWriter::put_u8(uint8_t v) { buf_.push_back(v); }

void MsgWriter::put_u32(uint32_t v) {
  buf_.insert(buf_.end(), reinterpret_cast<uint8_t *>(&v),
              reinterpret_cast<uint8_t *>(&v) + 4);
}

void MsgWriter::put_float(float v) {
  buf_.insert(buf_.end(), reinterpret_cast<uint8_t *>(&v),
              reinterpret_cast<uint8_t *>(&v) + 4);
}

void MsgWriter::put_str(const std::string &s) {
  put_u32((uint32_t)s.size());
  if (!s.empty())
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void MsgWriter::put_bytes(const void *data, size_t len) {
  auto p = static_cast<const uint8_t *>(data);
  buf_.insert(buf_.end(), p, p + len);
}

// --- MsgReader ---

bool MsgReader::get_u8(uint8_t &v) {
  if (pos_ + 1 > data_.size()) return false;
  v = data_[pos_++];
  return true;
}

bool MsgReader::get_u32(uint32_t &v) {
  if (pos_ + 4 > data_.size()) return false;
  memcpy(&v, data_.data() + pos_, 4);
  pos_ += 4;
  return true;
}

bool MsgReader::get_float(float &v) {
  if (pos_ + 4 > data_.size()) return false;
  memcpy(&v, data_.data() + pos_, 4);
  pos_ += 4;
  return true;
}

bool MsgReader::get_str(std::string &s) {
  uint32_t len;
  if (!get_u32(len)) return false;
  if (pos_ + len > data_.size()) return false;
  s.assign(reinterpret_cast<const char *>(data_.data() + pos_), len);
  pos_ += len;
  return true;
}

bool MsgReader::get_bytes(void *out, size_t len) {
  if (pos_ + len > data_.size()) return false;
  memcpy(out, data_.data() + pos_, len);
  pos_ += len;
  return true;
}

// --- Encryption ---

static void increment_nonce(uint8_t nonce[24]) {
  for (int i = 0; i < 24; i++) {
    if (++nonce[i] != 0) break;
  }
}

void crypto_init(CryptoState &state, const uint8_t *shared_key,
                 const uint8_t *local_nonce, const uint8_t *remote_nonce,
                 bool is_client) {
  memcpy(state.key, shared_key, 32);
  if (is_client) {
    memcpy(state.send_nonce, local_nonce, 24);
    memcpy(state.recv_nonce, remote_nonce, 24);
  } else {
    memcpy(state.send_nonce, local_nonce, 24);
    memcpy(state.recv_nonce, remote_nonce, 24);
  }
  state.active = true;
}

bool crypto_frame_write(int fd, CryptoState &state,
                        const uint8_t *data, uint32_t len) {
  // Layout: [mac:16][ciphertext:len]
  std::vector<uint8_t> ct(16 + len);
  crypto_aead_lock(ct.data() + 16,  // cipher_text
                   ct.data(),        // mac
                   state.key, state.send_nonce,
                   nullptr, 0, data, len);
  increment_nonce(state.send_nonce);
  return frame_write(fd, ct.data(), (uint32_t)ct.size());
}

std::vector<uint8_t> crypto_frame_read(int fd, CryptoState &state) {
  auto frame = frame_read(fd);
  if (frame.size() < 16) return {};
  uint32_t ct_len = (uint32_t)frame.size() - 16;
  std::vector<uint8_t> pt(ct_len);
  int rc = crypto_aead_unlock(pt.data(),           // plain_text
                              frame.data(),        // mac
                              state.key, state.recv_nonce,
                              nullptr, 0,
                              frame.data() + 16,   // cipher_text
                              ct_len);
  if (rc != 0) return {}; // decryption failed (wrong key)
  increment_nonce(state.recv_nonce);
  return pt;
}

// --- Handshake ---

// Generate random nonce
static void random_nonce(uint8_t out[24]) {
  // Use /dev/urandom
  int fd = open("/dev/urandom", 0);
  if (fd >= 0) {
    (void)read(fd, out, 24);
    close(fd);
  }
}

HandshakeResult client_handshake(int fd, const std::string &key_name,
                                 const uint8_t *key_bytes) {
  HandshakeResult result;
  uint8_t flags = key_bytes ? RPC_FLAG_ENCRYPTED : 0;

  // Send: [version:u8] [flags:u8] [key_name_len:u8] [key_name] [nonce:24]
  uint8_t local_nonce[24] = {};
  if (flags & RPC_FLAG_ENCRYPTED) random_nonce(local_nonce);

  raw_write(fd, &RPC_VERSION, 1);
  raw_write(fd, &flags, 1);
  uint8_t name_len = (uint8_t)key_name.size();
  raw_write(fd, &name_len, 1);
  if (name_len > 0) raw_write(fd, key_name.data(), name_len);
  raw_write(fd, local_nonce, 24);

  // Receive: [version:u8] [status:u8] [nonce:24]
  uint8_t srv_version, status;
  uint8_t remote_nonce[24];
  if (!raw_read(fd, &srv_version, 1)) return result;
  if (!raw_read(fd, &status, 1)) return result;
  if (!raw_read(fd, remote_nonce, 24)) return result;

  if (status != 0) return result; // rejected

  result.ok = true;
  result.key_name = key_name;

  if (key_bytes && (flags & RPC_FLAG_ENCRYPTED)) {
    crypto_init(result.crypto, key_bytes, local_nonce, remote_nonce, true);
    result.encrypted = true;
  }
  return result;
}

HandshakeResult server_handshake(int fd,
                                 const std::map<std::string, std::vector<uint8_t>> &keys) {
  HandshakeResult result;

  // Receive: [version:u8] [flags:u8] [key_name_len:u8] [key_name] [nonce:24]
  uint8_t version, flags, name_len;
  if (!raw_read(fd, &version, 1)) return result;
  if (!raw_read(fd, &flags, 1)) return result;
  if (!raw_read(fd, &name_len, 1)) return result;

  std::string key_name(name_len, '\0');
  if (name_len > 0 && !raw_read(fd, key_name.data(), name_len)) return result;

  uint8_t client_nonce[24];
  if (!raw_read(fd, client_nonce, 24)) return result;

  // Check encryption requirement
  uint8_t local_nonce[24] = {};
  uint8_t status = 0;

  if (!keys.empty() && !(flags & RPC_FLAG_ENCRYPTED)) {
    // Server has keys configured but client didn't request encryption — reject
    status = 1;
    raw_write(fd, &RPC_VERSION, 1);
    raw_write(fd, &status, 1);
    raw_write(fd, local_nonce, 24);
    return result;
  }

  if (flags & RPC_FLAG_ENCRYPTED) {
    if (keys.empty() || !keys.count(key_name)) {
      // Reject: unknown key
      status = 1;
      raw_write(fd, &RPC_VERSION, 1);
      raw_write(fd, &status, 1);
      raw_write(fd, local_nonce, 24);
      return result;
    }
    random_nonce(local_nonce);
  }

  // Accept
  raw_write(fd, &RPC_VERSION, 1);
  raw_write(fd, &status, 1);
  raw_write(fd, local_nonce, 24);

  result.ok = true;
  result.key_name = key_name;

  if (flags & RPC_FLAG_ENCRYPTED) {
    auto &key = keys.at(key_name);
    crypto_init(result.crypto, key.data(), local_nonce, client_nonce, false);
    result.encrypted = true;
  }
  return result;
}
