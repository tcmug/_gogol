cmake_minimum_required(VERSION 3.14)
project(gogol LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# llama.cpp options
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)

# GPU backends - auto-detect available hardware
# HIP/ROCm for AMD GPUs (Linux)
# CUDA for NVIDIA GPUs
# Metal for Apple Silicon (macOS)
option(GGML_HIP "Enable HIP/ROCm for AMD GPUs" OFF)
option(GGML_CUDA "Enable CUDA for NVIDIA GPUs" OFF)
if(APPLE)
    set(GGML_METAL ON CACHE BOOL "Enable Metal for Apple GPUs" FORCE)
endif()

# Native CPU optimizations. GGML_NATIVE is the current flag; the old LLAMA_NATIVE
# was deprecated upstream (and setting it emits a CMake warning), so we set only
# the new name. (We already set GGML_NATIVE, so this is a warning fix, not a
# behavior change.)
set(GGML_NATIVE ON CACHE BOOL "Optimize ggml for current CPU")
add_subdirectory(vendor/llama.cpp)

add_executable(gogol
    src/cli/main.cpp
    src/core/refresh.cpp
    src/core/result_formatters.cpp
    src/mcp/tool_registry.cpp
    src/mcp/mcp_server.cpp
    src/mcp/http_server.cpp
    src/mcp/framing.cpp
    src/mcp/unix_server.cpp
    src/mcp/stdio_shim.cpp
    src/mcp/daemon_lock.cpp
    src/mcp/message_bus.cpp
    src/mcp/message_store.cpp
    src/adapters/file_adapter.cpp
    src/chunking/chunker.cpp
    src/chunking/chunker_iface.cpp
    src/chunking/ts_chunker.cpp
    src/storage/index_file.cpp
    src/storage/db.cpp
    src/store/store.cpp
    src/cli/cmd_store.cpp
    src/cli/git_identity.cpp
    src/embedding/embedder.cpp
    src/embedding/embed_provider.cpp
    src/embedding/model_prefixes.cpp
    src/config/config.cpp
    src/config/scanner.cpp
    vendor/monocypher/monocypher.c
    vendor/sqlite/sqlite3.c
    vendor/sqlite-vec/sqlite-vec.c
)

target_include_directories(gogol PRIVATE
    src
    vendor/llama.cpp/include
    vendor/llama.cpp/ggml/include
    vendor/CLI11
    vendor/monocypher
    vendor/sqlite
    vendor/json
)

target_link_libraries(gogol PRIVATE llama ggml)

# Warnings: -Wall -Wextra on OUR C++ only ($<COMPILE_LANGUAGE:CXX> excludes the vendored
# C amalgamations compiled into this target — sqlite/monocypher/sqlite-vec — which have
# their own warning profile). Not -Werror: keep local builds unblocked; CI can add it.
set(GOGOL_WARN_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-Wall$<SEMICOLON>-Wextra>)
target_compile_options(gogol PRIVATE ${GOGOL_WARN_FLAGS})

# Debug logging: enabled in Debug builds, disabled in Release
target_compile_definitions(gogol PRIVATE $<$<CONFIG:Debug>:GOGOL_DEBUG>)

# SQLite (vendored amalgamation): compile options for the sqlite3.c TU
set(GOGOL_SQLITE_DEFS SQLITE_ENABLE_FTS5 SQLITE_ENABLE_MATH_FUNCTIONS)
set_source_files_properties(vendor/sqlite/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "${GOGOL_SQLITE_DEFS}"
    COMPILE_OPTIONS "-w")   # vendored amalgamation: silence its warnings (not ours to patch)

