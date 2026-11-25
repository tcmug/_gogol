// model_prefixes.{h,cpp} — architecture -> retrieval-prefix table. Pure string logic,
// NO llama.cpp dependency, so it links into gogol-tests (which omits llama) too.
#pragma once
#include <string>
#include <utility>

// Built-in map from a GGUF `general.architecture` to the model's retrieval-handshake
// prefixes (query_prefix, document_prefix). The prefix convention is a per-model
// TRAINING detail documented on the model card, NOT stored in the GGUF, so it can't be
// read from the file — this table encodes the known embedding architectures. An
// unknown architecture returns {"",""} (empty = raw text; the caller may override via
// config). Keyed on the architecture string because it IS present in every GGUF.
std::pair<std::string, std::string> default_prefixes_for_arch(const std::string &arch);
