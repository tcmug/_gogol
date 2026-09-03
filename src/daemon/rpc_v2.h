// rpc_v2.h — Framed RPC protocol with version handshake and optional encryption
// Wire format:
//   Handshake: [version:u8] [flags:u8] [key_name_len:u8] [key_name] [nonce:24]
//   Message:   [len:u32] [cmd:u8] [payload...]
//   Response:  [len:u32] [status:u8] [payload...]
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

constexpr uint8_t RPC_VERSION = 1;
constexpr uint8_t RPC_FLAG_ENCRYPTED = 0x01;

// --- Framed I/O ---

// Write a framed message: [len:u32][data]
bool frame_write(int fd, const uint8_t *data, uint32_t len);
bool frame_write(int fd, const std::vector<uint8_t> &buf);

// Read a framed message. Returns empty on failure.
std::vector<uint8_t> frame_read(int fd);

// --- Buffer helpers (serialize into a byte vector) ---

class MsgWriter {
public:
    void put_u8(uint8_t v);
    void put_u32(uint32_t v);
    void put_float(float v);
    void put_str(const std::string &s);
    void put_bytes(const void *data, size_t len);
    const std::vector<uint8_t> &data() const { return buf_; }
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

class MsgReader {
public:
    MsgReader(const std::vector<uint8_t> &data) : data_(data) {}
    MsgReader(std::vector<uint8_t> &&data) : data_(std::move(data)) {}

    bool get_u8(uint8_t &v);
    bool get_u32(uint32_t &v);
    bool get_float(float &v);
    bool get_str(std::string &s);
    bool get_bytes(void *out, size_t len);
    bool eof() const { return pos_ >= data_.size(); }
    size_t remaining() const { return data_.size() - pos_; }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

// --- Encryption layer ---

struct CryptoState {
    uint8_t key[32] = {};
    uint8_t send_nonce[24] = {};
    uint8_t recv_nonce[24] = {};
    bool active = false;
};

// Initialize crypto from shared secret (32 bytes)
void crypto_init(CryptoState &state, const uint8_t *shared_key,
                 const uint8_t *local_nonce, const uint8_t *remote_nonce,
                 bool is_client);

// Encrypt and write a framed message
bool crypto_frame_write(int fd, CryptoState &state,
                        const uint8_t *data, uint32_t len);

// Read and decrypt a framed message
std::vector<uint8_t> crypto_frame_read(int fd, CryptoState &state);

// --- Handshake ---

struct HandshakeResult {
    bool ok = false;
    bool encrypted = false;
    std::string key_name;
    CryptoState crypto;
};

// Client-side handshake. key_name empty = no encryption.
HandshakeResult client_handshake(int fd, const std::string &key_name,
                                 const uint8_t *key_bytes);

// Server-side handshake. keys map: name → 32-byte key.
// If keys is empty, encryption is disabled.
HandshakeResult server_handshake(int fd,
                                 const std::map<std::string, std::vector<uint8_t>> &keys);
