#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <stdexcept>

#include "chunking/chunker.h"
#include "chunking/ref_extractor.h"
#include "config/scanner.h"
#include "config/utils.h"
#include "core/loc.h"
#include "storage/glossary_store.h"
#include "storage/index_file.h"
#include "storage/atomic_io.h"
#include "daemon/write_queue.h"
#include "storage/mem_store.h"
#include "storage/call_store.h"
#include "storage/import_store.h"
#include "storage/docref_store.h"
#include "storage/export_store.h"
#include "storage/type_store.h"
#include "storage/metrics_store.h"
#include "storage/docref_store.h"
#include "storage/storage_backend.h"
#include "storage/sqlite_backend.h"
#include "storage/db.h"
#include "core/types.h"
#include "mcp/json.h"
#include "mcp/tool_registry.h"
#include "config/config.h"
#include <fnmatch.h>
#include <unistd.h>
#include <sqlite3.h>

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() \
    do { tests_passed++; printf("ok\n"); } while(0)

// --- Scanner tests ---

void test_scanner_finds_md_files() {
    TEST(scanner_finds_md_files);
    fs::path tmp = fs::temp_directory_path() / "gogol-test-scan";
    fs::create_directories(tmp / "sub");
    std::ofstream(tmp / "a.md") << "# Hello";
    std::ofstream(tmp / "sub" / "b.md") << "# World";
    std::ofstream(tmp / "c.txt") << "not indexed";

    auto files = scan_md_files(tmp, {".md"});
    assert(files.size() == 2);

    fs::remove_all(tmp);
    PASS();
}

void test_scanner_skips_git() {
    TEST(scanner_skips_git);
    fs::path tmp = fs::temp_directory_path() / "gogol-test-git";
    fs::create_directories(tmp / ".git" / "objects");
    std::ofstream(tmp / "a.md") << "visible";
    std::ofstream(tmp / ".git" / "HEAD") << "ref: refs/heads/main";

    auto files = scan_md_files(tmp, {".md"});
    assert(files.size() == 1);
    assert(files[0].first == "a.md");

    fs::remove_all(tmp);
    PASS();
}

void test_scanner_multiple_extensions() {
    TEST(scanner_multiple_extensions);
    fs::path tmp = fs::temp_directory_path() / "gogol-test-ext";
    fs::create_directories(tmp);
    std::ofstream(tmp / "a.md") << "md";
    std::ofstream(tmp / "b.txt") << "txt";
    std::ofstream(tmp / "c.rs") << "rust";

    auto files = scan_md_files(tmp, {".md", ".txt"});
    assert(files.size() == 2);

    fs::remove_all(tmp);
    PASS();
}

void test_scanner_indexes_hidden_non_git_dirs() {
    TEST(scanner_indexes_hidden_non_git_dirs);
    fs::path tmp = fs::temp_directory_path() / "gogol-test-hidden";
    fs::create_directories(tmp / ".kiro");
    std::ofstream(tmp / "a.md") << "visible";
    std::ofstream(tmp / ".kiro" / "b.md") << "also visible";

    auto files = scan_md_files(tmp, {".md"});
    assert(files.size() == 2);

    fs::remove_all(tmp);
    PASS();
}

// --- Chunker tests ---

void test_chunk_markdown_splits_on_headings() {
    TEST(chunk_markdown_splits_on_headings);
    std::string text =
        "# Introduction\nSome intro text\nMore lines here\nAnd more\nFive lines\n"
        "## Setup\nInstall stuff\nDo things\nMore setup\nFinal step\n";

    auto chunks = chunk_markdown(text);
    assert(chunks.size() == 2);
    assert(chunks[0].heading == "Introduction");
    assert(chunks[0].line == 1);
    assert(chunks[1].heading == "Setup");
    assert(chunks[1].line == 6);
    PASS();
}

void test_chunk_markdown_merges_tiny_sections() {
    TEST(chunk_markdown_merges_tiny_sections);
    std::string text =
        "# Big section\nLine1\nLine2\nLine3\nLine4\nLine5\n"
        "## Tiny\nOne\n";

    auto chunks = chunk_markdown(text);
    assert(chunks.size() == 1);
    PASS();
}

void test_chunk_window_basic() {
    TEST(chunk_window_basic);
    std::string text(5000, 'a');
    auto chunks = chunk_window(text, 2000, 200);
    assert(chunks.size() >= 3);
    assert(chunks[0].line == 1);
    assert(chunks[0].content.size() == 2000);
    PASS();
}

// --- Location / entry-type tests ---

void test_parse_entry_type() {
    TEST(parse_entry_type);
    EntryType t;
    assert(parse_entry_type("doc", t) && t == EntryType::DOC);
    assert(parse_entry_type("note", t) && t == EntryType::NOTE);
    assert(parse_entry_type("term", t) && t == EntryType::TERM);
    assert(!parse_entry_type("bogus", t));
    assert(!parse_entry_type("", t));
    PASS();
}

void test_entry_type_str() {
    TEST(entry_type_str);
    assert(std::string(entry_type_str(EntryType::DOC)) == "doc");
    assert(std::string(entry_type_str(EntryType::NOTE)) == "note");
    assert(std::string(entry_type_str(EntryType::TERM)) == "term");
    PASS();
}

void test_split_path_line() {
    TEST(split_path_line);
    std::string path; uint32_t line;
    split_path_line("src/utils.ts:42", path, line);
    assert(path == "src/utils.ts");
    assert(line == 42);
    split_path_line("src/utils.ts", path, line);
    assert(path == "src/utils.ts");
    assert(line == 0);
    // Trailing non-numeric after colon is part of the path.
    split_path_line("a/b:notanumber", path, line);
    assert(path == "a/b:notanumber");
    assert(line == 0);
    PASS();
}

void test_format_loc_note() {
    TEST(format_loc_note);
    auto s = format_loc(EntryType::NOTE, "web", "auth/flows");
    assert(s == "note web auth/flows");
    PASS();
}

void test_format_loc_doc() {
    TEST(format_loc_doc);
    auto s = format_loc(EntryType::DOC, "web", "src/utils.ts", 42, "parseConfig");
    assert(s == "doc web src/utils.ts:42 \xC2\xA7 parseConfig");
    PASS();
}

void test_format_loc_doc_no_chunk() {
    TEST(format_loc_doc_no_chunk);
    auto s = format_loc(EntryType::DOC, "backend", "README.md", 1);
    assert(s == "doc backend README.md:1");
    PASS();
}

void test_format_loc_doc_no_line() {
    TEST(format_loc_doc_no_line);
    auto s = format_loc(EntryType::DOC, "backend", "README.md");
    assert(s == "doc backend README.md");
    PASS();
}

void test_format_loc_term() {
    TEST(format_loc_term);
    auto s = format_loc(EntryType::TERM, "team", "OMS");
    assert(s == "term team OMS");
    PASS();
}

// --- Index file tests ---

// --- Cosine similarity tests ---

void test_cosine_similarity_identical() {
    TEST(cosine_similarity_identical);
    std::vector<float> a = {1.0f, 0.0f, 0.0f};
    assert(cosine_similarity(a, a) > 0.99f);
    PASS();
}

void test_cosine_similarity_orthogonal() {
    TEST(cosine_similarity_orthogonal);
    std::vector<float> a = {1.0f, 0.0f, 0.0f};
    std::vector<float> b = {0.0f, 1.0f, 0.0f};
    float sim = cosine_similarity(a, b);
    assert(sim < 0.01f && sim > -0.01f);
    PASS();
}

