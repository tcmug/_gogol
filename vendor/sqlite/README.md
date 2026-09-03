# vendored SQLite amalgamation

`sqlite3.c` / `sqlite3.h` are the **official SQLite amalgamation**, copied verbatim
from sqlite.org. SQLite is developed in Fossil (not Git) and the amalgamation is a
released download artifact, so there is no official Git repo to use as a submodule —
vendoring the amalgamation as source is the method SQLite itself recommends
(https://sqlite.org/amalgamation.html, https://sqlite.org/howtocompile.html).

- Version: **3.46.0** (`SQLITE_VERSION` in sqlite3.h)
- Source: https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip
- Downloaded zip SHA3-256: `1221eed70de626871912bfca144c00411f0c30d3c2b7935cff3963b63370ef7c`
- Compiled directly into the `gogol` target (see CMakeLists.txt) with
  `SQLITE_ENABLE_FTS5` + `SQLITE_ENABLE_MATH_FUNCTIONS`. Zero external/runtime deps.

## Updating
1. Download the new amalgamation zip from https://www.sqlite.org/download.html
2. Verify its SHA3-256 against the value published on that page.
3. Replace `sqlite3.c` / `sqlite3.h`, update the version + checksum above.

(The `sqlite-vec` vector extension IS a real Git project and is tracked as a
submodule at `vendor/sqlite-vec` → https://github.com/asg017/sqlite-vec.)
