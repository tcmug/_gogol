cmake_minimum_required(VERSION 3.14)
project(gogol LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# llama.cpp options
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/llama.cpp)

add_executable(gogol
    src/cli/main.cpp
    src/cli/cmd_query.cpp
    src/cli/cmd_calls.cpp
    src/cli/cmd_crud.cpp
    src/cli/cmd_explore.cpp
    src/core/indexer.cpp
    src/core/searcher.cpp
    src/core/loc.cpp
    src/core/format.cpp
    src/core/operations.cpp
    src/core/call_graph_query.cpp
    src/core/version.cpp
    src/mcp/tool_registry.cpp
    src/mcp/mcp_server.cpp
    src/adapters/file_adapter.cpp
    src/adapters/mem_adapter.cpp
    src/chunking/chunker.cpp
    src/chunking/chunker_iface.cpp
    src/chunking/ref_extractor.cpp
    src/chunking/ts_chunker.cpp
    src/storage/index_file.cpp
    src/storage/mem_store.cpp
    src/storage/glossary_store.cpp
    src/storage/call_store.cpp
    src/storage/import_store.cpp
    src/storage/docref_store.cpp
    src/storage/export_store.cpp
    src/storage/type_store.cpp
    src/storage/metrics_store.cpp
    src/storage/storage_backend.cpp
    src/storage/db.cpp
    src/storage/sqlite_backend.cpp
    src/embedding/embedder.cpp
    src/embedding/embed_dispatcher.cpp
    src/daemon/embed_client.cpp
    src/daemon/embed_server.cpp
    src/daemon/rpc_v2.cpp
    src/daemon/file_watcher.cpp
    src/config/config.cpp
    src/config/scanner.cpp
    vendor/monocypher/monocypher.c
    vendor/sqlite/sqlite3.c
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

# Build version from git (changes only on new commits, not every rebuild)
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GOGOL_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GOGOL_GIT_VERSION)
    set(GOGOL_GIT_VERSION "unknown")
endif()
set_source_files_properties(src/core/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "GOGOL_GIT_VERSION=\"${GOGOL_GIT_VERSION}\"")

# Debug logging: enabled in Debug builds, disabled in Release
target_compile_definitions(gogol PRIVATE $<$<CONFIG:Debug>:GOGOL_DEBUG>)

# SQLite (vendored amalgamation): compile options for the sqlite3.c TU
set(GOGOL_SQLITE_DEFS SQLITE_ENABLE_FTS5 SQLITE_ENABLE_MATH_FUNCTIONS)
set_source_files_properties(vendor/sqlite/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "${GOGOL_SQLITE_DEFS}")

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
    src/storage/call_store.cpp
    src/storage/import_store.cpp
    src/storage/docref_store.cpp
    src/storage/export_store.cpp
    src/storage/type_store.cpp
    src/storage/metrics_store.cpp
    src/storage/storage_backend.cpp
    src/storage/db.cpp
    src/storage/sqlite_backend.cpp
    src/config/config.cpp
    src/config/scanner.cpp
    src/chunking/chunker.cpp
    src/chunking/ref_extractor.cpp
    src/storage/mem_store.cpp
    src/storage/glossary_store.cpp
    src/core/loc.cpp
    src/mcp/tool_registry.cpp
    vendor/sqlite/sqlite3.c
)
target_include_directories(gogol-tests PRIVATE src vendor/sqlite vendor/json tests)
target_compile_features(gogol-tests PRIVATE cxx_std_17)
target_link_libraries(gogol-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Tree-sitter chunker tests
add_executable(gogol-ts-tests
    tests/test_ts_chunker.cpp
    src/chunking/ts_chunker.cpp
    src/chunking/chunker.cpp
)
target_include_directories(gogol-ts-tests PRIVATE src vendor/tree-sitter/lib/include)
target_link_libraries(gogol-ts-tests PRIVATE tree-sitter ts-grammars)
target_compile_features(gogol-ts-tests PRIVATE cxx_std_17)
