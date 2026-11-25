#include "config/config.h"
#include "config/debug.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>

namespace fs = std::filesystem;

static std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r");
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\r");
  return s.substr(start, end - start + 1);
}

std::string expand_tilde(const std::string &path) {
  if (path.empty() || path[0] != '~') return path;
  // Both a bare "~" and "~/..." expand; "~user" is left as-is (uncommon, unsupported).
  if (path.size() == 1 || path[1] == '/') {
    if (const char *home = std::getenv("HOME"))
      return std::string(home) + path.substr(1);
  }
  return path;
}

// --- gogol home (single overridable source) ---
std::string gogol_home() {
  // $GOGOL_HOME wins (lets you relocate the whole store / test in isolation);
  // else the $HOME/.gogol convention; else CWD-relative as a last resort.
  if (const char *h = std::getenv("GOGOL_HOME")) {
    if (h[0]) return h;
  }
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.gogol";
  }
  return ".gogol";
}

// The derived/disposable store (Stage 3): code+doc chunk embeddings, graph edges,
// metrics, FTS, AND note embeddings — everything re-derivable by re-indexing. Named
// cache.db to make its durability class honest: any force/rebuild/reset path only
// ever touches this file, so nothing of record is lost. Routes through gogol_home()
// so GOGOL_HOME isolates it.
std::string cache_db_path() { return gogol_home() + "/cache.db"; }
// The disposable-but-NOT-re-derivable note store (Stage 3): the note CONTENT of
// record + its scope_path + its embedding, kept in its own small file so a cache
// rebuild never destroys notes. Gets only narrow ops (add-one, forget-one-by-id);
// no bulk/rebuild surface ever touches it. Routes through gogol_home().
std::string notes_db_path() { return gogol_home() + "/notes.db"; }
std::string messages_db_path() { return gogol_home() + "/messages.db"; }

std::string model_path() {
  // GOGOL_MODEL override (test an alt model without repointing the symlink),
  // else the fixed convention: gogol_home()/model (a symlink to the active .gguf).
  if (const char *m = std::getenv("GOGOL_MODEL")) {
    if (m[0]) return m;
  }
  return gogol_home() + "/model";
}

// The INI config file. Routes through gogol_home() (like cache/notes/model) so
// GOGOL_HOME relocates it too — an isolated GOGOL_HOME gets an isolated config, not the
// real ~/.gogol/config. Unset GOGOL_HOME => ~/.gogol/config as before.
std::string config_file_path() { return gogol_home() + "/config"; }

GlobalConfig load_global_config() {
  GlobalConfig gc;
  std::string config_path = config_file_path();
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
      else if (key == "http_port") gc.http_port = std::atoi(value.c_str());
      else if (key == "http_host") gc.http_host = value;
      else if (key == "socket")
        gc.socket_enabled = (value == "true" || value == "1" || value == "yes" || value == "on");
      else if (key == "socket_path") gc.socket_path = expand_tilde(value);
      else if (key == "idle_timeout_secs") gc.idle_timeout_secs = std::atoi(value.c_str());
      else if (key == "messaging")
        gc.messaging = (value == "true" || value == "1" || value == "yes" || value == "on");
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
      gc.model = expand_tilde(value);
    else if (key == "query_prefix" || key == "document_prefix") {
      // Prefixes are whitespace-sensitive (nomic's ends in a space). trim() already
      // stripped outer whitespace, but a quoted value ("...: ") preserves an intended
      // trailing space INSIDE the quotes — so unquote to recover it verbatim. Setting
      // either key OVERRIDES the architecture auto-selection (std::nullopt = auto).
      std::string v = value;
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
      if (key == "query_prefix") gc.query_prefix = v; else gc.document_prefix = v;
    }
    // (batch_size / tcp / watch / watch_debounce_ms / precision were removed — unused;
    //  an old config still containing them is ignored harmlessly.)
    //  and unused; an old config still containing them is ignored harmlessly.)
  }
  return gc;
}