void test_cosine_similarity_opposite() {
    TEST(cosine_similarity_opposite);
    std::vector<float> a = {1.0f, 0.0f, 0.0f};
    std::vector<float> b = {-1.0f, 0.0f, 0.0f};
    assert(cosine_similarity(a, b) < -0.99f);
    PASS();
}

// --- Utils tests ---

void test_split_csv() {
    TEST(split_csv);
    auto r = split_csv("web, backend ,team");
    assert(r.size() == 3);
    assert(r[0] == "web");
    assert(r[1] == "backend");
    assert(r[2] == "team");
    PASS();
}

void test_split_csv_empty() {
    TEST(split_csv_empty);
    auto r = split_csv("");
    assert(r.empty());
    PASS();
}

void test_count_entries() {
    TEST(count_entries);
    Index idx;
    IndexEntry e1; e1.proto = EntryType::DOC; e1.embedding = {1.0f};
    IndexEntry e2; e2.proto = EntryType::NOTE; e2.embedding = {1.0f};
    IndexEntry e3; e3.proto = EntryType::DOC; e3.embedding = {1.0f};
    idx.entries = {e1, e2, e3};

    auto c = count_entries(idx);
    assert(c.file == 2);
    assert(c.mem == 1);
    PASS();
}

// --- f16 conversion tests ---

void test_f16_roundtrip_values() {
    TEST(f16_roundtrip_values);
    // Typical embedding values in [-1, 1]
    float vals[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.123f, -0.789f, 0.001f};
    for (float v : vals) {
        uint16_t h = float_to_f16(v);
        float back = f16_to_float(h);
        float err = std::abs(v - back);
        assert(err < 0.001f); // f16 has ~0.001 precision in this range
    }
    PASS();
}

void test_f16_zero() {
    TEST(f16_zero);
    uint16_t h = float_to_f16(0.0f);
    float back = f16_to_float(h);
    assert(back == 0.0f);
    PASS();
}

// --- Split index format tests ---

void test_end_line_in_chunk() {
    TEST(end_line_in_chunk);
    // Chunk struct should carry end_line
    Chunk c{"myFunc()", 10, 25, "function body"};
    assert(c.line == 10);
    assert(c.end_line == 25);
    assert(c.heading == "myFunc()");
    PASS();
}

// --- Import store tests ---

void test_import_store_queries() {
    TEST(import_store_queries);
    ImportGraph graph;
    graph.imports["src/cart.ts"] = {
        {"./utils", {"formatPrice"}},
        {"./config", {}},
    };
    graph.imports["src/order.ts"] = {
        {"./cart", {"getCart", "formatPrice"}},
    };

    // imports_of returns module paths
    auto imports = graph.imports_of("src/cart.ts");
    assert(imports.size() == 2);

    // imported_by: who imports "./cart"?
    auto importers = graph.imported_by("./cart");
    assert(importers.size() == 1);
    assert(importers[0] == "src/order.ts");

    // files_importing_symbol
    auto sym_importers = graph.files_importing_symbol("formatPrice");
    assert(sym_importers.size() == 2); // both cart.ts and order.ts import it
    PASS();
}

// --- Docref store tests ---

void test_docref_store_queries() {
    TEST(docref_store_queries);
    DocRefGraph graph;
    graph.refs["docs/a.md"] = {
        {RefKind::Local, "docs/target.md", "T"},
        {RefKind::External, "https://ext.com", "E"},
    };
    graph.refs["docs/b.md"] = {
        {RefKind::Local, "docs/target.md", "T2"},
    };
    graph.refs["docs/c.md"] = {
        {RefKind::External, "docs/target.md", "not a local ref"},
    };

    // references_of returns all kinds
    auto refs = graph.references_of("docs/a.md");
    assert(refs.size() == 2);

    // referenced_by: who locally references docs/target.md? a.md and b.md, NOT c.md (external)
    auto reverse = graph.referenced_by("docs/target.md");
    assert(reverse.size() == 2);
    // map iteration is sorted by key
    assert(reverse[0] == "docs/a.md");
    assert(reverse[1] == "docs/b.md");

    // external edges excluded even though target string matches
    for (auto &src : reverse) assert(src != "docs/c.md");
    PASS();
}

// --- Export store tests ---

// --- Type store tests ---

void test_type_store_queries() {
    TEST(type_store_queries);
    TypeGraph graph;
    graph.edges.push_back(StoredTypeEdge{"OrderBase", "Entity", "extends", "src/order.ts", 10});
    graph.edges.push_back(StoredTypeEdge{"OrderDetail", "OrderBase", "extends", "src/order.ts", 50});
    graph.edges.push_back(StoredTypeEdge{"CartItem", "Serializable", "implements", "src/cart.ts", 5});

    auto parents = graph.parents_of("OrderBase");
    assert(parents.size() == 1);
    assert(parents[0].parent == "Entity");

    auto children = graph.children_of("OrderBase");
    assert(children.size() == 1);
    assert(children[0].name == "OrderDetail");

    auto no_children = graph.children_of("OrderDetail");
    assert(no_children.empty());
    PASS();
}

// --- Metrics store tests ---

void test_metrics_store_for_file() {
    TEST(metrics_store_for_file);
    MetricsStore store;
    FunctionMetrics m1; m1.name = "a"; m1.file = "x.ts"; m1.line = 1; m1.complexity = 1; m1.lines = 5; m1.params = 0; m1.returns = 1; m1.max_depth = 1;
    FunctionMetrics m2; m2.name = "b"; m2.file = "x.ts"; m2.line = 10; m2.complexity = 3; m2.lines = 20; m2.params = 2; m2.returns = 1; m2.max_depth = 3;
    FunctionMetrics m3; m3.name = "c"; m3.file = "y.ts"; m3.line = 1; m3.complexity = 1; m3.lines = 3; m3.params = 0; m3.returns = 1; m3.max_depth = 1;
    store.entries = {m1, m2, m3};

    auto results = store.for_file("x.ts");
    assert(results.size() == 2);
    assert(results[0].name == "a");
    assert(results[1].name == "b");
    assert(store.for_file("z.ts").empty());
    PASS();
}

// --- Call store tests ---

void test_call_store_queries() {
    TEST(call_store_queries);
    CallGraph graph;
    graph.edges.push_back(StoredCallEdge{"main", "helper", "src/app.ts", 5});
    graph.edges.push_back(StoredCallEdge{"main", "util", "src/app.ts", 6});
    graph.edges.push_back(StoredCallEdge{"handler", "main", "src/handler.ts", 3});

    auto callees = graph.callees_of("main");
    assert(callees.size() == 2);
    assert(callees[0].callee == "helper");
    assert(callees[1].callee == "util");

    auto callers = graph.callers_of("main");
    assert(callers.size() == 1);
    assert(callers[0].caller == "handler");

    assert(graph.callees_of("nonexistent").empty());
    assert(graph.callers_of("nonexistent").empty());
    PASS();
}

// --- Scope matching tests ---

// Replicate the scope matching logic from searcher.cpp
static bool scope_matches(const std::string &scope, EntryType proto, const std::string &idx, const std::string &path) {
    if (scope.empty()) return true;
    std::string s = scope;
    bool file_only = false, mem_only = false;
    if (s[0] == '!') { file_only = true; s = s.substr(1); }
    else if (s[0] == '@') { mem_only = true; s = s.substr(1); }
    bool is_mem = (proto == EntryType::NOTE);
    if (file_only && is_mem) return false;
    if (mem_only && !is_mem) return false;
    std::string loc = idx + ":" + path;
    return fnmatch(s.c_str(), loc.c_str(), 0) == 0;
}

