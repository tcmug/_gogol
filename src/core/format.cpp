// format.cpp — Output formatting utilities
#include "core/format.h"
#include "core/loc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

OutputFormat g_format = OutputFormat::DEFAULT;
PathMode g_path_mode = PathMode::FULL;

OutputFormat detect_format() {
  if (const char *fmt = std::getenv("GOGOL_FORMAT")) {
    if (std::strcmp(fmt, "agent") == 0 || std::strcmp(fmt, "compact") == 0)
      return OutputFormat::AGENT;
    if (std::strcmp(fmt, "default") == 0) return OutputFormat::DEFAULT;
  }
  if (!isatty(STDOUT_FILENO)) return OutputFormat::AGENT;
  return OutputFormat::DEFAULT;
}

std::string format_stale_suffix(const std::string &status) {
  if (status == "stale") return g_format == OutputFormat::AGENT ? "~" : " (stale)";
  if (status == "missing") return g_format == OutputFormat::AGENT ? "!" : " (missing)";
  return "";
}

std::string format_location(const std::string &index_name, const std::string &rel_path,
                            uint32_t line, const std::string &chunk,
                            const std::map<std::string, IndexConfig> &configs,
                            uint32_t end_line) {
  std::string line_str = std::to_string(line);
  if (end_line > line) line_str += "-" + std::to_string(end_line);

  PathMode mode = g_path_mode;

  std::string loc;
  switch (mode) {
  case PathMode::ABS: {
    auto it = configs.find(index_name);
    if (it != configs.end() && !it->second.paths.empty())
      loc = (fs::path(it->second.paths[0]) / rel_path).string() + ":" + line_str;
    else
      loc = index_name + ":" + rel_path + ":" + line_str;
    break;
  }
  case PathMode::SHORT:
    loc = fs::path(rel_path).filename().string() + ":" + line_str;
    break;
  case PathMode::FULL:
  default:
    loc = index_name + ":" + rel_path + ":" + line_str;
    break;
  }

  if (!chunk.empty()) {
    if (g_format == OutputFormat::AGENT)
      loc += " " + chunk;
    else
      loc += " \xC2\xA7 " + chunk;
  }
  return loc;
}

std::string format_doc_ref(const std::string &index_name, const std::string &rel_path,
                           uint32_t line, const std::string &chunk,
                           const std::map<std::string, IndexConfig> &configs) {
  std::string line_str = std::to_string(line);
  std::string pathspec;
  switch (g_path_mode) {
  case PathMode::ABS: {
    auto it = configs.find(index_name);
    if (it != configs.end() && !it->second.paths.empty())
      pathspec = (fs::path(it->second.paths[0]) / rel_path).string() + ":" + line_str;
    else
      pathspec = rel_path + ":" + line_str;
    break;
  }
  case PathMode::SHORT:
    pathspec = fs::path(rel_path).filename().string() + ":" + line_str;
    break;
  case PathMode::FULL:
  default:
    pathspec = rel_path + ":" + line_str;
    break;
  }
  if (!chunk.empty())
    pathspec += (g_format == OutputFormat::AGENT ? " " : " \xC2\xA7 ") + chunk;
  return std::string(entry_type_str(EntryType::DOC)) + " " + index_name + " " + pathspec;
}

std::string format_path(const std::string &index_name, const std::string &rel_path,
                        uint32_t line,
                        const std::map<std::string, IndexConfig> &configs) {
  return format_doc_ref(index_name, rel_path, line, "", configs);
}

void print_result(const SearchResult &r, bool scores,
                  const std::map<std::string, IndexConfig> &configs,
                  const std::string &extra) {
  std::string suffix = format_stale_suffix(r.status) + extra;

  // Format location from raw fields. proto: 0=doc, 1=note, 2=term.
  std::string loc;
  if (r.proto == 1) { // note
    loc = format_loc(EntryType::NOTE, r.index, r.path);
  } else {            // doc: "doc <index> <path>:<line> § <chunk>", round-trips
    loc = format_doc_ref(r.index, r.path, r.line, r.chunk, configs);
  }

  if (g_format == OutputFormat::AGENT) {
    if (scores)
      printf("%.3f %s%s\n", r.cosine, loc.c_str(), suffix.c_str());
    else
      printf("%s%s\n", loc.c_str(), suffix.c_str());
  } else {
    if (scores)
      printf("%.4f\t%.4f\t%s%s\n", r.score, r.cosine, loc.c_str(), suffix.c_str());
    else
      printf("%s%s\n", loc.c_str(), suffix.c_str());
  }

  if (!r.snippet.empty()) {
    std::istringstream iss(r.snippet);
    std::string line;
    while (std::getline(iss, line))
      printf("\t%s\n", line.c_str());
    printf("\n");
  }
}

std::string format_index_summary(const std::string &name, const std::string &mode,
                                 int file_count, int mem_count, const std::string &path) {
  char buf[256];
  if (g_format == OutputFormat::AGENT) {
    snprintf(buf, sizeof(buf), "%s\t%s\t%d\t%d",
             name.c_str(), mode.c_str(), file_count, mem_count);
  } else {
    snprintf(buf, sizeof(buf), "%-12s %-4s %5d file %4d mem  %s",
             name.c_str(), mode.c_str(), file_count, mem_count, path.c_str());
  }
  return buf;
}