std::vector<ChunkerDef> load_chunker_config() {
  std::vector<ChunkerDef> result;
  std::string config_path = config_file_path();
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

// --- Automatic daily backup of notes.db (Fibonacci retention up to 13 days) ---
//
// cache.db is derived/rebuildable and so is NOT backed up. notes.db is the
// LLM-authored content of record (not re-derivable), so it alone is backed up.
//
// Layout: gogol_home()/backups/YYYY-MM-DD/notes.db (+ .wal/.shm sidecars if present).
// The dated dir name is the per-day guard — a second invocation the same calendar
// day sees the dir exists and is a no-op.
//
// Retention: keep backups aged {1,2,3,5,8,13} days; delete anything older than 13
// days and anything whose age isn't in the Fibonacci set. Steady state = 6 snaps.

namespace {

// Today's date as YYYY-MM-DD (local time, calendar day — the unit the user thinks
// in). We deliberately do NOT use UTC: a "daily" backup that rolls at 00:00 UTC
// would surprise someone on a non-UTC timezone.
std::string today_date() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

// Days between a YYYY-MM-DD date string and now. Returns -1 if the string isn't a
// parseable date. Used to age backups for retention. Calendar-day granularity, so
// a backup taken at 23:59 and evaluated at 00:01 the next day counts as age 1.
int age_in_days(const std::string &date) {
  // Parse YYYY-MM-DD. std::get_time would be cleaner but localtime_r/mktime round-
  // trip is robust and avoids locale quirks.
  if (date.size() < 10 || date[4] != '-' || date[7] != '-') return -1;
  std::tm tm{};
  tm.tm_year = std::atoi(date.substr(0, 4).c_str()) - 1900;
  tm.tm_mon = std::atoi(date.substr(5, 2).c_str()) - 1;
  tm.tm_mday = std::atoi(date.substr(8, 2).c_str());
  tm.tm_hour = 12;  // noon to avoid DST edge-rounding
  std::time_t t = std::mktime(&tm);
  if (t == static_cast<std::time_t>(-1)) return -1;
  double diff = std::difftime(std::time(nullptr), t);
  return static_cast<int>(diff / 86400.0);  // truncate; <24h = 0, but we treat 0 as 1 below
}

// Fibonacci retention ages, in days, up to 13. A backup is kept iff its age is one
// of these (or it's today's fresh backup, age 0). Anything else is pruned.
constexpr int kFibAges[] = {1, 2, 3, 5, 8, 13};
constexpr int kMaxAge = 13;

bool is_fib_age(int age) {
  for (int a : kFibAges)
    if (a == age) return true;
  return false;
}

// Copy a single file if it exists. Best-effort: swallows errors, returns success.
// Used for notes.db and its -wal/-shm sidecars (which may or may not exist).
bool copy_file_if_exists(const fs::path &src, const fs::path &dst) {
  std::error_code ec;
  if (!fs::exists(src, ec)) return true;  // missing sidecar is fine
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    DBG_WARN("backup: copy %s -> %s failed: %s",
             src.c_str(), dst.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

}  // namespace

void maybe_backup_dbs() {
  std::string home = gogol_home();
  fs::path notes = fs::path(home) / "notes.db";
  std::error_code ec;

  // Fresh install or isolated GOGOL_HOME with no notes yet: nothing to back up.
  if (!fs::exists(notes, ec)) return;

  fs::path backup_root = fs::path(home) / "backups";
  fs::path today_dir = backup_root / today_date();

  // Per-day guard: if today's backup dir already exists, this invocation is a
  // no-op for the backup step. (Retention pruning still runs below — a missed
  // prune yesterday should still get cleaned up today.)
  bool need_backup = !fs::exists(today_dir, ec);
  if (need_backup) {
    fs::create_directories(today_dir, ec);
    if (ec) {
      DBG_WARN("backup: mkdir %s failed: %s", today_dir.c_str(), ec.message().c_str());
      return;  // can't make the dir -> can't back up; don't block the invocation
    }
    // Copy notes.db + sidecars. Overwrite_existing handles the rare case where
    // the dir was created by us but a prior copy was interrupted.
    copy_file_if_exists(notes, today_dir / "notes.db");
    copy_file_if_exists(fs::path(home) / "notes.db-wal", today_dir / "notes.db-wal");
    copy_file_if_exists(fs::path(home) / "notes.db-shm", today_dir / "notes.db-shm");
    DBG("backup: wrote %s", today_dir.c_str());
  }

  // --- Retention: prune non-Fibonacci-aged and >13-day-old backups ---
  // Iterate the dated subdirs of backups/. Each dir name is YYYY-MM-DD; age it,
  // keep iff age==0 (today's fresh one) or age is in kFibAges, else remove.
  std::error_code list_ec;
  for (auto &entry : fs::directory_iterator(backup_root, list_ec)) {
    if (list_ec) break;  // couldn't list -> skip pruning, don't block
    if (!entry.is_directory(ec)) continue;
    std::string name = entry.path().filename().string();
    int age = age_in_days(name);
    if (age < 0) continue;  // not a date-stamped dir — leave user files alone
    bool keep = (age == 0) || is_fib_age(age);
    if (age > kMaxAge) keep = false;  // hard cap beyond 13 days
    if (!keep) {
      std::error_code rm_ec;
      fs::remove_all(entry.path(), rm_ec);
      if (rm_ec)
        DBG_WARN("backup: prune %s failed: %s", entry.path().c_str(),
                 rm_ec.message().c_str());
      else
        DBG("backup: pruned %s (age %d)", name.c_str(), age);
    }
  }
}