void test_scope_bare_matches_all() {
    TEST(scope_bare_matches_all);
    assert(scope_matches("backend:*", EntryType::DOC, "backend", "src/app.ts"));
    assert(scope_matches("backend:*", EntryType::NOTE, "backend", "deploy/notes"));
    assert(!scope_matches("backend:*", EntryType::DOC, "web", "src/app.ts"));
    PASS();
}

void test_scope_bang_files_only() {
    TEST(scope_bang_files_only);
    assert(scope_matches("!backend:*", EntryType::DOC, "backend", "src/app.ts"));
    assert(!scope_matches("!backend:*", EntryType::NOTE, "backend", "deploy/notes"));
    PASS();
}

void test_scope_at_mem_only() {
    TEST(scope_at_mem_only);
    assert(!scope_matches("@backend:*", EntryType::DOC, "backend", "src/app.ts"));
    assert(scope_matches("@backend:*", EntryType::NOTE, "backend", "deploy/notes"));
    PASS();
}

void test_scope_glob_pattern() {
    TEST(scope_glob_pattern);
    assert(scope_matches("backend:**/*.ts", EntryType::DOC, "backend", "src/deep/app.ts"));
    assert(!scope_matches("backend:**/*.ts", EntryType::DOC, "backend", "src/app.md"));
    assert(scope_matches("!backend:**/*.test.*", EntryType::DOC, "backend", "src/app.test.ts"));
    assert(!scope_matches("!backend:**/*.test.*", EntryType::NOTE, "backend", "notes.test.md"));
    PASS();
}

void test_scope_empty_matches_all() {
    TEST(scope_empty_matches_all);
    assert(scope_matches("", EntryType::DOC, "backend", "src/app.ts"));
    assert(scope_matches("", EntryType::NOTE, "backend", "topic"));
    PASS();
}

// --- atomic_write hardening (Option C: non-throwing IO) ---

void test_atomic_write_success() {
    TEST(atomic_write_success);
    fs::path tmp = fs::temp_directory_path() / "gogol-test-atomic" / "out.bin";
    fs::remove_all(tmp.parent_path());
    bool ok = atomic_write(tmp, [](std::ofstream &f) { f << "hello"; });
    assert(ok);
    assert(fs::exists(tmp));
    fs::remove_all(tmp.parent_path());
    PASS();
}

void test_atomic_write_returns_false_on_bad_path() {
    TEST(atomic_write_returns_false_on_bad_path);
    // Parent path collides with an existing regular file, so create_directories
    // fails. Must return false, not throw (which would abort the daemon).
    fs::path base = fs::temp_directory_path() / "gogol-test-atomic-bad";
    fs::remove_all(base);
    std::ofstream(base) << "i am a file, not a dir";
    fs::path target = base / "sub" / "out.bin"; // base is a file -> cannot mkdir
    bool ok = atomic_write(target, [](std::ofstream &f) { f << "data"; });
    assert(!ok);
    fs::remove_all(base);
    PASS();
}

// --- WriteQueue (single-writer serialization) ---

void test_write_queue_runs_jobs_in_order() {
    TEST(write_queue_runs_jobs_in_order);
    std::vector<int> order;
    {
        WriteQueue wq;
        for (int i = 0; i < 100; i++)
            wq.submit([&order, i]() { order.push_back(i); });
        // Destructor drains all queued jobs before returning.
    }
    assert(order.size() == 100);
    for (int i = 0; i < 100; i++) assert(order[i] == i);
    PASS();
}

void test_write_queue_submit_wait_returns_result() {
    TEST(write_queue_submit_wait_returns_result);
    WriteQueue wq;
    int r = wq.submit_wait<int>([]() { return 42; });
    assert(r == 42);
    std::string s = wq.submit_wait<std::string>([]() { return std::string("ok"); });
    assert(s == "ok");
    PASS();
}

void test_write_queue_submit_wait_propagates_exception() {
    TEST(write_queue_submit_wait_propagates_exception);
    WriteQueue wq;
    bool threw = false;
    try {
        wq.submit_wait<int>([]() -> int { throw std::runtime_error("boom"); });
    } catch (const std::runtime_error& e) {
        threw = (std::string(e.what()) == "boom");
    }
    assert(threw);
    // Queue still usable after a job threw.
    assert(wq.submit_wait<int>([]() { return 7; }) == 7);
    PASS();
}

void test_write_queue_serializes_concurrent_submits() {
    TEST(write_queue_serializes_concurrent_submits);
    // If jobs ran concurrently, this non-atomic counter would lose increments.
    // Single-writer execution means every increment is safe.
    long counter = 0;
    const int THREADS = 8, PER = 500;
    {
        WriteQueue wq;
        std::vector<std::thread> producers;
        for (int t = 0; t < THREADS; t++) {
            producers.emplace_back([&wq, &counter]() {
                for (int i = 0; i < PER; i++)
                    wq.submit([&counter]() { counter++; }); // non-atomic on purpose
            });
        }
        for (auto& p : producers) p.join();
        // Destructor drains remaining jobs.
    }
    assert(counter == (long)THREADS * PER);
    PASS();
}

void test_write_queue_job_exception_does_not_kill_worker() {
    TEST(write_queue_job_exception_does_not_kill_worker);
    WriteQueue wq;
    // A fire-and-forget job that throws must not take down the worker thread;
    // subsequent jobs must still run.
    wq.submit([]() { throw std::runtime_error("ignored"); });
    int r = wq.submit_wait<int>([]() { return 99; });
    assert(r == 99);
    PASS();
}

// --- IndexConfig::memory_dir ---

void test_memory_dir_default() {
    TEST(memory_dir_default);
    IndexConfig cfg;
    cfg.name = "backend";
    // No explicit memory -> default under ~/.gogol/memory/<name>
    std::string dir = cfg.memory_dir();
    std::string expected = std::string(std::getenv("HOME")) + "/.gogol/memory/backend";
    assert(dir == expected);
    PASS();
}

void test_memory_dir_explicit() {
    TEST(memory_dir_explicit);
    IndexConfig cfg;
    cfg.name = "backend";
    cfg.memory = "/custom/backend-notes";
    assert(cfg.memory_dir() == "/custom/backend-notes");
    PASS();
}

// --- Ref extractor tests ---

void test_ref_extractor_single_link() {
    TEST(ref_extractor_single_link);
    MarkdownLinkExtractor ext;
    auto refs = ext.extract("See [Orders](./domains/ORDERS.md) for details.");
    assert(refs.size() == 1);
    assert(refs[0].text == "Orders");
    assert(refs[0].target == "./domains/ORDERS.md");
    PASS();
}

void test_ref_extractor_multiple_links_per_line() {
    TEST(ref_extractor_multiple_links_per_line);
    MarkdownLinkExtractor ext;
    auto refs = ext.extract("[A](a.md) and [B](b.md) and [C](c.md)");
    assert(refs.size() == 3);
    assert(refs[0].text == "A" && refs[0].target == "a.md");
    assert(refs[1].text == "B" && refs[1].target == "b.md");
    assert(refs[2].text == "C" && refs[2].target == "c.md");
    PASS();
}

