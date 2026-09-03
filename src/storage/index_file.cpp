// Read-only: loaders retained for migration; writes go through SqliteBackend.
#include "storage/index_file.h"
#include "config/debug.h"
#include "config/config.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

// --- File format magic + versions (independent per file) ---
static const uint32_t META_MAGIC = 0x474F4D54; // "GOMT"
static const uint8_t  META_VERSION = 1;

static const uint32_t EMB_MAGIC = 0x474F4542; // "GOEB"
static const uint8_t  EMB_VERSION = 1;

// Legacy format (combined .idx)
static const uint32_t LEGACY_MAGIC = 0x474F474C; // "GOGL"

// --- float16 conversion (IEEE 754 half-precision) ---

uint16_t float_to_f16(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = x & 0x7FFFFF;
  if (exp <= 0) return sign;
  if (exp >= 31) return sign | 0x7C00;
  return sign | (exp << 10) | (mant >> 13);
}

float f16_to_float(uint16_t h) {
  uint32_t sign = (h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  if (exp == 0) {
    float f = 0.0f;
    uint32_t r = sign;
    std::memcpy(&f, &r, 4);
    return f;
  }
  if (exp == 31) exp = 255;
  else exp = exp - 15 + 127;
  uint32_t result = sign | (exp << 23) | (mant << 13);
  float f;
  std::memcpy(&f, &result, 4);
  return f;
}

// --- Path helpers ---

static fs::path base_dir() {
  fs::path dir = fs::path(std::getenv("HOME")) / ".gogol";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

static fs::path index_dir() {
  fs::path dir = base_dir() / "indexes";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

// --- .meta file: metadata only ---
// (The legacy .meta/.emb/.idx FILE readers were removed with the SQLite-only
// migration. The only surviving reader is load_emb_file below, used by
// Index::ensure_embeddings() as a no-op fallback for SQLite-backed indexes
// where embeddings are already materialized eagerly.)

// --- .emb file: embeddings only ---
// Header: [EMB_MAGIC:4][EMB_VERSION:1][dim:4][precision:1][count:4]
// Data:   [embedding × count] (each: dim × sizeof(precision))

static bool load_emb_file(const std::string &name, Index &idx) {
  fs::path p = index_dir() / (name + ".emb");
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;

  uint32_t magic, dim, count; uint8_t version, prec;
  f.read(reinterpret_cast<char *>(&magic), 4);
  f.read(reinterpret_cast<char *>(&version), 1);
  f.read(reinterpret_cast<char *>(&dim), 4);
  f.read(reinterpret_cast<char *>(&prec), 1);
  f.read(reinterpret_cast<char *>(&count), 4);

  if (magic != EMB_MAGIC || version > EMB_VERSION) return false;

  idx.dim = dim;
  idx.precision = static_cast<EmbedPrecision>(prec);

  if (count != idx.entries.size()) {
    DBG("load_emb: count mismatch (emb=%u, meta=%zu)", count, idx.entries.size());
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    idx.entries[i].embedding.resize(dim);
    if (idx.precision == EmbedPrecision::F16) {
      for (uint32_t j = 0; j < dim; j++) {
        uint16_t h;
        f.read(reinterpret_cast<char *>(&h), 2);
        idx.entries[i].embedding[j] = f16_to_float(h);
      }
    } else {
      f.read(reinterpret_cast<char *>(idx.entries[i].embedding.data()),
             dim * sizeof(float));
    }
    if (!f.good()) return false;
  }
  return true;
}

// --- Public API ---

void Index::ensure_embeddings() {
  if (emb_loaded) return;
  if (entries.empty()) { emb_loaded = true; return; }
  if (load_emb_file(name_, *this)) {
    emb_loaded = true;
    DBG("ensure_embeddings: %s loaded dim=%u prec=%s", name_.c_str(), dim,
        precision == EmbedPrecision::F16 ? "f16" : "f32");
  } else {
    DBG("ensure_embeddings: %s FAILED", name_.c_str());
    emb_loaded = true; // don't retry
  }
}

// --- Utility ---

std::string find_index_for_path(const fs::path &dir) {
  std::string abs = fs::canonical(dir).string();
  auto configs = load_config();
  for (auto &[name, cfg] : configs) {
    for (auto &p : cfg.paths) {
      std::error_code ec;
      std::string canon = fs::canonical(p, ec).string();
      if (!ec && (abs == canon || abs.rfind(canon + "/", 0) == 0))
        return name;
    }
  }
  return {};
}

std::vector<IndexInfo> list_indexes() {
  std::vector<IndexInfo> results;
  auto configs = load_config();
  for (auto &[name, cfg] : configs) {
    if (fs::exists(index_dir() / (name + ".meta")) ||
        fs::exists(index_dir() / (name + ".idx"))) {
      std::string root = cfg.paths.empty() ? "" : cfg.paths[0];
      results.push_back({name, root});
    }
  }
  return results;
}

float cosine_similarity(const std::vector<float> &a,
                        const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;
  float dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); i++) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  float denom = std::sqrt(na) * std::sqrt(nb);
  return denom > 0 ? dot / denom : 0.0f;
}
