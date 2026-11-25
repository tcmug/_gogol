#include "adapters/file_adapter.h"
#include "config/scanner.h"
#include "chunking/ts_chunker.h"

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

// Strip markdown heading markup (leading #'s / bullets, trailing #'s): "## Foo &
// bar" -> "Foo & bar". Keeps embed text + display free of raw markup (markdown
// analogue of the AST node-type leak).
static string clean_md_heading(string h) {
  size_t i = 0;
  while (i < h.size() && (h[i]=='#' || h[i]==' ' || h[i]=='-' || h[i]=='*')) i++;
  h = h.substr(i);
  while (!h.empty() && (h.back()==' ' || h.back()=='#')) h.pop_back();
  return h;
}

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
// Format: {context}␞{content}␞{keywords}
// - context: dir_context (also: siblings): path § heading
// - content: the actual code/doc text  
// - keywords: split identifiers for FTS matching
// kFieldSep (␞) defined in file_adapter.h

static string enrich_embed_text(const string &rel_path,
                                     const string &heading,
                                     const string &content,
                                     const string &dir_context,
                                     const string &siblings) {
  string ctx;
  if (!dir_context.empty())
    ctx = dir_context;
  if (!siblings.empty())
    ctx += " (also: " + siblings + ")";
  if (!ctx.empty())
    ctx += ": ";
  ctx += rel_path;
  if (!heading.empty())
    ctx += " \xC2\xA7 " + heading;

  // Append split identifiers for natural language matching
  string keywords = split_identifiers(heading, siblings);

  // Join with ␞: context ␞ content ␞ keywords
  string text = ctx;
  text += kFieldSep;
  text += content;
  if (!keywords.empty()) {
    text += kFieldSep;
    text += keywords;
  }

  return text;
}

FileAdapter::FileAdapter(const string &name, const IndexConfig &config)
    : name_(name), config_(config),
      chunkers_(default_chunkers()) {
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
    auto scanned = scan_md_files(p, extensions, repo_root_);
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

      // Collect sibling headings (other chunks in same file), cleaned of markdown
      // heading markup (leading #'s, stray punctuation) so the (also: ...) carries
      // clean heading TEXT, not raw '## Foo' syntax — the markdown analogue of the
      // AST-token leak: raw markup in siblings pollutes both display and embedding.
      string own_heading = clean_md_heading(c.heading);
      string siblings;
      for (auto &s : chunks) {
        if (&s == &c || s.heading.empty() || s.heading == c.heading) continue;
        string hc = clean_md_heading(s.heading);
        if (hc.empty()) continue;
        if (!siblings.empty()) siblings += ", ";
        siblings += hc;
        if (siblings.size() > 200) break; // cap length
      }

      string embed_text = enrich_embed_text(rel_path, own_heading, c.content,
                                                  context, siblings);

      result.push_back(
          {rel_path, c.heading, c.line, c.end_line, hash, embed_text});
    }
  }

  return result;
}

