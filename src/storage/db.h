#pragma once
#include <cstdint>
#include <string>

#include <sqlite3.h>

// Thin RAII wrapper around a single sqlite3 connection.
//
// Opens the database, applies durability PRAGMAs (WAL + synchronous=FULL +
// foreign_keys=ON), and ensures the gogol schema exists. Every sqlite3 return
// code is checked; failures throw std::runtime_error carrying sqlite3_errmsg.
//
// This is Phase 2 groundwork (see docs/design/storage-architecture.md). It does
// NOT implement SqliteBackend — it only provides the connection primitive that
// backend will be built on.
class Db {
public:
    // gogol magic constant written to PRAGMA application_id ("GOGO" as u32,
    // big-endian bytes 0x47 0x4F 0x47 0x4F). Marks a file as a gogol database.
    static constexpr int32_t kApplicationId = 0x474F474F;

    // Opens (creating if needed), applies PRAGMAs, and ensures the schema.
    explicit Db(const std::string &path);
    ~Db();

    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    // Execute one or more SQL statements with no bound params / no result rows.
    // Throws std::runtime_error on error.
    void exec(const std::string &sql);

    uint32_t user_version();
    void set_user_version(uint32_t version);

    sqlite3 *raw() { return db_; }

    // RAII prepared statement. Wraps a sqlite3_stmt*, finalizes on destruction.
    // Bind params with 1-based indices; read columns with 0-based indices.
    // step() returns true while a row is available (SQLITE_ROW), false at
    // SQLITE_DONE. Any sqlite3 error throws std::runtime_error.
    class Stmt {
    public:
        Stmt(Db &db, const std::string &sql);
        ~Stmt();

        Stmt(const Stmt &) = delete;
        Stmt &operator=(const Stmt &) = delete;

        // Bind params (1-based index, per sqlite3 convention).
        void bind(int index, int64_t value);
        void bind(int index, const std::string &value);
        void bind(int index, const void *data, int size); // BLOB
        void bind_null(int index);

        // Advance. true = a row is ready, false = statement is done.
        bool step();

        // Read columns from the current row (0-based index).
        int64_t column_int64(int col);
        std::string column_text(int col);

        // Reset so the statement can be re-executed (clears bindings too).
        void reset();

        sqlite3_stmt *raw() { return stmt_; }

    private:
        Db &db_;
        sqlite3_stmt *stmt_ = nullptr;
    };

    // RAII transaction. BEGIN in ctor. commit() runs COMMIT. If destroyed
    // without a successful commit(), runs ROLLBACK (best-effort, never throws
    // from the destructor).
    class Tx {
    public:
        explicit Tx(Db &db);
        ~Tx();

        Tx(const Tx &) = delete;
        Tx &operator=(const Tx &) = delete;

        void commit();

    private:
        Db &db_;
        bool active_ = true;
    };

private:
    void apply_pragmas();
    void ensure_schema();
    void repair_fts_schema(); // recreate entries_fts if it has a stale column set

    sqlite3 *db_ = nullptr;
};
