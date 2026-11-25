#include "embedding/model_prefixes.h"

std::pair<std::string, std::string> default_prefixes_for_arch(const std::string &arch) {
  // nomic-embed-text: "search_query: " / "search_document: " (trailing spaces matter).
  if (arch == "nomic-bert")
    return {"search_query: ", "search_document: "};
  // bge / generic bert: query-side retrieval instruction, document side empty.
  if (arch == "bert")
    return {"Represent this sentence for searching relevant passages: ", ""};
  // e5 family: "query: " / "passage: ".
  if (arch == "e5")
    return {"query: ", "passage: "};
  // Unknown architecture: empty prefixes (raw text). Caller may override via config.
  return {"", ""};
}
