// Tests must run their checks even in a Release (-DNDEBUG) build. Without this,
// every assert() below is preprocessed away and the suite passes vacuously
// (the set-but-not-used `found_*` warnings were the symptom). Force asserts active.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

#include "chunking/ts_chunker.h"
#include "chunking/chunker_iface.h"
#include <algorithm>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)

// A chunk heading is the symbol name, often followed by its signature / generics /
// parent context (e.g. "handlePayment(card: string): boolean", "Svc.charge(...)").
// So tests match by NAME: heading == name, or name immediately followed by a
// non-identifier char (so "process" matches "process(x)" but NOT "processItems").
static bool heading_is(const std::string &heading, const std::string &name) {
    if (heading == name) return true;
    if (heading.size() <= name.size()) return false;
    if (heading.compare(0, name.size(), name) != 0) return false;
    char n = heading[name.size()];
    bool ident = (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
                 (n >= '0' && n <= '9') || n == '_';
    return !ident;  // next char ends the identifier => this heading IS `name`
}
#define PASS() do { tests_passed++; printf("ok\n"); } while(0)

void test_has_grammar() {
    TEST(has_grammar);
    assert(has_treesitter_grammar(".ts"));
    assert(has_treesitter_grammar(".tsx"));
    assert(has_treesitter_grammar(".py"));
    assert(has_treesitter_grammar(".go"));
    assert(has_treesitter_grammar(".rs"));
    assert(has_treesitter_grammar(".c"));
    assert(has_treesitter_grammar(".cpp"));
    assert(has_treesitter_grammar(".cc"));
    assert(has_treesitter_grammar(".h"));
    assert(has_treesitter_grammar(".php"));
    assert(has_treesitter_grammar(".gd"));
    assert(!has_treesitter_grammar(".md"));
    assert(!has_treesitter_grammar(".txt"));
    assert(!has_treesitter_grammar(".json"));
    PASS();
}

void test_typescript_chunking() {
    TEST(typescript_chunking);
    std::string code = R"(
import { foo } from 'bar';

export function handlePayment(card: string): boolean {
  const token = validate(card);
  if (!token) return false;
  return processPayment(token);
}

export class PaymentService {
  private api: Api;

  constructor(api: Api) {
    this.api = api;
  }

  async charge(amount: number): Promise<void> {
    await this.api.post('/charge', { amount });
  }
}
)";
    auto chunks = chunk_treesitter(code, ".ts");
    assert(chunks.size() >= 2);
    // Should find handlePayment and PaymentService
    bool found_func = false, found_class = false;
    for (auto& c : chunks) {
        if (heading_is(c.heading, "handlePayment")) found_func = true;
        if (heading_is(c.heading, "PaymentService")) found_class = true;
    }
    assert(found_func);
    assert(found_class);
    PASS();
}

void test_python_chunking() {
    TEST(python_chunking);
    std::string code = R"(
import os
from typing import List

def process(items: List[str]) -> int:
    total = 0
    for item in items:
        total += len(item)
    return total

class Store:
    def __init__(self, path: str):
        self.path = path
        self.data = {}

    def save(self, key: str, value: str):
        self.data[key] = value
)";
    auto chunks = chunk_treesitter(code, ".py");
    assert(chunks.size() >= 2);
    bool found_func = false, found_class = false;
    for (auto& c : chunks) {
        if (heading_is(c.heading, "process")) found_func = true;
        if (heading_is(c.heading, "Store")) found_class = true;
    }
    assert(found_func);
    assert(found_class);
    PASS();
}

void test_go_chunking() {
    TEST(go_chunking);
    std::string code = R"(
package main

import "fmt"

func processItems(items []string) int {
    total := 0
    for _, item := range items {
        total += len(item)
    }
    return total
}

type DataStore struct {
    path string
    data map[string]string
    dirty bool
    count int
}

func NewDataStore(path string) *DataStore {
    return &DataStore{
        path: path,
        data: make(map[string]string),
    }
}
)";
    auto chunks = chunk_treesitter(code, ".go");
    assert(chunks.size() >= 2);
    bool found_func = false, found_type = false;
    for (auto& c : chunks) {
        if (heading_is(c.heading, "processItems")) found_func = true;
        if (heading_is(c.heading, "DataStore")) found_type = true;
    }
    assert(found_func);
    assert(found_type);
    PASS();
}

void test_gdscript_chunking() {
    TEST(gdscript_chunking);
    std::string code = R"(
extends Node

func process_data(value: int, name: String) -> bool:
    if value > 0:
        print("Positive: ", name)
        return true
    elif value < 0:
        print("Negative: ", name)
        return false
    else:
        print("Zero: ", name)
        return false

class MyNode extends Node2D:
    var speed: float = 10.0
    
    func _ready():
        pass
    
    func move():
        position.x += speed
)";
    auto chunks = chunk_treesitter(code, ".gd");
    assert(chunks.size() >= 2);
    bool found_func = false, found_class = false;
    for (auto& c : chunks) {
        if (heading_is(c.heading, "process_data")) found_func = true;
        if (heading_is(c.heading, "MyNode")) found_class = true;
    }
    assert(found_func);
    assert(found_class);
    PASS();
}

void test_fallback_on_unknown_ext() {
    TEST(fallback_on_unknown_ext);
    auto chunks = chunk_treesitter("some content", ".xyz");
    assert(chunks.empty());
    PASS();
}

void test_small_nodes_merged() {
    TEST(small_nodes_merged);
    std::string code = R"(
import a from 'a';
import b from 'b';
import c from 'c';

export function bigFunction(): void {
  const x = 1;
  const y = 2;
  const z = 3;
  console.log(x + y + z);
}
)";
    auto chunks = chunk_treesitter(code, ".ts");
    // Imports are merged into `pending` and discarded; bigFunction is its own chunk.
    bool found_func = false;
    for (auto& c : chunks) {
        if (heading_is(c.heading, "bigFunction")) found_func = true;
    }
    assert(found_func);
    PASS();
}