void test_ref_extractor_skips_images() {
    TEST(ref_extractor_skips_images);
    MarkdownLinkExtractor ext;
    auto refs = ext.extract("![diagram](img.png) but [real](real.md)");
    assert(refs.size() == 1);
    assert(refs[0].text == "real");
    assert(refs[0].target == "real.md");
    PASS();
}

void test_ref_extractor_skips_code_fence() {
    TEST(ref_extractor_skips_code_fence);
    MarkdownLinkExtractor ext;
    std::string content =
        "[before](before.md)\n"
        "```\n"
        "[inside](inside.md)\n"
        "```\n"
        "[after](after.md)\n";
    auto refs = ext.extract(content);
    assert(refs.size() == 2);
    assert(refs[0].target == "before.md");
    assert(refs[1].target == "after.md");
    PASS();
}

void test_ref_extractor_anchor_and_url_not_filtered() {
    TEST(ref_extractor_anchor_and_url_not_filtered);
    MarkdownLinkExtractor ext;
    auto refs = ext.extract(
        "[anchor](#section) and [url](https://example.com) and [file](x.md)");
    assert(refs.size() == 3);
    assert(refs[0].target == "#section");
    assert(refs[1].target == "https://example.com");
    assert(refs[2].target == "x.md");
    PASS();
}

void test_ref_extractor_trims_target_whitespace() {
    TEST(ref_extractor_trims_target_whitespace);
    MarkdownLinkExtractor ext;
    auto refs = ext.extract("[t](  ./spaced.md  )");
    assert(refs.size() == 1);
    assert(refs[0].target == "./spaced.md");
    PASS();
}

void test_ref_extractor_supports() {
    TEST(ref_extractor_supports);
    MarkdownLinkExtractor ext;
    assert(ext.supports(".md"));
    assert(ext.supports(".mdx"));
    assert(!ext.supports(".ts"));
    auto exts = default_ref_extractors();
    assert(exts.size() >= 1);
    PASS();
}

// --- Doc reference resolution tests ---

void test_resolve_doc_ref() {
    TEST(resolve_doc_ref);
    const std::string source = "domains/ORDERS.md";
    std::set<std::string> valid = {
        "domains/REAL_TIME_INVENTORY.md",
        "DATA_MODELS.md",
    };
    DocRefEdge out;

    // "./REAL_TIME_INVENTORY.md" -> Local, "domains/REAL_TIME_INVENTORY.md"
    assert(resolve_doc_ref(source, "./REAL_TIME_INVENTORY.md", "RTI", valid, out));
    assert(out.kind == RefKind::Local);
    assert(out.target == "domains/REAL_TIME_INVENTORY.md");
    assert(out.text == "RTI");

    // "../DATA_MODELS.md" -> Local, "DATA_MODELS.md"
    out = DocRefEdge{};
    assert(resolve_doc_ref(source, "../DATA_MODELS.md", "DM", valid, out));
    assert(out.kind == RefKind::Local);
    assert(out.target == "DATA_MODELS.md");

    // "https://jira/x" -> External, target kept as-is
    out = DocRefEdge{};
    assert(resolve_doc_ref(source, "https://jira/x", "ticket", valid, out));
    assert(out.kind == RefKind::External);
    assert(out.target == "https://jira/x");
    assert(out.text == "ticket");

    // "#section" -> dropped (pure anchor)
    out = DocRefEdge{};
    assert(!resolve_doc_ref(source, "#section", "sec", valid, out));

    // "./nope.md" -> dropped (not in valid set)
    out = DocRefEdge{};
    assert(!resolve_doc_ref(source, "./nope.md", "nope", valid, out));

    // "./REAL_TIME_INVENTORY.md#data-model" -> Local, anchor stripped
    out = DocRefEdge{};
    assert(resolve_doc_ref(source, "./REAL_TIME_INVENTORY.md#data-model", "RTI", valid, out));
    assert(out.kind == RefKind::Local);
    assert(out.target == "domains/REAL_TIME_INVENTORY.md");

    PASS();
}

// --- SQLite link smoke test ---

void test_sqlite_linked() {
    TEST(sqlite_linked);
    printf("(sqlite %s) ", sqlite3_libversion());

    sqlite3* db = nullptr;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(db != nullptr);

    char* err = nullptr;
    assert(sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)",
                        nullptr, nullptr, &err) == SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO t(id, v) VALUES (1, 'hello')",
                        nullptr, nullptr, &err) == SQLITE_OK);

    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT id, v FROM t WHERE id = 1", -1,
                              &stmt, nullptr) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    assert(std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                       "hello") == 0);
    assert(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
    assert(sqlite3_close(db) == SQLITE_OK);
    PASS();
}

// --- Db wrapper tests ---

