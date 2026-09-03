// mem_store.cpp — Per-index memory-note types.
//
// The legacy .mem FILE reader/writer was removed with the SQLite-only
// migration; mem notes are now persisted in the index's SQLite `notes` table
// (see storage/sqlite_backend.cpp). This TU intentionally holds no file I/O —
// the MemEntry type lives in the header. Kept as a compilation unit so the
// build graph and includes remain stable.
#include "storage/mem_store.h"
