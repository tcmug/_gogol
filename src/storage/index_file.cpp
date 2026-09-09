// Live helpers only: cosine similarity, f16 conversion, and path->index lookup.
// Index metadata + embeddings are persisted/loaded through SqliteBackend, which
// materializes every entry's embedding eagerly in load_index() (emb_loaded=true).
#include "storage/index_file.h"
#include "config/config.h"

#include <cmath>
#include <cstring>

namespace fs = std::filesystem;

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

// --- Public API ---

// Embeddings are materialized eagerly by SqliteBackend::load_index (which sets
// emb_loaded), so this is a no-op guard: the only surviving purpose is to mark
// an empty index as "loaded". The legacy .emb sidecar reader was removed with
// the SQLite-only migration.
void Index::ensure_embeddings() {
  emb_loaded = true;
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
