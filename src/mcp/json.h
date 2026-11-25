// gogol JSON seam.
//
// A thin, stable alias over the vendored JSON library so the rest of gogol
// never references the concrete implementation directly. This keeps a single
// swap point: if the underlying lib is ever replaced, only this header and its
// helpers change — call sites keep using gogol::Json.
//
// Underlying lib: nlohmann/json (single-header, MIT), vendored at
// vendor/json/json.hpp and compiled as source (header-only, no .cpp).
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace gogol {

// Stable local alias. Callers use gogol::Json, not nlohmann::json.
using Json = nlohmann::json;

// Parse text into a Json value. On failure returns false and leaves `out`
// unspecified; the error message (if provided) is filled with the parse error.
// Non-throwing seam so callers don't have to guard nlohmann's exceptions.
inline bool json_parse(const std::string& text, Json& out, std::string* error = nullptr) {
    Json parsed = Json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        if (error) *error = "invalid JSON";
        return false;
    }
    out = std::move(parsed);
    return true;
}

}  // namespace gogol
