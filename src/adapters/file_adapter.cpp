#include "adapters/file_adapter.h"
#include "config/scanner.h"
#include "storage/storage_backend.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

using std::map;
using std::set;
using std::string;
using std::vector;

// Split camelCase/PascalCase/snake_case identifiers into words
static string split_identifiers(const string &heading,
                                     const string &siblings) {
  set<string> words;
  auto extract = [&](const string &s) {
    string word;
    for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (c == '_' || c == '(' || c == ')' || c == ',' || c == ' ' ||
          c == ':' || c == '<' || c == '>' || c == '[' || c == ']') {
        if (word.size() > 2) {
          std::transform(word.begin(), word.end(), word.begin(), ::tolower);
          words.insert(word);
        }
        word.clear();
      } else if (std::isupper(c) && !word.empty() && std::islower(word.back())) {
        // camelCase split point
        if (word.size() > 2) {
          std::transform(word.begin(), word.end(), word.begin(), ::tolower);
          words.insert(word);
        }
        word.clear();
        word += (char)std::tolower(c);
      } else {
        word += c;
      }
    }
    if (word.size() > 2) {
      std::transform(word.begin(), word.end(), word.begin(), ::tolower);
      words.insert(word);
    }
  };

  extract(heading);
  extract(siblings);

  if (words.empty()) return "";
  string result;
  for (auto &w : words) {
    if (!result.empty()) result += " ";
    result += w;
  }
  return result;
}

// Enrich a chunk's text with contextual information for better semantic search.
// Produces: {dir_context} (also: {siblings}): {path} § {heading}. {code} [{split_ids}] {glossary}
static string enrich_embed_text(const string &rel_path,
                                     const string &heading,
                                     const string &content,
                                     const string &dir_context,
                                     const string &siblings,
                                     const map<string, string> &glossary) {
  string text;
  if (!dir_context.empty())
    text = dir_context;
  if (!siblings.empty())
    text += " (also: " + siblings + ")";
  if (!text.empty())
    text += ": ";
  text += rel_path;
  if (!heading.empty())
    text += " \xC2\xA7 " + heading;
  text += ". " + content;

  // Append split identifiers for natural language matching
  string split_words = split_identifiers(heading, siblings);
  if (!split_words.empty())
    text += " [" + split_words + "]";

  // Append glossary expansions for matched terms
  if (!glossary.empty()) {
    string expansions;
    string upper_text = heading + " " + split_words + " " + content;
    std::transform(upper_text.begin(), upper_text.end(), upper_text.begin(), ::toupper);
    for (auto &[term, expansion] : glossary) {
      string upper_term = term;
      std::transform(upper_term.begin(), upper_term.end(), upper_term.begin(), ::toupper);
      if (upper_text.find(upper_term) != string::npos) {
        if (!expansions.empty()) expansions += ". ";
        expansions += term + ": " + expansion;
      }
    }
    if (!expansions.empty())
      text += " {" + expansions + "}";
  }

  return text;
}

FileAdapter::FileAdapter(const string &name, const IndexConfig &config)
    : name_(name), config_(config), writable_(config.is_writable()),
      chunkers_(default_chunkers()),
      // Glossary load routes through the StorageBackend (open_backend) so the
      // glossary comes from the same backend as the rest of the index. Safe:
      // storage_backend.h includes only store headers, never adapter headers.
      glossary_(open_backend(name)->load_glossary()) {
  if (!config_.paths.empty() && fs::exists(config_.paths[0])) {
    root_ = fs::canonical(config_.paths[0]).string();
  } else if (!config_.paths.empty()) {
    root_ = config_.paths[0];
  }
}