void test_db_wrapper() {
    TEST(db_wrapper);

    fs::path tmp = fs::temp_directory_path() /
                   ("gogol_db_test_" + std::to_string(::getpid()) + ".db");
    // Clean any stale artifacts (incl. WAL sidecars) from a prior run.
    fs::remove(tmp);
    fs::remove(fs::path(tmp.string() + "-wal"));
    fs::remove(fs::path(tmp.string() + "-shm"));

    // Open: applies PRAGMAs + ensures schema. user_version round-trips.
    {
        Db db(tmp.string());
        assert(db.user_version() == 0);
        db.set_user_version(7);
        assert(db.user_version() == 7);

        // Insert a note via a bound statement inside a committed transaction.
        {
            Db::Tx tx(db);
            Db::Stmt ins(db,
                "INSERT INTO notes(topic, content, timestamp, sources) "
                "VALUES(?, ?, ?, ?)");
            ins.bind(1, std::string("auth/flow"));
            ins.bind(2, std::string("OAuth redirects to /login"));
            ins.bind(3, (int64_t)1234567890);
            ins.bind(4, std::string("mem://auth/flow"));
            assert(ins.step() == false); // INSERT yields no rows
            tx.commit();
        }
    }

    // Reopen: user_version persisted, committed row present.
    {
        Db db(tmp.string());
        assert(db.user_version() == 7);

        Db::Stmt q(db, "SELECT content, timestamp FROM notes WHERE topic = ?");
        q.bind(1, std::string("auth/flow"));
        assert(q.step() == true);
        assert(q.column_text(0) == "OAuth redirects to /login");
        assert(q.column_int64(1) == 1234567890);
        assert(q.step() == false);
    }

    // Rollback: insert without commit, reopen, row must be absent.
    {
        Db db(tmp.string());
        {
            Db::Tx tx(db);
            Db::Stmt ins(db,
                "INSERT INTO notes(topic, content, timestamp, sources) "
                "VALUES(?, ?, ?, ?)");
            ins.bind(1, std::string("deploy/notes"));
            ins.bind(2, std::string("HPA scales at 70% CPU"));
            ins.bind(3, (int64_t)999);
            ins.bind(4, std::string(""));
            assert(ins.step() == false);
            // tx goes out of scope without commit() -> ROLLBACK
        }
    }
    {
        Db db(tmp.string());
        Db::Stmt q(db, "SELECT COUNT(*) FROM notes WHERE topic = ?");
        q.bind(1, std::string("deploy/notes"));
        assert(q.step() == true);
        assert(q.column_int64(0) == 0);
    }

    // Bad path -> constructor throws (error not silently swallowed).
    bool threw = false;
    try {
        Db bad("/nonexistent_dir_gogol/definitely/not/here.db");
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    // Cleanup temp files.
    fs::remove(tmp);
    fs::remove(fs::path(tmp.string() + "-wal"));
    fs::remove(fs::path(tmp.string() + "-shm"));
    PASS();
}

// --- SqliteBackend roundtrip ---

void test_sqlite_backend_roundtrip() {
    TEST(sqlite_backend_roundtrip);

    // Throwaway index name (NOT a real index). The .db lives under the real
    // indexes dir but is deleted at the end; nothing else references it.
    const std::string idx = "sqlite_be_test";
    const std::string dbp = SqliteBackend::db_path(idx);
    auto cleanup = [&]() {
        fs::remove(dbp);
        fs::remove(dbp + "-wal");
        fs::remove(dbp + "-shm");
    };
    cleanup(); // clear any stale artifacts from a prior run

    // --- Save everything through a fresh SqliteBackend ---
    {
        SqliteBackend be(idx);

        // Index with 2 entries + real embeddings (f32).
        Index ix;
        ix.dim = 4;
        ix.precision = EmbedPrecision::F32;
        IndexEntry e1;
        e1.proto = EntryType::DOC;
        e1.path = "src/auth.ts";
        e1.chunk = "handleLogin";
        e1.line = 10;
        e1.end_line = 42;
        e1.hash = 0xDEADBEEFCAFEull;
        e1.embedding = {0.1f, 0.2f, 0.3f, 0.4f};
        IndexEntry e2;
        e2.proto = EntryType::NOTE;
        e2.path = "notes/topic";
        e2.chunk = "";
        e2.line = 0;
        e2.end_line = 0;
        e2.hash = 7;
        e2.embedding = {-1.0f, 0.5f, 0.25f, -0.75f};
        ix.entries = {e1, e2};
        be.save_index("/root/path", ix);

        // Mem entry.
        std::map<std::string, MemEntry> mem;
        mem["auth"] = {"OAuth2 with PKCE", {"src/auth.ts", "mem://auth"}, 1234567};
        be.save_mem(mem);

        // Glossary term.
        std::map<std::string, std::string> glossary;
        glossary["OMS"] = "order management system";
        be.save_glossary(glossary);

        // Call graph.
        CallGraph cg;
        cg.edges.push_back({"processOrder", "hashValue", "src/orders.ts", 42});
        cg.edges.push_back({"retryOrder", "processOrder", "src/orders.ts", 90});
        be.save_call_graph(cg);

        // Docref graph (incl. kind + target + text).
        DocRefGraph dg;
        dg.refs["docs/a.md"].push_back({RefKind::Local, "docs/b.md", "See B"});
        dg.refs["docs/a.md"].push_back(
            {RefKind::External, "https://example.com", "Example"});
        be.save_docref_graph(dg);
    }

    // --- Load back through a FRESH backend on the same path ---
    {
        SqliteBackend be(idx);

        Index ix = be.load_index();
        assert(ix.dim == 4);
        assert(ix.precision == EmbedPrecision::F32);
        assert(ix.emb_loaded); // eager load (documented difference)
        assert(ix.entries.size() == 2);
        assert(ix.entries[0].proto == EntryType::DOC);
        assert(ix.entries[0].path == "src/auth.ts");
        assert(ix.entries[0].chunk == "handleLogin");
        assert(ix.entries[0].line == 10);
        assert(ix.entries[0].end_line == 42);
        assert(ix.entries[0].hash == 0xDEADBEEFCAFEull);
        assert(ix.entries[0].embedding.size() == 4);
        // f32 round-trips exactly.
        assert(ix.entries[0].embedding[0] == 0.1f);
        assert(ix.entries[0].embedding[3] == 0.4f);
        assert(ix.entries[1].proto == EntryType::NOTE);
        assert(ix.entries[1].embedding[0] == -1.0f);
        assert(ix.entries[1].embedding[3] == -0.75f);

        IndexCounts counts = be.load_index_counts();
        assert(counts.dim == 4);
        assert(counts.file_count == 1); // one DOC
        assert(counts.mem_count == 1);  // one NOTE

        std::map<std::string, MemEntry> mem;
        assert(be.load_mem(mem));
        assert(mem.size() == 1);
        assert(mem["auth"].content == "OAuth2 with PKCE");
        assert(mem["auth"].sources.size() == 2);
        assert(mem["auth"].sources[0] == "src/auth.ts");
        assert(mem["auth"].sources[1] == "mem://auth");
        assert(mem["auth"].timestamp == 1234567);

        auto glossary = be.load_glossary();
        assert(glossary.size() == 1);
        assert(glossary["OMS"] == "order management system");

        CallGraph cg = be.load_call_graph();
        assert(cg.edges.size() == 2);
        assert(cg.edges[0].caller == "processOrder");
        assert(cg.edges[0].callee == "hashValue");
        assert(cg.edges[0].file == "src/orders.ts");
        assert(cg.edges[0].line == 42);
        auto callers = cg.callers_of("processOrder");
        assert(callers.size() == 1 && callers[0].caller == "retryOrder");

        DocRefGraph dg = be.load_docref_graph();
        auto refs = dg.references_of("docs/a.md");
        assert(refs.size() == 2);
        assert(refs[0].kind == RefKind::Local);
        assert(refs[0].target == "docs/b.md");
        assert(refs[0].text == "See B");
        assert(refs[1].kind == RefKind::External);
        assert(refs[1].target == "https://example.com");
        assert(refs[1].text == "Example");
    }

    cleanup();
    PASS();
}

// --- FTS5 keyword search (SqliteBackend::keyword_search_fts) ---

void test_fts5_keyword_search() {
    TEST(fts5_keyword_search);

    const std::string idx = "__test_fts5_kw__";
    const std::string dbp = SqliteBackend::db_path(idx);
    auto cleanup = [&]() {
        fs::remove(dbp);
        fs::remove(dbp + "-wal");
        fs::remove(dbp + "-shm");
    };
    cleanup(); // clear any stale artifacts from a prior run

    // Save an Index with distinct terms in path/chunk. save_index_locked runs
    // the FTS 'rebuild' so entries_fts is populated to match entries.
    {
        SqliteBackend be(idx);
        Index ix;
        ix.dim = 0; // no embeddings needed for a keyword-only test
        ix.precision = EmbedPrecision::F32;

        IndexEntry e0; // entry_index 0
        e0.proto = EntryType::DOC;
        e0.path = "src/order-state-machine.ts";
        e0.chunk = "OrderStateMachine transition";
        e0.line = 10;
        IndexEntry e1; // entry_index 1
        e1.proto = EntryType::DOC;
        e1.path = "src/write_queue.h";
        e1.chunk = "WriteQueue submit_wait";
        e1.line = 20;
        IndexEntry e2; // entry_index 2
        e2.proto = EntryType::DOC;
        e2.path = "src/auth/login.ts";
        e2.chunk = "handleLogin session";
        e2.line = 30;
        ix.entries = {e0, e1, e2};
        be.save_index("/root", ix);
    }

    // Query for a term that appears only in entry 1's chunk.
    {
        SqliteBackend be(idx);
        auto hits = be.keyword_search_fts("WriteQueue", 5);
        assert(!hits.empty());
        // Top hit must be entry_index 1 (the WriteQueue chunk).
        assert(hits[0].first == 1);
        // Score follows the larger-is-better convention (negated FTS rank).
        for (size_t i = 1; i < hits.size(); i++)
            assert(hits[i - 1].second >= hits[i].second);
    }

    // Query for a term only in entry 0's path/chunk.
    {
        SqliteBackend be(idx);
        auto hits = be.keyword_search_fts("OrderStateMachine", 5);
        assert(!hits.empty());
        assert(hits[0].first == 0);
    }

    // Path term also matches (FTS indexes path column).
    {
        SqliteBackend be(idx);
        auto hits = be.keyword_search_fts("login", 5);
        assert(!hits.empty());
        assert(hits[0].first == 2);
    }

    // A term present in no entry yields no hits (and does not error).
    {
        SqliteBackend be(idx);
        auto hits = be.keyword_search_fts("nonexistentxyzzy", 5);
        assert(hits.empty());
    }

    // FTS5 operator chars in the query are neutralized (quoted) — no throw,
    // and the embedded term still matches.
    {
        SqliteBackend be(idx);
        auto hits = be.keyword_search_fts("OrderStateMachine OR (login", 5);
        assert(!hits.empty());
    }

    // Empty query → no results, no error.
    {
        SqliteBackend be(idx);
        assert(be.keyword_search_fts("   ", 5).empty());
    }

    cleanup();
    PASS();
}

// --- save_all / load_all whole-index roundtrip ---

void test_backend_save_all_roundtrip_sqlite() {
    TEST(backend_save_all_roundtrip_sqlite);

    const std::string idx = "saveall_sqlite_test";
    const std::string dbp = SqliteBackend::db_path(idx);
    auto cleanup = [&]() {
        fs::remove(dbp);
        fs::remove(dbp + "-wal");
        fs::remove(dbp + "-shm");
    };
    cleanup(); // clear stale artifacts from a prior run

    // Same IndexData shape as the file test.
    IndexData data;
    data.index.dim = 4;
    data.index.precision = EmbedPrecision::F32;
    {
        IndexEntry e1;
        e1.proto = EntryType::DOC;
        e1.path = "src/auth.ts";
        e1.chunk = "handleLogin";
        e1.line = 10;
        e1.end_line = 42;
        e1.hash = 0xABCDEFull;
        e1.embedding = {0.1f, 0.2f, 0.3f, 0.4f};
        IndexEntry e2;
        e2.proto = EntryType::DOC;
        e2.path = "src/orders.ts";
        e2.chunk = "processOrder";
        e2.line = 100;
        e2.end_line = 150;
        e2.hash = 0x123456ull;
        e2.embedding = {-1.0f, 0.5f, 0.25f, -0.75f};
        data.index.entries = {e1, e2};
        data.index.emb_loaded = true;
    }
    data.mem["auth"] = {"OAuth2 with PKCE", {"src/auth.ts"}, 1234567};
    data.calls.edges.push_back({"processOrder", "hashValue", "src/orders.ts", 42});
    data.docrefs.refs["docs/a.md"].push_back({RefKind::Local, "docs/b.md", "See B"});

    // Save + load through fresh SqliteBackends. save_all commits atomically.
    {
        SqliteBackend be(idx);
        be.save_all("/root/path", data);
        be.set_schema_version(5); // schema version round-trips
    }
    {
        SqliteBackend be(idx);
        IndexData loaded = be.load_all();

        assert(loaded.index.dim == 4);
        assert(loaded.index.precision == EmbedPrecision::F32);
        assert(loaded.index.emb_loaded); // eager (documented difference)
        assert(loaded.index.entries.size() == 2);
        assert(loaded.index.entries[0].path == "src/auth.ts");
        assert(loaded.index.entries[0].chunk == "handleLogin");
        assert(loaded.index.entries[0].embedding.size() == 4);
        assert(loaded.index.entries[0].embedding[0] == 0.1f);
        assert(loaded.index.entries[1].embedding[3] == -0.75f);

        assert(loaded.mem.size() == 1);
        assert(loaded.mem["auth"].content == "OAuth2 with PKCE");
        assert(loaded.mem["auth"].timestamp == 1234567);

        assert(loaded.calls.edges.size() == 1);
        assert(loaded.calls.edges[0].caller == "processOrder");
        assert(loaded.calls.edges[0].callee == "hashValue");

        auto refs = loaded.docrefs.references_of("docs/a.md");
        assert(refs.size() == 1);
        assert(refs[0].kind == RefKind::Local);
        assert(refs[0].target == "docs/b.md");
        assert(refs[0].text == "See B");

        assert(be.schema_version() == 5); // persisted user_version
    }

    cleanup();
    PASS();
}

// --- JSON seam tests ---

void test_json_roundtrip() {
    TEST(json_roundtrip);
    gogol::Json obj;
    obj["name"] = "gogol";
    obj["array"] = {1, 2, 3};
    obj["nested"] = {{"enabled", true}, {"count", 42}};

    std::string serialized = obj.dump();

    gogol::Json parsed;
    std::string err;
    assert(gogol::json_parse(serialized, parsed, &err));
    assert(err.empty());

    // Structural equality after roundtrip.
    assert(parsed == obj);

    // Field access.
    assert(gogol::json_get_string(parsed, "name") == "gogol");
    assert(gogol::json_get_string(parsed, "missing", "fallback") == "fallback");
    assert(parsed["array"].size() == 3);
    assert(parsed["array"][2].get<int>() == 3);
    assert(parsed["nested"]["enabled"].get<bool>() == true);
    assert(parsed["nested"]["count"].get<int>() == 42);

    // Invalid JSON is rejected without throwing.
    gogol::Json bad;
    std::string bad_err;
    assert(!gogol::json_parse("{ not valid", bad, &bad_err));
    assert(!bad_err.empty());

    PASS();
}

// --- MCP tool registry tests ---

void test_tool_registry() {
    TEST(tool_registry);
    const auto& reg = gogol::mcp::tool_registry();

    // Registry is non-empty.
    assert(!reg.empty());

    // The v1 catalog is exactly these read-only tools.
    std::vector<std::string> expected = {"query", "explore", "calls",
                                         "affected", "get", "list"};
    for (const auto& name : expected) {
        bool found = false;
        for (const auto& def : reg) {
            if (def.name == name) { found = true; break; }
        }
        assert(found);
    }

    for (const auto& def : reg) {
        // Every tool has a name and a description.
        assert(!def.name.empty());
        assert(!def.description.empty());
        // Every tool has a handler.
        assert(static_cast<bool>(def.handler));
        // Each param is well-formed: name + type + description.
        for (const auto& p : def.params) {
            assert(!p.name.empty());
            assert(!p.type.empty());
            assert(!p.description.empty());
        }
        // All v1 tools are read-only.
        assert(def.read_only);
    }

    // Spot-check that query has at least the required `text` param marked required.
    for (const auto& def : reg) {
        if (def.name != "query") continue;
        assert(!def.params.empty());
        bool text_required = false;
        for (const auto& p : def.params) {
            if (p.name == "text") { text_required = p.required; }
        }
        assert(text_required);
    }

    PASS();
}

void test_tools_list_schema() {
    TEST(tools_list_schema);
    gogol::Json schema = gogol::mcp::tools_list_schema(/*include_write=*/false);

    // It's a JSON array.
    assert(schema.is_array());
    // One entry per read-only tool (all v1 tools are read-only → matches registry size).
    assert(schema.size() == gogol::mcp::tool_registry().size());

    for (const auto& tool : schema) {
        // Each entry has name, description, inputSchema.
        assert(tool.contains("name") && tool["name"].is_string());
        assert(tool.contains("description") && tool["description"].is_string());
        assert(tool.contains("inputSchema") && tool["inputSchema"].is_object());

        const auto& input = tool["inputSchema"];
        assert(input["type"] == "object");
        assert(input.contains("properties") && input["properties"].is_object());
        assert(input.contains("required") && input["required"].is_array());

        // Find the matching ToolDef and assert schema matches its params.
        const gogol::mcp::ToolDef* def = nullptr;
        for (const auto& d : gogol::mcp::tool_registry()) {
            if (d.name == tool["name"].get<std::string>()) { def = &d; break; }
        }
        assert(def != nullptr);

        const auto& props = input["properties"];
        assert(props.size() == def->params.size());

        // Collect required names from the generated schema.
        std::vector<std::string> schema_required;
        for (const auto& r : input["required"]) schema_required.push_back(r.get<std::string>());

        // Every param appears as a property with type + description; required
        // list matches params flagged required.
        std::vector<std::string> def_required;
        for (const auto& p : def->params) {
            assert(props.contains(p.name));
            assert(props[p.name]["type"] == p.type);
            assert(props[p.name]["description"] == p.description);
            if (p.required) def_required.push_back(p.name);
        }
        assert(schema_required.size() == def_required.size());
        for (const auto& name : def_required) {
            bool in_schema = false;
            for (const auto& s : schema_required) if (s == name) { in_schema = true; break; }
            assert(in_schema);
        }
    }

    // Write tools are excluded when include_write=false. Since all v1 tools are
    // read-only, include_write=true yields the same count today; assert the
    // read-only-only view is never larger than the full view.
    gogol::Json full = gogol::mcp::tools_list_schema(/*include_write=*/true);
    assert(schema.size() <= full.size());

    // Every tool present in the read-only schema is read-only in the registry.
    for (const auto& tool : schema) {
        for (const auto& d : gogol::mcp::tool_registry()) {
            if (d.name == tool["name"].get<std::string>()) { assert(d.read_only); break; }
        }
    }

    PASS();
}

// --- MCP config gating ---

void test_mcp_config_parse() {
    TEST(mcp_config_parse);

    // load_global_config() reads $HOME/.gogol/config. Point HOME at a temp dir
    // so we can control the config content without touching the real one.
    const char* orig_home = std::getenv("HOME");
    std::string saved_home = orig_home ? orig_home : "";

    fs::path tmp_home = fs::temp_directory_path() /
                        ("gogol_mcp_cfg_" + std::to_string(::getpid()));
    fs::create_directories(tmp_home / ".gogol");
    fs::path cfg_path = tmp_home / ".gogol" / "config";

    auto write_cfg = [&](const std::string& body) {
        std::ofstream out(cfg_path, std::ios::trunc);
        out << body;
        out.close();
    };

    setenv("HOME", tmp_home.c_str(), /*overwrite=*/1);

    // 1) Explicit [mcp] enabled=true, tools=read-write.
    write_cfg(
        "model = /tmp/model.gguf\n"
        "\n"
        "[mcp]\n"
        "enabled = true\n"
        "tools = read-write\n"
        "\n"
        "[web]\n"
        "path = /tmp/web\n");
    {
        GlobalConfig gc = load_global_config();
        assert(gc.mcp_enabled == true);
        assert(gc.mcp_tools == "read-write");
        assert(gc.mcp_read_write() == true);
    }

    // 2) No [mcp] section → defaults: disabled, read-only.
    write_cfg(
        "model = /tmp/model.gguf\n"
        "\n"
        "[web]\n"
        "path = /tmp/web\n");
    {
        GlobalConfig gc = load_global_config();
        assert(gc.mcp_enabled == false);
        assert(gc.mcp_tools == "read");
        assert(gc.mcp_read_write() == false);
    }

    // 3) [mcp] present but enabled omitted → still disabled; tools defaults read.
    write_cfg(
        "[mcp]\n"
        "tools = read\n");
    {
        GlobalConfig gc = load_global_config();
        assert(gc.mcp_enabled == false);
        assert(gc.mcp_tools == "read");
    }

    // 4) enabled=true, tools omitted → enabled + read default.
    write_cfg(
        "[mcp]\n"
        "enabled = true\n");
    {
        GlobalConfig gc = load_global_config();
        assert(gc.mcp_enabled == true);
        assert(gc.mcp_tools == "read");
    }

    // 5) An unknown tools value falls back to the safe "read" default.
    write_cfg(
        "[mcp]\n"
        "enabled = true\n"
        "tools = bogus\n");
    {
        GlobalConfig gc = load_global_config();
        assert(gc.mcp_enabled == true);
        assert(gc.mcp_tools == "read");
    }

    // Restore HOME and clean up.
    if (!saved_home.empty())
        setenv("HOME", saved_home.c_str(), 1);
    else
        unsetenv("HOME");
    std::error_code ec;
    fs::remove_all(tmp_home, ec);

    PASS();
}


// --- MCP sessions (P4): scope + result cursors ---
//
// NOTE: this suite is built in Release with -DNDEBUG, so assert() is compiled
// out. These tests therefore use explicit runtime checks (SCHECK) that always
// evaluate, so a regression fails the run rather than silently passing. They
// exercise the daemon-free session helpers (apply_set_scope, Session cursor
// cache) shared with the MCP dispatch; the full end-to-end "query used the
// session default index" path is covered by the scripted live-daemon handshake.
#define SCHECK(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);               \
            return; /* skips PASS() → tests_passed < tests_run → exit 1 */ \
        }                                                                  \
    } while (0)