# sqlite-vec: compile statically INTO the binary (not a loadable extension).
# SQLITE_CORE makes SQLITE_EXTENSION_INIT1 a no-op; we register sqlite3_vec_init
# via sqlite3_auto_extension in Db so every connection gets the vec0 KNN table.
# The submodule ships only sqlite-vec.c; sqlite-vec.h is a build artifact generated
# from sqlite-vec.h.tmpl (version substituted). Generate it into the build dir so a
# fresh submodule checkout builds without touching the submodule.
set(SQLITE_VEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite-vec")
set(SQLITE_VEC_GEN "${CMAKE_CURRENT_BINARY_DIR}/sqlite-vec-gen")
file(MAKE_DIRECTORY "${SQLITE_VEC_GEN}")
if(EXISTS "${SQLITE_VEC_DIR}/sqlite-vec.h.tmpl")
    file(READ "${SQLITE_VEC_DIR}/sqlite-vec.h.tmpl" _svh)
    string(REPLACE "\${VERSION}" "0.1.10-alpha.4" _svh "${_svh}")
    string(REPLACE "\${DATE}" "" _svh "${_svh}")
    string(REPLACE "\${SOURCE}" "" _svh "${_svh}")
    string(REPLACE "\${VERSION_MAJOR}" "0" _svh "${_svh}")
    string(REPLACE "\${VERSION_MINOR}" "1" _svh "${_svh}")
    string(REPLACE "\${VERSION_PATCH}" "10" _svh "${_svh}")
    file(WRITE "${SQLITE_VEC_GEN}/sqlite-vec.h" "${_svh}")
endif()
set_source_files_properties(vendor/sqlite-vec/sqlite-vec.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_CORE"
    INCLUDE_DIRECTORIES "${SQLITE_VEC_GEN};${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite"
    COMPILE_OPTIONS "-w")

# SQLite needs threads (and dl on Linux); harmless on macOS
find_package(Threads REQUIRED)
target_link_libraries(gogol PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# macOS: CoreServices for FSEvents file watching
if(APPLE)
  target_link_libraries(gogol PRIVATE "-framework CoreServices")
endif()

# Tree-sitter runtime
file(GLOB TS_LIB_SRC vendor/tree-sitter/lib/src/*.c)
add_library(tree-sitter STATIC ${TS_LIB_SRC})
target_include_directories(tree-sitter PUBLIC
    vendor/tree-sitter/lib/include
    vendor/tree-sitter/lib/src
)

# Tree-sitter grammars
add_library(ts-grammars STATIC
    vendor/tree-sitter-typescript/typescript/src/parser.c
    vendor/tree-sitter-typescript/typescript/src/scanner.c
    vendor/tree-sitter-typescript/tsx/src/parser.c
    vendor/tree-sitter-typescript/tsx/src/scanner.c
    vendor/tree-sitter-python/src/parser.c
    vendor/tree-sitter-python/src/scanner.c
    vendor/tree-sitter-go/src/parser.c
    vendor/tree-sitter-rust/src/parser.c
    vendor/tree-sitter-rust/src/scanner.c
    vendor/tree-sitter-c/src/parser.c
    vendor/tree-sitter-cpp/src/parser.c
    vendor/tree-sitter-cpp/src/scanner.c
    vendor/tree-sitter-php/php/src/parser.c
    vendor/php_scanner_wrap.c
    vendor/tree-sitter-gdscript/src/parser.c
    vendor/tree-sitter-gdscript/src/scanner.c
)
target_include_directories(ts-grammars PRIVATE
    vendor/tree-sitter/lib/include
    vendor/tree-sitter/lib/src
    vendor/include
)
target_compile_options(ts-grammars PRIVATE -Wno-implicit-function-declaration)

target_include_directories(gogol PRIVATE vendor/tree-sitter/lib/include)
target_link_libraries(gogol PRIVATE tree-sitter ts-grammars)

# Tests (no llama.cpp dependency)
add_executable(gogol-tests
    tests/test_all.cpp
    src/storage/index_file.cpp
    src/storage/db.cpp
    src/store/store.cpp
    vendor/monocypher/monocypher.c
    src/config/config.cpp
    src/config/scanner.cpp
    src/chunking/chunker.cpp
    src/mcp/tool_registry.cpp
    src/mcp/mcp_server.cpp
    src/mcp/message_bus.cpp
    src/mcp/message_store.cpp
    src/mcp/http_server.cpp
    src/mcp/framing.cpp
    src/mcp/unix_server.cpp
    src/mcp/stdio_shim.cpp
    src/mcp/daemon_lock.cpp
    # refresh (on-demand indexing) + its scan/chunk deps. These only depend on the
    # abstract EmbedProvider interface (header-only), NOT llama, so the test can drive
    # the real ingest pipeline with a fake embedder — no model/llama link needed.
    src/core/refresh.cpp
    src/core/result_formatters.cpp
    src/adapters/file_adapter.cpp
    src/chunking/chunker_iface.cpp
    src/chunking/ts_chunker.cpp
    src/cli/git_identity.cpp
    src/embedding/embed_provider.cpp
    src/embedding/embedder.cpp
    src/embedding/model_prefixes.cpp  # pure arch->prefix table (no llama) — for the unit test
    vendor/sqlite/sqlite3.c
    vendor/sqlite-vec/sqlite-vec.c
)
target_include_directories(gogol-tests PRIVATE src vendor/sqlite vendor/json vendor/monocypher vendor/tree-sitter/lib/include vendor/llama.cpp/include vendor/llama.cpp/ggml/include tests)
target_compile_features(gogol-tests PRIVATE cxx_std_17)
target_compile_options(gogol-tests PRIVATE ${GOGOL_WARN_FLAGS})
target_link_libraries(gogol-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS} tree-sitter ts-grammars llama ggml)

# Tree-sitter chunker tests
add_executable(gogol-ts-tests
    tests/test_ts_chunker.cpp
    src/chunking/ts_chunker.cpp
    src/chunking/chunker.cpp
    src/chunking/chunker_iface.cpp
    src/config/config.cpp
)
target_include_directories(gogol-ts-tests PRIVATE src vendor/tree-sitter/lib/include)
target_link_libraries(gogol-ts-tests PRIVATE tree-sitter ts-grammars)
target_compile_features(gogol-ts-tests PRIVATE cxx_std_17)
target_compile_options(gogol-ts-tests PRIVATE ${GOGOL_WARN_FLAGS})

# sg (semantic grep prototype) was RETIRED 2026-09-24: its git-identity model was
# promoted into gogol core (sg_git.cpp, linked in the gogol + gogol-tests targets),
# and the mined ideas (BM25 backend, backend registry, batched cache prescan) are
# documented in docs/NEXT_SESSION_PLAN.md. The resolve_git_identity coverage moved
# into tests/test_all.cpp (test_resolve_git_identity_*).

# --- CTest suite ------------------------------------------------------------
# `ctest` (or `cmake --build build --target test`) runs the whole suite: the two
# C++ unit executables PLUS the scripted CLI/MCP verb test. The scripted test is
# a first-class suite member (not a manual step) per the project rule "no test =>
# it's broken"; it self-skips (exit 0) if gtimeout or the model are unavailable.
enable_testing()

add_test(NAME unit         COMMAND gogol-tests)
add_test(NAME ts_chunker   COMMAND gogol-ts-tests)

# CLI + MCP verb coverage: isolated (GOGOL_HOME/HOME), bounded with gtimeout.
# Passes the freshly-built binary path so it never depends on PATH or a live install.
add_test(NAME cli_verbs
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_verbs_test.sh)
set_tests_properties(cli_verbs PROPERTIES
    ENVIRONMENT "GOGOL_BIN=$<TARGET_FILE:gogol>"
    TIMEOUT 300)

add_test(NAME http_serve
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_serve_test.sh)
set_tests_properties(http_serve PROPERTIES
    ENVIRONMENT "GOGOL_BIN=$<TARGET_FILE:gogol>"
    TIMEOUT 180)
