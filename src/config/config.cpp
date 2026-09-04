#include "config/config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r");
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\r");
  return s.substr(start, end - start + 1);
}

static std::string expand_home(const std::string &path) {
  if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
    if (const char *home = std::getenv("HOME")) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

static IndexMode parse_mode(const std::string &s) {
  if (s == "rw")
    return IndexMode::RW;
  return IndexMode::R;
}

std::string IndexConfig::memory_dir() const {
  if (!memory.empty())
    return memory;
  const char *home = std::getenv("HOME");
  std::string base = home ? std::string(home) : ".";
  return base + "/.gogol/memory/" + name;
}

std::map<std::string, IndexConfig> load_config() {
  std::map<std::string, IndexConfig> configs;
  fs::path config_path = fs::path(std::getenv("HOME")) / ".gogol" / "config";
  std::ifstream f(config_path);
  if (!f)
    return configs;

  std::string current_section;
  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;

    // Section header: [name]
    if (line.front() == '[' && line.back() == ']') {
      current_section = line.substr(1, line.size() - 2);
      continue;
    }
    if (current_section.empty() || current_section == "keys" ||
        current_section == "chunkers" || current_section == "global" ||
        current_section == "mcp")
      continue;

    // Key = value
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));

    if (key == "paths" || key == "path") {
      std::istringstream iss(value);
      std::string p;
      while (std::getline(iss, p, ',')) {
        p = trim(p);
        if (!p.empty())
          configs[current_section].paths.push_back(expand_home(p));
      }
    } else if (key == "ext") {
      std::istringstream iss(value);
      std::string e;
      while (std::getline(iss, e, ',')) {
        e = trim(e);
        if (!e.empty()) {
          if (e[0] != '.')
            e = "." + e;
          std::transform(e.begin(), e.end(), e.begin(), ::tolower);
          configs[current_section].extensions.push_back(e);
        }
      }
    } else if (key == "mode") {
      configs[current_section].mode = parse_mode(value);
    } else if (key == "model") {
      configs[current_section].model = expand_home(value);
    } else if (key == "memory") {
      configs[current_section].memory = expand_home(value);
    } else if (key == "watch") {
      bool on = (value == "true" || value == "1" || value == "yes");
      configs[current_section].watch_override = on ? 1 : 0;
    } else if (key == "watch_debounce_ms") {
      configs[current_section].watch_debounce_ms_override = std::atoi(value.c_str());
    }
  }
  // Fill in each index's own name (used for the default memory dir).
  for (auto &[name, cfg] : configs)
    cfg.name = name;
  return configs;
}

GlobalConfig load_global_config() {
  GlobalConfig gc;
  fs::path config_path = fs::path(std::getenv("HOME")) / ".gogol" / "config";
  std::ifstream f(config_path);
  if (!f)
    return gc;

  std::string line;
  bool in_global = true; // before first section = global (legacy top-level keys)
  bool in_mcp = false;   // inside the [mcp] section
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']') {
      // Both the legacy pre-section top-level keys AND an explicit [global]
      // section feed GlobalConfig, so either form works (back-compatible).
      in_global = (line == "[global]");
      in_mcp = (line == "[mcp]");
      continue;
    }

    if (in_mcp) {
      auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = trim(line.substr(0, eq));
      std::string value = trim(line.substr(eq + 1));
      if (key == "enabled")
        gc.mcp_enabled = (value == "true" || value == "1" || value == "yes");
      else if (key == "tools")
        gc.mcp_tools = (value == "read-write") ? "read-write" : "read";
      continue;
    }

    if (!in_global)
      continue;

    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));

    if (key == "model")
      gc.model = expand_home(value);
    else if (key == "tcp")
      gc.tcp = value;
    else if (key == "batch_size")
      gc.batch_size = std::atoi(value.c_str());
    else if (key == "watch")
      gc.watch = (value == "true" || value == "1" || value == "yes");
    else if (key == "watch_debounce_ms")
      gc.watch_debounce_ms = std::atoi(value.c_str());
    else if (key == "precision")
      gc.precision = (value == "f16") ? EmbedPrecision::F16 : EmbedPrecision::F32;
  }
  return gc;
}

bool effective_watch(const IndexConfig &idx, const GlobalConfig &g) {
  if (idx.watch_override == -1)
    return g.watch;              // inherit global default
  return idx.watch_override == 1; // explicit per-index override
}

int effective_watch_debounce_ms(const IndexConfig &idx, const GlobalConfig &g) {
  if (idx.watch_debounce_ms_override < 0)
    return g.watch_debounce_ms;  // inherit global default
  return idx.watch_debounce_ms_override;
}

static uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return 0;
}

static std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    bytes.push_back((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1]));
  return bytes;
}

std::map<std::string, std::vector<uint8_t>> load_keys() {
  std::map<std::string, std::vector<uint8_t>> keys;
  fs::path config_path = fs::path(std::getenv("HOME")) / ".gogol" / "config";
  std::ifstream f(config_path);
  if (!f) return keys;

  std::string line;
  bool in_keys = false;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      in_keys = (line == "[keys]");
      continue;
    }
    if (!in_keys) continue;

    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string name = trim(line.substr(0, eq));
    std::string hex_val = trim(line.substr(eq + 1));
    auto bytes = hex_to_bytes(hex_val);
    if (bytes.size() == 32)
      keys[name] = bytes;
  }
  return keys;
}

std::vector<ChunkerDef> load_chunker_config() {
  std::vector<ChunkerDef> result;
  fs::path config_path = fs::path(std::getenv("HOME")) / ".gogol" / "config";
  std::ifstream f(config_path);
  if (!f) return result;

  std::string line;
  bool in_chunkers = false;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      in_chunkers = (line == "[chunkers]");
      continue;
    }
    if (!in_chunkers) continue;

    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string exts_str = trim(line.substr(0, eq));
    std::string pattern = trim(line.substr(eq + 1));
    if (exts_str.empty() || pattern.empty()) continue;

    ChunkerDef def;
    def.pattern = pattern;
    // Parse comma-separated extensions
    std::istringstream iss(exts_str);
    std::string ext;
    while (std::getline(iss, ext, ',')) {
      ext = trim(ext);
      if (!ext.empty()) {
        if (ext[0] != '.') ext = "." + ext;
        def.extensions.push_back(ext);
      }
    }
    if (!def.extensions.empty())
      result.push_back(def);
  }
  return result;
}
