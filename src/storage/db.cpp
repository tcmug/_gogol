#include "storage/db.h"

#include <stdexcept>
#include <string>

namespace {

// Build a std::runtime_error from a message + the connection's last error.
std::runtime_error db_error(const std::string &what, sqlite3 *db) {
    std::string msg = what;
    if (db) {
        const char *em = sqlite3_errmsg(db);
        if (em && *em) {
            msg += ": ";
            msg += em;
        }
    }
    return std::runtime_error(msg);
}

} // namespace

// --- Db ---------------------------------------------------------------------

Db::Db(const std::string &path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        // db_ may be non-null even on failure; capture the message then close.
        std::runtime_error err = db_error("failed to open database '" + path + "'", db_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw err;
    }
    apply_pragmas();
    ensure_schema();
}

Db::~Db() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Db::exec(const std::string &sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = "exec failed";
        if (err) {
            msg += ": ";
            msg += err;
            sqlite3_free(err);
        }
        throw std::runtime_error(msg);
    }
}

uint32_t Db::user_version() {
    Stmt stmt(*this, "PRAGMA user_version");
    if (!stmt.step()) {
        throw std::runtime_error("PRAGMA user_version returned no row");
    }
    return static_cast<uint32_t>(stmt.column_int64(0));
}

void Db::set_user_version(uint32_t version) {
    // PRAGMA does not accept bound parameters, so the value is formatted in.
    // This is safe: the argument is an integer, never untrusted text.
    exec("PRAGMA user_version = " + std::to_string(version));
}

void Db::apply_pragmas() {
    // WAL for concurrent readers + single writer; FULL fsync for durability of
    // precious (sole-copy) data; foreign_keys on for referential integrity.
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous = FULL");
    exec("PRAGMA foreign_keys = ON");
    // Wait out a transient writer lock instead of failing with SQLITE_BUSY.
    // Read commands (calls/affected/metrics/explore) open the index DB directly
    // while the daemon may briefly hold the write lock during a reindex; without
    // this they could crash with "database is locked". 5s is generous — writes
    // are short — and readers under WAL don't block on the writer anyway except
    // at the moment of a checkpoint/commit.
    exec("PRAGMA busy_timeout = 5000");
    exec("PRAGMA application_id = " + std::to_string(Db::kApplicationId));
}

void Db::ensure_schema() {
    // Idempotent: all CREATE ... IF NOT EXISTS. Mirrors the schema in
    // docs/design/storage-architecture.md.
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS entries(
  rowid INTEGER PRIMARY KEY,
  proto INT, path TEXT, chunk TEXT, line INT, end_line INT, hash INT,
  embedding BLOB
);

CREATE TABLE IF NOT EXISTS notes(
  topic TEXT PRIMARY KEY, content TEXT, timestamp INT, sources TEXT
);

CREATE TABLE IF NOT EXISTS glossary(
  term TEXT PRIMARY KEY, expansion TEXT
);

CREATE TABLE IF NOT EXISTS edges(
  kind TEXT, src TEXT, dst TEXT, file TEXT, line INT, attrs TEXT
);
CREATE INDEX IF NOT EXISTS edges_src ON edges(kind, src);
CREATE INDEX IF NOT EXISTS edges_dst ON edges(kind, dst);

CREATE TABLE IF NOT EXISTS exports(file TEXT, symbol TEXT);

CREATE TABLE IF NOT EXISTS metrics(
  file TEXT, name TEXT, complexity INT, lines INT,
  params INT, returns INT, max_depth INT
);

-- External-content FTS5 index over the entries table. The indexed columns MUST
-- map 1:1 to real columns of the content table (entries), so we index (path,
-- chunk) only — the two text columns that exist. (`content` was named here
-- before but there is no entries.content column, which makes the FTS table
-- unusable: any MATCH/rebuild errors with "no such column".) content_rowid maps
-- FTS rowids to entries.rowid so a MATCH result's rowid is the entry's rowid.
-- External-content tables store no copy of the text; after any bulk change to
-- entries the index must be rebuilt: INSERT INTO entries_fts(entries_fts)
-- VALUES('rebuild') — done at the end of save_index_locked.
CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
  path, chunk, content='entries', content_rowid='rowid'
);
)SQL");

    repair_fts_schema();
}

void Db::repair_fts_schema() {
    // Legacy DBs (migrated before this change) created entries_fts with a
    // phantom `content` column (fts5(path, chunk, content, content='entries')).
    // There is no entries.content, so that table is unusable — every MATCH or
    // 'rebuild' errors with "no such column: content". entries_fts is DERIVED
    // data (a rebuildable index over entries), never precious, so it is safe to
    // drop + recreate to the correct 2-column shape and repopulate from entries.
    //
    // Detection: the FTS table exposes its indexed columns via table_info. If it
    // has any column other than (path, chunk) — i.e. the phantom `content` — the
    // schema is stale and we recreate it.
    bool stale = false;
    bool has_content_col = false;
    {
        Stmt q(*this, "PRAGMA table_info(entries_fts)");
        while (q.step()) {
            std::string col = q.column_text(1); // column name
            if (col == "content") has_content_col = true;
        }
    }
    stale = has_content_col;

    if (stale) {
        exec("DROP TABLE IF EXISTS entries_fts");
        exec(R"SQL(
CREATE VIRTUAL TABLE entries_fts USING fts5(
  path, chunk, content='entries', content_rowid='rowid'
);
)SQL");
        // Repopulate from the (unchanged) entries table so keyword search works
        // immediately, without requiring a full reindex of the underlying files.
        exec("INSERT INTO entries_fts(entries_fts) VALUES('rebuild')");
    }
}

// --- Db::Stmt ---------------------------------------------------------------

Db::Stmt::Stmt(Db &db, const std::string &sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db_.raw(), sql.c_str(),
                                static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw db_error("prepare failed for '" + sql + "'", db_.raw());
    }
}

Db::Stmt::~Stmt() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

void Db::Stmt::bind(int index, int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
        throw db_error("bind int64 failed", db_.raw());
    }
}

void Db::Stmt::bind(int index, const std::string &value) {
    // SQLITE_TRANSIENT: sqlite copies the bytes, so value need not outlive bind.
    if (sqlite3_bind_text(stmt_, index, value.c_str(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw db_error("bind text failed", db_.raw());
    }
}

void Db::Stmt::bind(int index, const void *data, int size) {
    if (sqlite3_bind_blob(stmt_, index, data, size, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw db_error("bind blob failed", db_.raw());
    }
}

void Db::Stmt::bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
        throw db_error("bind null failed", db_.raw());
    }
}

bool Db::Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw db_error("step failed", db_.raw());
}

int64_t Db::Stmt::column_int64(int col) {
    return sqlite3_column_int64(stmt_, col);
}

std::string Db::Stmt::column_text(int col) {
    const unsigned char *txt = sqlite3_column_text(stmt_, col);
    if (!txt) {
        return std::string();
    }
    int n = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char *>(txt), static_cast<size_t>(n));
}

void Db::Stmt::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

// --- Db::Tx -----------------------------------------------------------------

Db::Tx::Tx(Db &db) : db_(db) {
    db_.exec("BEGIN");
}

Db::Tx::~Tx() {
    if (active_) {
        // Best-effort rollback; never throw from a destructor.
        char *err = nullptr;
        sqlite3_exec(db_.raw(), "ROLLBACK", nullptr, nullptr, &err);
        if (err) {
            sqlite3_free(err);
        }
    }
}

void Db::Tx::commit() {
    if (!active_) {
        return;
    }
    db_.exec("COMMIT");
    active_ = false;
}