void test_mcp_session_scope() {
    TEST(mcp_session_scope);
    using namespace gogol::mcp;

    Session s;
    SCHECK(s.default_index.empty());
    SCHECK(s.default_type.empty());

    // set_scope {index:"backend"} — leaves type unchanged.
    ToolArgs set_idx;
    set_idx.obj = gogol::Json::object();
    set_idx.obj["index"] = "backend";
    gogol::Json scope = apply_set_scope(s, set_idx);
    SCHECK(s.default_index == "backend");
    SCHECK(s.default_type.empty());
    SCHECK(scope["index"] == "backend");

    // A subsequent tool call that omits `index` inherits the session default —
    // this mirrors handle_query's fallback: index = has("index") ? arg : default.
    ToolArgs q;
    q.obj = gogol::Json::object();
    q.obj["text"] = "order state machine";
    std::string resolved_index = q.has("index") ? q.get_str("index") : s.default_index;
    SCHECK(resolved_index == "backend");

    // set_scope {type:"note"} — leaves the previously-set index unchanged.
    ToolArgs set_type;
    set_type.obj = gogol::Json::object();
    set_type.obj["type"] = "note";
    apply_set_scope(s, set_type);
    SCHECK(s.default_index == "backend");  // unchanged
    SCHECK(s.default_type == "note");

    // An explicit arg overrides the session default.
    ToolArgs q2;
    q2.obj = gogol::Json::object();
    q2.obj["index"] = "web";
    std::string overridden = q2.has("index") ? q2.get_str("index") : s.default_index;
    SCHECK(overridden == "web");

    PASS();
}

