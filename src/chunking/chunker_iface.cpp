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

// --- RstChunker ---

bool RstChunker::supports(const std::string &ext) const {
  return ext == ".rst";
}

// True if `line` is an RST section underline: a single repeated char from
// {=, -, ~, ^, "} of length >= 3 (the minimum RST accepts). We do NOT require
// the underline to be at least as long as the title here — Godot's generated
// docs sometimes under-length the underline; requiring >= 3 is lenient and
// still rejects prose lines like "---foo---" (mixed chars) and short dashes.
static bool is_rst_underline(const std::string &line) {
  if (line.size() < 3) return false;
  char c = line[0];
  if (c != '=' && c != '-' && c != '~' && c != '^' && c != '"') return false;
  for (size_t i = 1; i < line.size(); i++)
    if (line[i] != c) return false;
  return true;
}

std::vector<Chunk> RstChunker::chunk(const std::string &content,
                                     const std::string &) {
  std::vector<Chunk> chunks;
  std::string heading;
  std::string body;
  uint32_t chunk_start = 1;
  uint32_t current_line = 1;

  // Walk lines. A chunk boundary is a non-blank line (the title) immediately
  // followed by an underline. The title becomes the heading; the chunk starts
  // at the title line (so the title + underline are included in the body).
  // We keep a one-line look-back: when we see an underline and the previous
  // emitted line was a non-blank title, we close the current chunk and start a
  // new one whose heading is that title and whose body begins with it.
  std::string prev_line;       // last non-blank line seen (candidate title)
  bool have_prev = false;

  size_t i = 0;
  while (i <= content.size()) {
    size_t eol = content.find('\n', i);
    if (eol == std::string::npos) eol = content.size();
    std::string line = content.substr(i, eol - i);
    // strip a trailing \r for CRLF files
    if (!line.empty() && line.back() == '\r') line.pop_back();

    bool is_underline = is_rst_underline(line);
    // RST section header: a non-blank title line immediately followed by an
    // underline at least as long as the title. The length check rejects
    // standalone ---- transitions that follow a long prose line (the prose
    // is not a title because the underline is far shorter than it).
    bool is_section = is_underline && have_prev && !prev_line.empty() &&
                      line.size() >= prev_line.size();
    if (is_section) {
      // Close the current chunk (everything up to but NOT including the title
      // line, which we already emitted to body). We need to remove the title
      // line from the current body since it belongs to the next chunk.
      // Easiest: rebuild body without the trailing title line.
      // Find last newline in body.
      if (!body.empty()) {
        // body ends with "...\n<title>"; strip the title line.
        size_t nl = body.rfind('\n');
        if (nl == std::string::npos) {
          // body == title only (no prior content) -> empty chunk, skip
          body.clear();
        } else {
          body = body.substr(0, nl);  // drop the trailing "\n<title>"
        }
      }
      if (!body.empty()) {
        chunks.push_back({heading, chunk_start, 0, body});
        body.clear();
      }
      // Start new chunk at the title line.
      heading = prev_line;
      chunk_start = current_line - 1;  // title was the previous line
      body = prev_line;                  // title starts the new body
      // The underline line itself is NOT appended to body (it's markup).
    } else if (!line.empty()) {
      // Regular content line (or a standalone underline with no title —
      // treat as content, do not split). Append to body.
      if (!body.empty()) body += '\n';
      body += line;
    }

    // Track the candidate title: a non-blank line that is not itself an
    // underline. Blank lines clear the candidate (a title must be immediately
    // above the underline).
    if (line.empty()) {
      prev_line.clear();
      have_prev = false;
    } else if (!is_underline) {
      prev_line = line;
      have_prev = true;
    } else {
      // underline with no preceding title: reset so we don't mis-pair
      prev_line.clear();
      have_prev = false;
    }

    if (eol == content.size()) break;
    i = eol + 1;
    current_line++;
  }

  if (!body.empty())
    chunks.push_back({heading, chunk_start, 0, body});

  // If we produced nothing usable, let the caller fall back to WindowChunker
  // by returning empty (matches TreeSitterChunker's convention).
  if (chunks.empty()) return {};
  // Collapse: if a chunk's body is just its heading line (no real content),
  // drop it — it's a header with an empty section.
  std::vector<Chunk> filtered;
  for (auto &c : chunks) {
    if (c.content == c.heading) continue;       // title-only, no body
    if (c.content.empty()) continue;
    filtered.push_back(std::move(c));
  }
  return filtered;
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

  // Built-in: reStructuredText (Godot docs, Sphinx). Dedicated chunker —
  // RST section headers are a two-line title+underline construct that the
  // line-by-line RegexChunker cannot match (see ADR-001).
  chunkers.push_back(std::make_unique<RstChunker>());

  chunkers.push_back(std::make_unique<WindowChunker>()); // must be last
  return chunkers;
}

std::vector<std::string> default_index_extensions() {
  std::vector<std::string> exts;
  // Tree-sitter code grammars: the authoritative source is has_treesitter_grammar
  // (get_language in ts_chunker.cpp). We probe the candidate set against it so this
  // list can never claim a grammar gogol doesn't actually have — add a grammar in
  // ts_chunker's get_language and it lights up here automatically.
  for (const char *e : {".ts", ".tsx", ".py", ".go", ".rs", ".c", ".cpp",
                        ".cc", ".h", ".hpp", ".php", ".gd"}) {
    if (has_treesitter_grammar(e)) exts.push_back(e);
  }
  // Built-in RegexChunker formats (mirror default_chunkers above): markdown +
  // GraphQL + YAML + SQL + reStructuredText. These have no tree-sitter grammar but are chunked by a
  // header/regex splitter, so they belong in the default index set.
  for (const char *e : {".md", ".mdx", ".graphql", ".gql", ".yml", ".yaml", ".sql", ".rst"})
    exts.push_back(e);
  return exts;
}
