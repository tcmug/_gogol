// loc.cpp — Entry types and location formatting (no sigils).
#include "core/loc.h"
#include <cctype>

using std::string;

bool parse_entry_type(const string &s, EntryType &out) {
  if (s == "doc") { out = EntryType::DOC; return true; }
  if (s == "note") { out = EntryType::NOTE; return true; }
  if (s == "term") { out = EntryType::TERM; return true; }
  return false;
}

const char *entry_type_str(EntryType t) {
  switch (t) {
  case EntryType::NOTE: return "note";
  case EntryType::TERM: return "term";
  default: return "doc";
  }
}

void split_path_line(const string &in, string &path, uint32_t &line) {
  path = in;
  line = 0;
  auto colon = in.rfind(':');
  if (colon != string::npos && colon + 1 < in.size()) {
    bool all_digits = true;
    for (size_t i = colon + 1; i < in.size(); i++)
      if (!std::isdigit((unsigned char)in[i])) { all_digits = false; break; }
    if (all_digits) {
      line = (uint32_t)std::atoi(in.substr(colon + 1).c_str());
      path = in.substr(0, colon);
    }
  }
}

string format_loc(EntryType type, const string &index_name, const string &path,
                  uint32_t line, const string &chunk) {
  string r = string(entry_type_str(type)) + " " + index_name + " " + path;
  if (type == EntryType::DOC) {
    if (line > 0) r += ":" + std::to_string(line);
    if (!chunk.empty()) r += " \xC2\xA7 " + chunk;
  }
  return r;
}
