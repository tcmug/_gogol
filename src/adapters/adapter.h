// adapter.h — the chunk type produced by scanning/chunking a source tree.
// (There is no Adapter base class anymore: FileAdapter is the only backend and is used
// concretely. The former virtual interface — protocol/check_stale/get_content — was a
// speculative seam for a mem:// backend that never materialized; it was all dead.)
#pragma once
#include <cstdint>
#include <string>

// A chunk ready for embedding.
struct AdapterChunk {
  std::string key;       // repo-relative path (unique within the scan)
  std::string heading;   // chunk heading (for display + symbol extraction)
  uint32_t line = 0;     // line offset
  uint32_t end_line = 0; // end line (0 = unknown)
  uint64_t hash = 0;     // change-detection hash (0 = always re-embed)
  std::string text;      // text to embed (context-enriched)
};