// --- Call extraction tests ---

void test_extract_calls_typescript() {
    TEST(extract_calls_typescript);
    std::string code = R"(
export function handlePayment(card: string) {
  const token = validate(card);
  const result = processPayment(token);
  logger.info("done");
  return result;
}

export const authorize = (amount: number) => {
  checkBalance(amount);
  return handlePayment("card");
};
)";
    auto edges = extract_calls(code, ".ts");
    assert(!edges.empty());
    // handlePayment should call validate, processPayment, info
    bool found_validate = false, found_process = false, found_info = false;
    bool found_check = false, found_handle = false;
    for (auto& e : edges) {
        if (e.caller == "handlePayment" && e.callee == "validate") found_validate = true;
        if (e.caller == "handlePayment" && e.callee == "processPayment") found_process = true;
        if (e.caller == "handlePayment" && e.callee == "info") found_info = true;
        if (e.caller == "authorize" && e.callee == "checkBalance") found_check = true;
        if (e.caller == "authorize" && e.callee == "handlePayment") found_handle = true;
    }
    assert(found_validate);
    assert(found_process);
    assert(found_info);
    assert(found_check);
    assert(found_handle);
    PASS();
}

void test_extract_calls_cpp() {
    TEST(extract_calls_cpp);
    std::string code = R"(
#include <string>

void helper(int x) {
    printf("value: %d\n", x);
}

int process(const std::string& input) {
    helper(42);
    auto result = transform(input);
    return validate(result);
}
)";
    auto edges = extract_calls(code, ".cpp");
    assert(!edges.empty());
    bool found_printf = false, found_helper = false, found_transform = false;
    for (auto& e : edges) {
        if (e.caller == "helper" && e.callee == "printf") found_printf = true;
        if (e.caller == "process" && e.callee == "helper") found_helper = true;
        if (e.caller == "process" && e.callee == "transform") found_transform = true;
    }
    assert(found_printf);
    assert(found_helper);
    assert(found_transform);
    PASS();
}

void test_extract_calls_python() {
    TEST(extract_calls_python);
    std::string code = R"(
def fetch_data(url):
    response = requests.get(url)
    return parse_json(response.text)

def process(items):
    filtered = filter_items(items)
    return transform(filtered)
)";
    auto edges = extract_calls(code, ".py");
    assert(!edges.empty());
    bool found_get = false, found_parse = false, found_filter = false;
    for (auto& e : edges) {
        if (e.caller == "fetch_data" && e.callee == "get") found_get = true;
        if (e.caller == "fetch_data" && e.callee == "parse_json") found_parse = true;
        if (e.caller == "process" && e.callee == "filter_items") found_filter = true;
    }
    assert(found_get);
    assert(found_parse);
    assert(found_filter);
    PASS();
}

void test_extract_calls_go() {
    TEST(extract_calls_go);
    std::string code = R"(
package main

func handler(w http.ResponseWriter, r *http.Request) {
    data := parseBody(r)
    result := processData(data)
    json.NewEncoder(w).Encode(result)
}

func main() {
    handler(nil, nil)
}
)";
    auto edges = extract_calls(code, ".go");
    assert(!edges.empty());
    bool found_parse = false, found_process = false, found_handler = false;
    for (auto& e : edges) {
        if (e.caller == "handler" && e.callee == "parseBody") found_parse = true;
        if (e.caller == "handler" && e.callee == "processData") found_process = true;
        if (e.caller == "main" && e.callee == "handler") found_handler = true;
    }
    assert(found_parse);
    assert(found_process);
    assert(found_handler);
    PASS();
}

// Default index extension set (configless indexing): must be derived from the
// chunkers gogol actually has — every tree-sitter grammar extension plus the
// built-in regex-chunker doc formats, and it must NOT include arbitrary types.
void test_default_index_extensions() {
    TEST(default_index_extensions);
    auto exts = default_index_extensions();
    auto has = [&](const std::string &e) {
        return std::find(exts.begin(), exts.end(), e) != exts.end();
    };
    // every tree-sitter grammar ext is present (single-source: has_treesitter_grammar)
    assert(has(".ts") && has(".tsx") && has(".py") && has(".go") && has(".rs"));
    assert(has(".c") && has(".cpp") && has(".cc") && has(".h") && has(".hpp") && has(".php") && has(".gd"));
    // built-in regex-chunker doc formats present
    assert(has(".md") && has(".graphql") && has(".yaml") && has(".yml") && has(".sql") && has(".rst"));
    // NOT a catch-all: unrelated types are absent (WindowChunker not enumerated)
    assert(!has(".txt") && !has(".png") && !has(".exe"));
    // consistency: every code ext (not a doc format) is a real tree-sitter grammar
    for (const auto &e : exts)
        if (e != ".md" && e != ".mdx" && e != ".graphql" && e != ".gql" &&
            e != ".yml" && e != ".yaml" && e != ".sql" && e != ".rst")
            assert(has_treesitter_grammar(e));
    PASS();
}

int main() {
    printf("Running tree-sitter chunker tests...\n\n");
    test_has_grammar();
    test_typescript_chunking();
    test_python_chunking();
    test_go_chunking();
    test_gdscript_chunking();
    test_fallback_on_unknown_ext();
    test_small_nodes_merged();
    test_extract_calls_typescript();
    test_extract_calls_cpp();
    test_extract_calls_python();
    test_extract_calls_go();
    test_default_index_extensions();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