vector<AdapterChunk>
FileAdapter::scan(bool force,
                  const map<string, uint64_t> &existing_hashes) {
  vector<AdapterChunk> result;

  auto extensions = config_.extensions;
  if (extensions.empty())
    extensions = {".md"};

  // Scan all configured paths
  vector<std::pair<string, uint64_t>> files;
  for (auto &sp : config_.paths) {
    fs::path p(sp);
    if (!fs::exists(p))
      continue;
    auto scanned = scan_md_files(p, extensions);
    string canon = fs::canonical(p).string();
    for (auto &[rel, hash] : scanned) {
      if (config_.paths.size() > 1) {
        string abs = (fs::path(canon) / rel).string();
        files.emplace_back(abs, hash);
      } else {
        files.emplace_back(rel, hash);
      }
    }
  }

  for (auto &[rel_path, hash] : files) {
    all_paths_.insert(rel_path);

    // Skip unchanged files
    if (!force) {
      auto it = existing_hashes.find(rel_path);
      if (it != existing_hashes.end() && it->second == hash)
        continue;
    }

    // Read and chunk
    string abs_path =
        (rel_path[0] == '/') ? rel_path : (fs::path(root_) / rel_path).string();
    string content = read_file(abs_path);
    if (content.empty())
      continue;

    string ext = fs::path(rel_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    vector<Chunk> chunks;
    for (auto &chunker : chunkers_) {
      if (chunker->supports(ext)) {
        chunks = chunker->chunk(content, rel_path);
        break;
      }
    }

    for (auto &c : chunks) {
      // Build context-enriched embed text
      // Extract meaningful directory components as module context
      string context;
      {
        auto slash = rel_path.rfind('/');
        string dir = (slash != string::npos) ? rel_path.substr(0, slash) : "";
        vector<string> parts;
        std::istringstream iss(dir);
        string part;
        while (std::getline(iss, part, '/')) {
          if (!part.empty() && part != "src" && part != "lib" &&
              part != "services" && part != "backend" && part != "frontend")
            parts.push_back(part);
        }
        size_t start = parts.size() > 2 ? parts.size() - 2 : 0;
        for (size_t i = start; i < parts.size(); i++) {
          if (!context.empty()) context += "/";
          context += parts[i];
        }
      }

      // Collect sibling headings (other chunks in same file)
      string siblings;
      for (auto &s : chunks) {
        if (&s == &c || s.heading.empty() || s.heading == c.heading) continue;
        if (!siblings.empty()) siblings += ", ";
        siblings += s.heading;
        if (siblings.size() > 200) break; // cap length
      }

      string embed_text = enrich_embed_text(rel_path, c.heading, c.content,
                                                  context, siblings, glossary_);

      result.push_back(
          {rel_path, c.heading, c.line, c.end_line, hash, embed_text});
    }
  }

  return result;
}

EntryStatus FileAdapter::check_stale(const string &key,
                                     uint64_t stored_hash) {
  fs::path file_path =
      (key[0] == '/') ? fs::path(key) : fs::path(root_) / key;
  std::error_code ec;
  auto entry = fs::directory_entry(file_path, ec);
  if (ec || !entry.exists())
    return EntryStatus::MISSING;

  auto ftime = entry.last_write_time().time_since_epoch().count();
  auto size = entry.file_size();
  uint64_t h = 0xcbf29ce484222325ULL;
  h ^= static_cast<uint64_t>(ftime);
  h *= 0x100000001b3ULL;
  h ^= static_cast<uint64_t>(size);
  h *= 0x100000001b3ULL;
  return (h != stored_hash) ? EntryStatus::STALE : EntryStatus::OK;
}

string FileAdapter::get_content(const string &key, uint32_t line,
                                     int max_lines) {
  string abs_path =
      (key[0] == '/') ? key : (fs::path(root_) / key).string();
  string content = read_file(abs_path);
  if (content.empty())
    return {};

  std::istringstream iss(content);
  string l;
  uint32_t cur = 1;
  while (cur < line && std::getline(iss, l))
    cur++;

  string result;
  int n = 0;
  while (std::getline(iss, l)) {
    if (max_lines > 0 && n >= max_lines) break;
    if (!result.empty())
      result += "\n";
    result += l;
    n++;
  }
  return result;
}

bool FileAdapter::add(const string &key, const string &content,
                      const vector<string> &sources) {
  (void)sources;
  if (!writable_ || config_.paths.empty())
    return false;
  fs::path fpath = fs::path(config_.paths[0]) / key;
  std::error_code ec;
  fs::create_directories(fpath.parent_path(), ec);
  if (ec)
    return false;
  std::ofstream out(fpath);
  if (!out)
    return false;
  out << content << "\n";
  return out.good();
}

bool FileAdapter::remove(const string &key) {
  if (!writable_ || config_.paths.empty())
    return false;
  fs::path fpath = fs::path(config_.paths[0]) / key;
  if (!fs::exists(fpath))
    return false;
  return fs::remove(fpath);
}
