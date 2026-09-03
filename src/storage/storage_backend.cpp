// storage_backend.cpp — open_backend() factory.
#include "storage/storage_backend.h"
#include "storage/sqlite_backend.h"

#include <string>

std::unique_ptr<StorageBackend> open_backend(const std::string &index) {
    // SQLite-only: every index lives in a single <index>.db SQLite database.
    // The legacy pile-of-files FileBackend and the .db-existence/schema-version
    // probe (plus the GOGOL_BACKEND env override) were removed — SqliteBackend
    // is the sole storage backend. A fresh index simply gets an empty .db.
    return std::make_unique<SqliteBackend>(index);
}

bool is_sqlite_backed(const std::string &) {
    // SQLite is now the only backend, so this is always true. Kept as a stable
    // symbol for callers that branch on it (e.g. FTS5 keyword search).
    return true;
}
