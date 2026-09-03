// glossary_store.cpp — Per-index glossary types.
//
// The legacy .glossary FILE reader/writer was removed with the SQLite-only
// migration; glossary terms are now persisted in the index's SQLite `glossary`
// table (see storage/sqlite_backend.cpp). Kept as a compilation unit so the
// build graph and includes remain stable.
#include "storage/glossary_store.h"
