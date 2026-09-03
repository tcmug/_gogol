#include "chunking/chunker.h"

std::vector<Chunk> chunk_markdown(const std::string &text) {
  std::vector<Chunk> chunks;
  std::string heading;
  std::string content;
  uint32_t chunk_start = 1;
  uint32_t current_line = 1;
  bool in_code = false;

  size_t i = 0;
  while (i < text.size()) {
    // Find end of line
    size_t eol = text.find('\n', i);
    if (eol == std::string::npos)
      eol = text.size();
    std::string line = text.substr(i, eol - i);

    // Track code fences
    if (line.size() >= 3 && line.substr(0, 3) == "```") {
      in_code = !in_code;
    }

    // Check for h1/h2 heading outside code blocks
    if (!in_code && (line.size() > 2 && line[0] == '#' &&
                     (line[1] == ' ' || (line[1] == '#' && line[2] == ' ')))) {
      // Save previous chunk
      if (!content.empty() || !heading.empty()) {
        chunks.push_back({heading, chunk_start, 0, content});
      }
      // Extract heading text
      size_t start = line.find_first_not_of("# ");
      heading = (start != std::string::npos) ? line.substr(start) : "";
      content.clear();
      chunk_start = current_line;
    } else {
      if (!content.empty())
        content += '\n';
      content += line;
    }

    i = eol + 1;
    current_line++;
  }

  // Final chunk
  if (!content.empty() || !heading.empty()) {
    chunks.push_back({heading, chunk_start, 0, content});
  }

  // Merge tiny chunks (<5 lines) into previous
  std::vector<Chunk> merged;
  for (auto &c : chunks) {
    int lines = 1;
    for (char ch : c.content)
      if (ch == '\n')
        lines++;
    if (merged.empty() || lines >= 5) {
      merged.push_back(std::move(c));
    } else {
      merged.back().content += "\n" + c.heading + "\n" + c.content;
    }
  }
  return merged;
}

std::vector<Chunk> chunk_window(const std::string &text, int window,
                                int overlap) {
  std::vector<Chunk> chunks;
  size_t i = 0;

  while (i < text.size()) {
    size_t end = std::min(i + (size_t)window, text.size());

    // Count line number at offset i
    uint32_t line = 1;
    for (size_t j = 0; j < i; j++) {
      if (text[j] == '\n')
        line++;
    }

    chunks.push_back({"", line, 0, text.substr(i, end - i)});
    i += window - overlap;
  }
  return chunks;
}