void test_mcp_session_cursors() {
    TEST(mcp_session_cursors);
    using namespace gogol::mcp;

    Session s;
    // A fresh session has an empty cursor cache.
    SCHECK(s.last_results.is_array());
    SCHECK(s.last_results.empty());
    SCHECK(!s.result_ref(0).valid);  // nothing cached yet

    // Simulate what handle_query stores after producing results: the hit array.
    gogol::Json hits = gogol::Json::array();
    gogol::Json h0 = gogol::Json::object();
    h0["index"] = "backend"; h0["path"] = "src/order-state-machine.ts";
    h0["line"] = 34; h0["type"] = "doc";
    gogol::Json h1 = gogol::Json::object();
    h1["index"] = "backend"; h1["path"] = "src/orders.ts";
    h1["line"] = 166; h1["type"] = "doc";
    hits.push_back(h0);
    hits.push_back(h1);
    s.set_last_results(hits);

    // last_results is non-empty after a query stores results.
    SCHECK(s.last_results.is_array());
    SCHECK(s.last_results.size() == 2);

    // result_ref resolves a 0-based cursor to (type,index,path,line).
    ResultRef r0 = s.result_ref(0);
    SCHECK(r0.valid);
    SCHECK(r0.index == "backend");
    SCHECK(r0.path == "src/order-state-machine.ts");
    SCHECK(r0.line == 34);
    SCHECK(r0.type == "doc");

    ResultRef r1 = s.result_ref(1);
    SCHECK(r1.valid);
    SCHECK(r1.path == "src/orders.ts");
    SCHECK(r1.line == 166);

    // Out-of-range / negative cursors are invalid, not a crash.
    SCHECK(!s.result_ref(2).valid);
    SCHECK(!s.result_ref(-1).valid);

    // Storing a non-array resets the cache to empty (defensive).
    s.set_last_results(gogol::Json("not an array"));
    SCHECK(s.last_results.is_array());
    SCHECK(s.last_results.empty());

    PASS();
}

