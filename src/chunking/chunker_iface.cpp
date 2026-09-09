// chunker_iface.cpp — Chunker implementations
#include "chunking/chunker_iface.h"
#include "chunking/ts_chunker.h"
#include "config/config.h"

#include <regex>

bool TreeSitterChunker::supports(const std::string &ext) const {
  return has_treesitter_grammar(ext);
}

std::vector<Chunk> TreeSitterChunker::chunk(const std::string &content,
                                            const std::string &path) {
  std::string ext = path.substr(path.rfind('.'));
  auto chunks = chunk_treesitter(content, ext);
  if (chunks.empty())
    return chunk_window(content); // fallback on parse failure
  return chunks;
}

std::vector<Chunk> WindowChunker::chunk(const std::string &content,
                                        const std::string &) {
  return chunk_window(content);
}

// --- RegexChunker ---

bool RegexChunker::supports(const std::string &ext) const {
  for (auto &e : exts_)
    if (e == ext) return true;
  return false;
}

std::vector<Chunk> RegexChunker::chunk(const std::string &content,
                                       const std::string &) {
  std::regex re(pattern_);
  std::vector<Chunk> chunks;
  std::string heading;
  std::string body;
  uint32_t chunk_start = 1;
  uint32_t current_line = 1;

  size_t i = 0;
  while (i < content.size()) {
    size_t eol = content.find('\n', i);
    if (eol == std::string::npos) eol = content.size();
    std::string line = content.substr(i, eol - i);

    bool is_split = std::regex_search(line, re);

    if (is_split && (!body.empty() || !heading.empty())) {
      chunks.push_back({heading, chunk_start, 0, body});
      body.clear();
      // Heading: line up to first ( or {, trimmed
      size_t end = line.find_first_of("({");
      heading = (end != std::string::npos) ? line.substr(0, end) : line;
      while (!heading.empty() && heading.back() == ' ') heading.pop_back();
      chunk_start = current_line;
    } else if (is_split && body.empty()) {
      size_t end = line.find_first_of("({");
      heading = (end != std::string::npos) ? line.substr(0, end) : line;
      while (!heading.empty() && heading.back() == ' ') heading.pop_back();
      chunk_start = current_line;
    }

    if (!body.empty()) body += '\n';
    body += line;

    i = eol + 1;
    current_line++;
  }

  if (!body.empty() || !heading.empty())
    chunks.push_back({heading, chunk_start, 0, body});

  return chunks;
}

// --- default_chunkers ---

std::vector<std::unique_ptr<IChunker>> default_chunkers() {
  std::vector<std::unique_ptr<IChunker>> chunkers;

  // User-defined chunkers from config (take priority)
  auto config_chunkers = load_chunker_config();
  for (auto &def : config_chunkers) {
    chunkers.push_back(std::make_unique<RegexChunker>(def.extensions, def.pattern));
  }

  // Built-in: Markdown
  chunkers.push_back(std::make_unique<RegexChunker>(
      std::vector<std::string>{".md", ".mdx"},
      "^#{1,2} "));

  chunkers.push_back(std::make_unique<TreeSitterChunker>());

  // Built-in: GraphQL
  chunkers.push_back(std::make_unique<RegexChunker>(
      std::vector<std::string>{".graphql", ".gql"},
      "^(query|mutation|fragment|type|input|enum|interface|scalar|extend)\\b"));

  // Built-in: YAML
  chunkers.push_back(std::make_unique<RegexChunker>(
      std::vector<std::string>{".yml", ".yaml"},
      "^[a-zA-Z]"));

  // Built-in: SQL
  chunkers.push_back(std::make_unique<RegexChunker>(
      std::vector<std::string>{".sql"},
      "^(CREATE|ALTER|DROP|INSERT|UPDATE|DELETE|SELECT|--\\s+Migration)"));

  chunkers.push_back(std::make_unique<WindowChunker>()); // must be last
  return chunkers;
}