#undef SCHECK

// --- Config: [global] section + per-index watch override ---

void test_config_global_and_watch_override() {
    TEST(config_global_and_watch_override);
    fs::path home = fs::temp_directory_path() / "gogol-test-cfg";
    fs::create_directories(home / ".gogol");
    // watch on by default via [global]; index "off" overrides to off,
    // index "on" overrides debounce, index "inherit" takes the global values.
    {
        std::ofstream f(home / ".gogol" / "config");
        f << "[global]\n"
          << "watch = true\n"
          << "watch_debounce_ms = 3000\n\n"
          << "[inherit]\npath = /tmp/a\next = md\n\n"
          << "[off]\npath = /tmp/b\next = md\nwatch = false\n\n"
          << "[tuned]\npath = /tmp/c\next = md\nwatch_debounce_ms = 500\n";
    }
    const char* old_home = getenv("HOME");
    setenv("HOME", home.c_str(), 1);

    auto gc = load_global_config();
    assert(gc.watch == true);              // [global] parsed
    assert(gc.watch_debounce_ms == 3000);

    auto cfgs = load_config();
    assert(cfgs.count("inherit") && cfgs.count("off") && cfgs.count("tuned"));
    // [global] must NOT become an index.
    assert(!cfgs.count("global"));

    // inherit: no override → global values.
    assert(effective_watch(cfgs["inherit"], gc) == true);
    assert(effective_watch_debounce_ms(cfgs["inherit"], gc) == 3000);
    // off: overrides watch to false.
    assert(effective_watch(cfgs["off"], gc) == false);
    // tuned: watch inherited on, debounce overridden.
    assert(effective_watch(cfgs["tuned"], gc) == true);
    assert(effective_watch_debounce_ms(cfgs["tuned"], gc) == 500);

    // Legacy top-level keys (no [global]) still work.
    {
        std::ofstream f(home / ".gogol" / "config");
        f << "watch = false\n\n[a]\npath = /tmp/a\next = md\nwatch = true\n";
    }
    auto gc2 = load_global_config();
    assert(gc2.watch == false);            // legacy top-level parsed
    auto cfgs2 = load_config();
    assert(effective_watch(cfgs2["a"], gc2) == true); // index override wins

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    fs::remove_all(home);
    PASS();
}

// --- Main ---

int main() {
    printf("Running gogol tests...\n\n");
    test_config_global_and_watch_override();

    printf("[Scanner]\n");
    test_scanner_finds_md_files();
    test_scanner_skips_git();
    test_scanner_multiple_extensions();
    test_scanner_indexes_hidden_non_git_dirs();

    printf("\n[Chunker]\n");
    test_chunk_markdown_splits_on_headings();
    test_chunk_markdown_merges_tiny_sections();
    test_chunk_window_basic();

    printf("\n[Ref extractor]\n");
    test_ref_extractor_single_link();
    test_ref_extractor_multiple_links_per_line();
    test_ref_extractor_skips_images();
    test_ref_extractor_skips_code_fence();
    test_ref_extractor_anchor_and_url_not_filtered();
    test_ref_extractor_trims_target_whitespace();
    test_ref_extractor_supports();

    printf("\n[Location]\n");
    test_parse_entry_type();
    test_entry_type_str();
    test_split_path_line();
    test_format_loc_note();
    test_format_loc_doc();
    test_format_loc_doc_no_chunk();
    test_format_loc_doc_no_line();
    test_format_loc_term();

    printf("\n[Cosine]\n");
    test_cosine_similarity_identical();
    test_cosine_similarity_orthogonal();
    test_cosine_similarity_opposite();

    printf("\n[Utils]\n");
    test_split_csv();
    test_split_csv_empty();
    test_count_entries();

    printf("\n[f16 conversion]\n");
    test_f16_roundtrip_values();
    test_f16_zero();

    printf("\n[Split index format]\n");
    test_end_line_in_chunk();

    printf("\n[Import store]\n");
    test_import_store_queries();

    printf("\n[Docref store]\n");
    test_docref_store_queries();

    printf("\n[Type store]\n");
    test_type_store_queries();

    printf("\n[Metrics store]\n");
    test_metrics_store_for_file();

    printf("\n[Call store]\n");
    test_call_store_queries();

    printf("\n[Scope matching]\n");
    test_scope_bare_matches_all();
    test_scope_bang_files_only();
    test_scope_at_mem_only();
    test_scope_glob_pattern();
    test_scope_empty_matches_all();

    printf("\n[atomic_write]\n");
    test_atomic_write_success();
    test_atomic_write_returns_false_on_bad_path();

    printf("\n[WriteQueue]\n");
    test_write_queue_runs_jobs_in_order();
    test_write_queue_submit_wait_returns_result();
    test_write_queue_submit_wait_propagates_exception();
    test_write_queue_serializes_concurrent_submits();
    test_write_queue_job_exception_does_not_kill_worker();

    printf("\n[IndexConfig memory_dir]\n");
    test_memory_dir_default();
    test_memory_dir_explicit();

    printf("\n[Doc reference resolution]\n");
    test_resolve_doc_ref();

    printf("\n[SQLite]\n");
    test_sqlite_linked();

    printf("\n[Db wrapper]\n");
    test_db_wrapper();

    printf("\n[SqliteBackend]\n");
    test_sqlite_backend_roundtrip();
    test_fts5_keyword_search();

    printf("\n[Backend save_all/load_all]\n");
    test_backend_save_all_roundtrip_sqlite();

    printf("\n[JSON seam]\n");
    test_json_roundtrip();

    printf("\n[MCP tool registry]\n");
    test_tool_registry();
    test_tools_list_schema();

    printf("\n[MCP config gating]\n");
    test_mcp_config_parse();

    printf("\n[MCP sessions]\n");
    test_mcp_session_scope();
    test_mcp_session_cursors();

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
