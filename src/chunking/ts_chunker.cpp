#include "chunking/ts_chunker.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <tree_sitter/api.h>

// Grammar declarations
extern "C" {
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_php(void);
}

static const TSLanguage *get_language(const std::string &ext) {
  if (ext == ".ts")
    return tree_sitter_typescript();
  if (ext == ".tsx")
    return tree_sitter_tsx();
  if (ext == ".py")
    return tree_sitter_python();
  if (ext == ".go")
    return tree_sitter_go();
  if (ext == ".rs")
    return tree_sitter_rust();
  if (ext == ".c")
    return tree_sitter_c();
  if (ext == ".cpp" || ext == ".cc" || ext == ".hpp" || ext == ".h")
    return tree_sitter_cpp();
  if (ext == ".php")
    return tree_sitter_php();
  return nullptr;
}

bool has_treesitter_grammar(const std::string &ext) {
  return get_language(ext) != nullptr;
}

// Find first identifier-like child to use as chunk name
static std::string find_name(const std::string &content, TSNode node,
                             int depth = 0) {
  if (depth > 3)
    return "";
  uint32_t count = ts_node_child_count(node);

  // First pass: prioritize declarator children (skips return types in C/C++)
  for (uint32_t i = 0; i < count && i < 10; i++) {
    TSNode child = ts_node_child(node, i);
    const char *type = ts_node_type(child);
    if (std::strstr(type, "declarator")) {
      auto name = find_name(content, child, depth + 1);
      if (!name.empty())
        return name;
    }
  }

  // Second pass: direct identifiers and other recursive cases
  for (uint32_t i = 0; i < count && i < 10; i++) {
    TSNode child = ts_node_child(node, i);
    const char *type = ts_node_type(child);
    if (std::strstr(type, "identifier") || std::strstr(type, "name")) {
      uint32_t s = ts_node_start_byte(child);
      uint32_t e = ts_node_end_byte(child);
      if (e - s < 100)
        return content.substr(s, e - s);
    }
    if (std::strstr(type, "declaration") || std::strstr(type, "definition") ||
        std::strstr(type, "spec") || std::strstr(type, "item") ||
        std::strstr(type, "statement") || std::strstr(type, "assignment") ||
        std::strstr(type, "variable")) {
      auto name = find_name(content, child, depth + 1);
      if (!name.empty())
        return name;
    }
  }
  return "";
}

// Extract first line of a node (the signature)
static std::string extract_signature(const std::string &content, TSNode node) {
  uint32_t start = ts_node_start_byte(node);
  uint32_t end = ts_node_end_byte(node);
  // Find first newline or opening brace
  for (uint32_t i = start; i < end && i < start + 200; i++) {
    if (content[i] == '\n' || content[i] == '{') {
      return content.substr(start, i - start);
    }
  }
  return content.substr(start, std::min(end - start, (uint32_t)200));
}

// Extract preceding comment text (sibling before this node)
static std::string extract_preceding_comment(const std::string &content,
                                             TSNode node) {
  TSNode prev = ts_node_prev_sibling(node);
  if (ts_node_is_null(prev))
    return "";
  const char *type = ts_node_type(prev);
  if (!std::strstr(type, "comment"))
    return "";

  uint32_t s = ts_node_start_byte(prev);
  uint32_t e = ts_node_end_byte(prev);
  if (e - s > 500)
    e = s + 500; // Cap comment length
  std::string comment = content.substr(s, e - s);

  // Strip comment markers
  std::string clean;
  for (size_t i = 0; i < comment.size(); i++) {
    if (comment[i] == '/' && i + 1 < comment.size() &&
        (comment[i + 1] == '/' || comment[i + 1] == '*')) {
      i++; // skip //  or /*
      if (comment[i] == '*')
        i++; // skip *
      continue;
    }
    if (comment[i] == '*' && i + 1 < comment.size() && comment[i + 1] == '/') {
      i++;
      continue;
    }
    if (comment[i] == '*' && (i == 0 || comment[i - 1] == '\n'))
      continue; // leading * in block comments
    if (comment[i] == '#')
      continue; // python comments
    clean += comment[i];
  }
  // Trim
  while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\n'))
    clean.erase(clean.begin());
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\n'))
    clean.pop_back();
  return clean;
}

// Find enclosing parent name (class, struct, impl)
static std::string find_parent_name(const std::string &content, TSNode node) {
  TSNode parent = ts_node_parent(node);
  if (ts_node_is_null(parent))
    return "";
  TSNode grandparent = ts_node_parent(parent);
  if (ts_node_is_null(grandparent))
    return "";

  const char *gp_type = ts_node_type(grandparent);
  if (std::strstr(gp_type, "class") || std::strstr(gp_type, "struct") ||
      std::strstr(gp_type, "impl") || std::strstr(gp_type, "interface") ||
      std::strstr(gp_type, "module")) {
    return find_name(content, grandparent);
  }
  return "";
}

std::vector<Chunk> chunk_treesitter(const std::string &content,
                                    const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang)
    return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);

  TSTree *tree =
      ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) {
    ts_parser_delete(parser);
    return {};
  }

  TSNode root = ts_tree_root_node(tree);
  uint32_t child_count = ts_node_child_count(root);

  std::vector<Chunk> chunks;
  std::string pending;
  uint32_t pending_line = 1;

  for (uint32_t i = 0; i < child_count; i++) {
    TSNode child = ts_node_child(root, i);
    const char *type = ts_node_type(child);
    uint32_t start_byte = ts_node_start_byte(child);
    uint32_t end_byte = ts_node_end_byte(child);
    uint32_t start_line = ts_node_start_point(child).row + 1;
    uint32_t end_line = ts_node_end_point(child).row + 1;
    uint32_t lines = end_line - start_line + 1;

    // Skip comment and import nodes
    if (std::strstr(type, "comment") || std::strstr(type, "import")) {
      if (pending.empty())
        pending_line = start_line;
      if (!pending.empty())
        pending += "\n";
      pending += content.substr(start_byte, end_byte - start_byte);
      continue;
    }

    std::string node_text = content.substr(start_byte, end_byte - start_byte);

    if (lines >= 5) {
      // Discard accumulated small nodes (imports, type aliases, etc.)
      pending.clear();

      // Build enriched chunk
      std::string name = find_name(content, child);
      if (name.empty())
        name = type;

      // Improvement 3: parent context
      std::string parent_name = find_parent_name(content, child);
      std::string heading =
          parent_name.empty() ? name : parent_name + "." + name;

      // Improvement 2: signature as part of heading (params + return type)
      std::string sig = extract_signature(content, child);
      // Extract params from signature if present
      auto paren_start = sig.find('(');
      auto paren_end = sig.rfind(')');
      if (paren_start != std::string::npos && paren_end != std::string::npos &&
          paren_end > paren_start) {
        // Include params
        heading += sig.substr(paren_start, paren_end - paren_start + 1);
        // Include return type if present (between ) and { or end of line)
        auto after_paren = paren_end + 1;
        auto brace = sig.find('{', after_paren);
        auto arrow = sig.find("=>", after_paren);
        auto end_pos = std::min({brace, arrow, sig.size()});
        if (end_pos > after_paren) {
          std::string ret = sig.substr(after_paren, end_pos - after_paren);
          // Trim trailing whitespace
          while (!ret.empty() && (ret.back() == ' ' || ret.back() == '\t' || ret.back() == '\n'))
            ret.pop_back();
          if (!ret.empty() && ret.find(':') != std::string::npos)
            heading += ret;
        }
      }

      // Improvement 1: preceding comment
      std::string comment = extract_preceding_comment(content, child);

      // Improvement 4: build chunk content with name repetition + comment +
      // code
      std::string chunk_content;
      chunk_content += name + ". ";
      if (!comment.empty())
        chunk_content += comment + "\n";
      chunk_content += node_text;

      chunks.push_back({heading, start_line, end_line, chunk_content});
    } else {
      // Accumulate small nodes
      if (pending.empty())
        pending_line = start_line;
      if (!pending.empty())
        pending += "\n";
      pending += node_text;
    }
  }

  // Flush remaining
  // Discard remaining small nodes (imports at end of file)

  ts_tree_delete(tree);
  ts_parser_delete(parser);

  if (chunks.empty())
    return {};
  return chunks;
}

// --- Call-graph extraction ---

// Check if node type is a call expression (language-dependent)
static bool is_call_node(const char *type) {
  return std::strcmp(type, "call_expression") == 0 ||      // TS, C, Go, Rust
         std::strcmp(type, "call") == 0 ||                 // Python
         std::strcmp(type, "function_call_expression") == 0 || // PHP
         std::strcmp(type, "member_call_expression") == 0;    // PHP method calls
}

// Check if node type is a method/field access expression
static bool is_member_access(const char *type) {
  return std::strcmp(type, "member_expression") == 0 ||   // TS
         std::strcmp(type, "field_expression") == 0 ||    // C, Rust
         std::strcmp(type, "selector_expression") == 0 || // Go
         std::strcmp(type, "attribute") == 0;             // Python
}

// Extract the method/property name from a member access node
static std::string extract_method_name(const std::string &src, TSNode node) {
  uint32_t fc = ts_node_child_count(node);
  for (uint32_t i = fc; i > 0; i--) {
    TSNode prop = ts_node_child(node, i - 1);
    const char *pt = ts_node_type(prop);
    if (std::strstr(pt, "identifier") || std::strstr(pt, "name")) {
      uint32_t s = ts_node_start_byte(prop), e = ts_node_end_byte(prop);
      if (e - s < 80) return src.substr(s, e - s);
    }
  }
  return "";
}

// Recursively find call nodes within a function body
static void collect_calls(const std::string &src, TSNode node,
                          const std::string &caller,
                          std::vector<CallEdge> &edges) {
  const char *type = ts_node_type(node);

  if (is_call_node(type)) {
    TSNode fn = ts_node_child(node, 0);
    if (!ts_node_is_null(fn)) {
      std::string callee;
      const char *fn_type = ts_node_type(fn);
      if (is_member_access(fn_type)) {
        callee = extract_method_name(src, fn);
      } else if (std::strstr(fn_type, "identifier") || std::strcmp(fn_type, "name") == 0) {
        uint32_t s = ts_node_start_byte(fn), e = ts_node_end_byte(fn);
        if (e - s < 80) callee = src.substr(s, e - s);
      }
      // PHP member_call_expression: method name is a direct child named "name"
      if (callee.empty() && std::strcmp(type, "member_call_expression") == 0) {
        callee = extract_method_name(src, node);
      }
      if (!callee.empty() && callee != caller) {
        edges.push_back({caller, callee, ts_node_start_point(node).row + 1});
      }
    }
  }

  // Recurse but don't descend into nested named function definitions
  uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    const char *ct = ts_node_type(child);
    // Skip named function defs (they get their own caller entry in walk_for_calls)
    // But DO descend into arrow functions (callbacks) — their calls belong to parent
    if (std::strcmp(ct, "function_declaration") == 0 ||
        std::strcmp(ct, "function_definition") == 0 ||
        std::strcmp(ct, "function_item") == 0 ||
        std::strcmp(ct, "method_definition") == 0)
      continue;
    collect_calls(src, child, caller, edges);
  }
}

// Check if node is a function/method definition (any language)
static bool is_function_def(const char *type) {
  return std::strcmp(type, "function_declaration") == 0 ||  // TS, Go
         std::strcmp(type, "function_definition") == 0 ||   // C, Python, PHP
         std::strcmp(type, "function_item") == 0 ||         // Rust
         std::strcmp(type, "method_definition") == 0 ||     // TS class methods
         std::strcmp(type, "method_declaration") == 0;      // Go methods, PHP methods
}

// Check if node is a class/struct/impl container
static bool is_class_container(const char *type) {
  return std::strstr(type, "class_declaration") ||
         std::strstr(type, "class_definition") ||  // Python
         std::strcmp(type, "impl_item") == 0 ||    // Rust
         std::strstr(type, "struct");
}

// Check if node is a function body
static bool is_body_node(const char *type) {
  return std::strstr(type, "body") ||
         std::strstr(type, "block") ||
         std::strcmp(type, "compound_statement") == 0 ||   // C, PHP
         std::strcmp(type, "declaration_list") == 0 ||     // PHP class body
         std::strcmp(type, "statement_list") == 0;         // Go (inside block)
}

// Shared AST walker: finds all function definitions and calls visitor for each.
// Visitor receives: (qualified_name, function_node, body_node, is_arrow)
using FnBodyVisitor = void (*)(const std::string &src, const std::string &name,
                               TSNode fn_node, TSNode body_node, void *ctx);

static void walk_functions(const std::string &src, TSNode node,
                           const std::string &parent_ctx,
                           FnBodyVisitor visitor, void *ctx) {
  uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    const char *type = ts_node_type(child);

    if (std::strcmp(type, "export_statement") == 0 ||
        std::strcmp(type, "decorated_definition") == 0) {
      walk_functions(src, child, parent_ctx, visitor, ctx);
    } else if (is_class_container(type)) {
      std::string class_name = find_name(src, child);
      uint32_t cc = ts_node_child_count(child);
      for (uint32_t j = 0; j < cc; j++) {
        TSNode member = ts_node_child(child, j);
        const char *mt = ts_node_type(member);
        if (is_body_node(mt) || std::strstr(mt, "declaration_list"))
          walk_functions(src, member, class_name, visitor, ctx);
      }
    } else if (is_function_def(type)) {
      std::string fn_name = find_name(src, child);
      if (fn_name.empty()) continue;
      std::string full = parent_ctx.empty() ? fn_name : parent_ctx + "." + fn_name;
      uint32_t cc = ts_node_child_count(child);
      for (uint32_t j = 0; j < cc; j++) {
        TSNode body = ts_node_child(child, j);
        if (is_body_node(ts_node_type(body))) {
          visitor(src, full, child, body, ctx);
          break;
        }
      }
    } else if (std::strcmp(type, "lexical_declaration") == 0 ||
               std::strcmp(type, "variable_declaration") == 0) {
      uint32_t dc = ts_node_child_count(child);
      for (uint32_t j = 0; j < dc; j++) {
        TSNode decl = ts_node_child(child, j);
        if (std::strcmp(ts_node_type(decl), "variable_declarator") != 0) continue;
        std::string var_name = find_name(src, decl);
        if (var_name.empty()) continue;
        std::string full = parent_ctx.empty() ? var_name : parent_ctx + "." + var_name;
        uint32_t vc = ts_node_child_count(decl);
        for (uint32_t k = 0; k < vc; k++) {
          TSNode val = ts_node_child(decl, k);
          const char *vt = ts_node_type(val);
          if (std::strstr(vt, "arrow_function") || std::strstr(vt, "function")) {
            uint32_t bc = ts_node_child_count(val);
            for (uint32_t l = 0; l < bc; l++) {
              TSNode body = ts_node_child(val, l);
              if (is_body_node(ts_node_type(body))) {
                visitor(src, full, val, body, ctx);
                break;
              }
            }
          } else if (std::strcmp(vt, "object") == 0) {
            walk_functions(src, val, var_name, visitor, ctx);
          }
        }
      }
    } else {
      walk_functions(src, child, parent_ctx, visitor, ctx);
    }
  }
}

// --- Call extraction visitor ---
static void calls_visitor(const std::string &src, const std::string &name,
                          TSNode fn_node, TSNode body_node, void *ctx) {
  (void)fn_node;
  auto *edges = static_cast<std::vector<CallEdge> *>(ctx);
  collect_calls(src, body_node, name, *edges);
}

std::vector<CallEdge> extract_calls(const std::string &content,
                                    const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang) return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  TSTree *tree = ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) { ts_parser_delete(parser); return {}; }

  std::vector<CallEdge> edges;
  walk_functions(content, ts_tree_root_node(tree), "", calls_visitor, &edges);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return edges;
}

// --- Import extraction ---

// Find the first string/path child within an import node
static std::string find_import_path(const std::string &src, TSNode node) {
  uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    const char *type = ts_node_type(child);
    // TS/JS: string_fragment inside string
    if (std::strcmp(type, "string_fragment") == 0 ||
        std::strcmp(type, "interpreted_string_literal_content") == 0) {
      uint32_t s = ts_node_start_byte(child), e = ts_node_end_byte(child);
      if (e - s < 200) return src.substr(s, e - s);
    }
    // Go: interpreted_string_literal (has quotes)
    if (std::strstr(type, "string_literal") || std::strcmp(type, "interpreted_string_literal") == 0) {
      uint32_t s = ts_node_start_byte(child), e = ts_node_end_byte(child);
      std::string text = src.substr(s, e - s);
      // Strip quotes
      if (text.size() >= 2 && (text[0] == '"' || text[0] == '\''))
        return text.substr(1, text.size() - 2);
      return text;
    }
    // Python: dotted_name / module_name
    if (std::strcmp(type, "dotted_name") == 0 || std::strcmp(type, "module_name") == 0) {
      uint32_t s = ts_node_start_byte(child), e = ts_node_end_byte(child);
      if (e - s < 200) return src.substr(s, e - s);
    }
    // Recurse into string nodes
    if (std::strcmp(type, "string") == 0 || std::strstr(type, "import_spec") ||
        std::strcmp(type, "interpreted_string_literal") == 0) {
      std::string r = find_import_path(src, child);
      if (!r.empty()) return r;
    }
  }
  return "";
}

// Extract named import symbols from an import node (TS/JS: import { X, Y } from ...)
static std::vector<std::string> find_named_imports(const std::string &src, TSNode node) {
  std::vector<std::string> symbols;
  uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    const char *type = ts_node_type(child);
    // TS/JS: import_clause → named_imports → import_specifier
    if (std::strcmp(type, "import_specifier") == 0) {
      // The import_specifier has a "name" child or is itself the identifier
      uint32_t sc = ts_node_child_count(child);
      for (uint32_t j = 0; j < sc; j++) {
        TSNode spec_child = ts_node_child(child, j);
        const char *st = ts_node_type(spec_child);
        if (std::strcmp(st, "identifier") == 0 || std::strcmp(st, "type_identifier") == 0) {
          uint32_t s = ts_node_start_byte(spec_child), e = ts_node_end_byte(spec_child);
          if (e - s < 200) symbols.push_back(src.substr(s, e - s));
          break; // take the first identifier (the local name, not "as" alias source)
        }
      }
      // If no child identifiers, the specifier itself is the name
      if (symbols.empty() || symbols.back().empty()) {
        uint32_t s = ts_node_start_byte(child), e = ts_node_end_byte(child);
        std::string text = src.substr(s, e - s);
        // Strip "as alias" if present
        auto as_pos = text.find(" as ");
        if (as_pos != std::string::npos) text = text.substr(0, as_pos);
        if (!text.empty() && text.size() < 100)
          symbols.push_back(text);
      }
    }
    // Python: from X import Y, Z — look for dotted_name/identifier in import list
    else if (std::strcmp(type, "dotted_name") == 0 || 
             (std::strcmp(type, "identifier") == 0 && i > 0)) {
      // Skip the module name (first dotted_name is the module)
      uint32_t s = ts_node_start_byte(child), e = ts_node_end_byte(child);
      if (e - s < 200) symbols.push_back(src.substr(s, e - s));
    }
    // Recurse into import_clause, named_imports, import_list etc.
    else if (std::strstr(type, "import") || std::strstr(type, "named") ||
             std::strstr(type, "clause") || std::strstr(type, "list")) {
      auto sub = find_named_imports(src, child);
      symbols.insert(symbols.end(), sub.begin(), sub.end());
    }
  }
  return symbols;
}

std::vector<ImportEdge> extract_imports(const std::string &content,
                                       const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang) return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  TSTree *tree = ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) { ts_parser_delete(parser); return {}; }

  std::vector<ImportEdge> imports;
  TSNode root = ts_tree_root_node(tree);
  uint32_t count = ts_node_child_count(root);

  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(root, i);
    const char *type = ts_node_type(child);

    bool is_import = std::strstr(type, "import") ||
                     std::strcmp(type, "preproc_include") == 0 ||  // C/C++
                     std::strcmp(type, "use_declaration") == 0;    // PHP/Rust

    if (!is_import) continue;

    // Go has import_declaration with multiple import_spec children
    if (std::strcmp(type, "import_declaration") == 0) {
      // Go: import_declaration → import_spec_list → import_spec*
      uint32_t ic = ts_node_child_count(child);
      for (uint32_t j = 0; j < ic; j++) {
        TSNode inner = ts_node_child(child, j);
        const char *it = ts_node_type(inner);
        if (std::strcmp(it, "import_spec") == 0) {
          std::string path = find_import_path(content, inner);
          if (!path.empty()) imports.push_back({path, {}});
        } else if (std::strstr(it, "list") || std::strstr(it, "group")) {
          // Recurse one level into import_spec_list
          uint32_t lc = ts_node_child_count(inner);
          for (uint32_t k = 0; k < lc; k++) {
            TSNode spec = ts_node_child(inner, k);
            if (std::strcmp(ts_node_type(spec), "import_spec") == 0) {
              std::string path = find_import_path(content, spec);
              if (!path.empty()) imports.push_back({path, {}});
            }
          }
        }
      }
    } else {
      std::string path = find_import_path(content, child);
      if (!path.empty()) {
        auto symbols = find_named_imports(content, child);
        imports.push_back({path, symbols});
      }
    }
  }

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return imports;
}

// --- Function metrics extraction ---

// Check if a node type is a decision point
static bool is_decision(const char *type) {
  return std::strcmp(type, "if_statement") == 0 ||
         std::strcmp(type, "for_statement") == 0 ||
         std::strcmp(type, "for_in_statement") == 0 ||
         std::strcmp(type, "for_range_loop") == 0 ||
         std::strcmp(type, "while_statement") == 0 ||
         std::strcmp(type, "do_statement") == 0 ||
         std::strcmp(type, "catch_clause") == 0 ||
         std::strcmp(type, "switch_case") == 0 ||
         std::strcmp(type, "case_statement") == 0 ||
         std::strcmp(type, "conditional_expression") == 0 ||
         std::strcmp(type, "ternary_expression") == 0;
}

// Check if node is a logical operator (&&, ||)
static bool is_logical_op(const char *type) {
  return std::strcmp(type, "&&") == 0 || std::strcmp(type, "||") == 0 ||
         std::strcmp(type, "and") == 0 || std::strcmp(type, "or") == 0;
}

// Count decisions + returns + max nesting in a function body
struct BodyStats {
  int decisions = 0;
  int returns = 0;
  int max_depth = 0;
};

static void analyze_body(TSNode node, int depth, BodyStats &stats) {
  const char *type = ts_node_type(node);

  if (is_decision(type)) {
    stats.decisions++;
    depth++;
    if (depth > stats.max_depth) stats.max_depth = depth;
  }

  if (std::strcmp(type, "return_statement") == 0)
    stats.returns++;

  // Logical operators in binary expressions
  if (std::strcmp(type, "binary_expression") == 0 ||
      std::strcmp(type, "boolean_operator") == 0) {
    uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; i++) {
      TSNode child = ts_node_child(node, i);
      if (is_logical_op(ts_node_type(child)))
        stats.decisions++;
    }
  }

  // Recurse (skip nested function defs)
  uint32_t cc = ts_node_child_count(node);
  for (uint32_t i = 0; i < cc; i++) {
    TSNode child = ts_node_child(node, i);
    const char *ct = ts_node_type(child);
    if (std::strcmp(ct, "function_declaration") == 0 ||
        std::strcmp(ct, "function_definition") == 0 ||
        std::strcmp(ct, "function_item") == 0 ||
        std::strcmp(ct, "arrow_function") == 0 ||
        std::strcmp(ct, "method_definition") == 0 ||
        std::strcmp(ct, "lambda_expression") == 0)
      continue;
    analyze_body(child, depth, stats);
  }
}

// Count parameters in a parameter list node
static int count_params(TSNode params_node) {
  int count = 0;
  uint32_t cc = ts_node_child_count(params_node);
  for (uint32_t i = 0; i < cc; i++) {
    TSNode child = ts_node_child(params_node, i);
    const char *type = ts_node_type(child);
    if (std::strstr(type, "parameter") || std::strstr(type, "param") ||
        std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "required_parameter") == 0 ||
        std::strcmp(type, "optional_parameter") == 0 ||
        std::strcmp(type, "simple_parameter") == 0)
      count++;
  }
  return count;
}

// --- Metrics extraction visitor ---
static void metrics_visitor(const std::string &src, const std::string &name,
                            TSNode fn_node, TSNode body_node, void *ctx) {
  auto *metrics = static_cast<std::vector<FunctionMetrics> *>(ctx);

  uint32_t start_line = ts_node_start_point(fn_node).row + 1;
  uint32_t end_line = ts_node_end_point(fn_node).row + 1;
  uint16_t lines = (uint16_t)(end_line - start_line + 1);

  // Count params from fn_node children
  int params = 0;
  uint32_t fc = ts_node_child_count(fn_node);
  for (uint32_t j = 0; j < fc; j++) {
    TSNode part = ts_node_child(fn_node, j);
    const char *pt = ts_node_type(part);
    if (std::strstr(pt, "parameter") || std::strstr(pt, "formal"))
      params = count_params(part);
  }

  BodyStats stats;
  analyze_body(body_node, 0, stats);

  metrics->push_back({name, "", start_line,
                      (uint16_t)(stats.decisions + 1), lines,
                      (uint8_t)params, (uint8_t)stats.returns,
                      (uint8_t)stats.max_depth});
}

std::vector<FunctionMetrics> extract_metrics(const std::string &content,
                                            const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang) return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  TSTree *tree = ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) { ts_parser_delete(parser); return {}; }

  std::vector<FunctionMetrics> metrics;
  walk_functions(content, ts_tree_root_node(tree), "", metrics_visitor, &metrics);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return metrics;
}

// --- Export extraction ---

std::vector<ExportedSymbol> extract_exports(const std::string &content,
                                           const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang) return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  TSTree *tree = ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) { ts_parser_delete(parser); return {}; }

  std::vector<ExportedSymbol> exports;
  TSNode root = ts_tree_root_node(tree);
  uint32_t count = ts_node_child_count(root);

  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(root, i);
    const char *type = ts_node_type(child);
    uint32_t line = ts_node_start_point(child).row + 1;
    uint32_t start = ts_node_start_byte(child);
    uint32_t end = ts_node_end_byte(child);

    // TS/JS: export_statement, exported declarations
    bool is_export = std::strstr(type, "export") != nullptr;
    // Go: capitalized names are exported (handled separately)
    // Rust: pub keyword

    if (!is_export) {
      // Go: top-level function/type with capitalized first letter
      if (ext == "go" && (std::strstr(type, "function") || std::strstr(type, "type"))) {
        std::string name = find_name(content, child);
        if (!name.empty() && std::isupper(name[0])) {
          std::string kind = std::strstr(type, "function") ? "function" : "type";
          exports.push_back({name, kind, line});
        }
      }
      // Rust: check for pub keyword
      if ((ext == "rs") && end > start) {
        std::string text = content.substr(start, std::min((uint32_t)10, end - start));
        if (text.find("pub ") == 0 || text.find("pub(") == 0) {
          std::string name = find_name(content, child);
          if (!name.empty()) {
            std::string kind = "function";
            if (std::strstr(type, "struct")) kind = "struct";
            else if (std::strstr(type, "enum")) kind = "enum";
            else if (std::strstr(type, "trait")) kind = "trait";
            else if (std::strstr(type, "type")) kind = "type";
            exports.push_back({name, kind, line});
          }
        }
      }
      continue;
    }

    // TS/JS export statements
    // Look for the exported name inside
    uint32_t cc = ts_node_child_count(child);
    for (uint32_t j = 0; j < cc; j++) {
      TSNode inner = ts_node_child(child, j);
      const char *inner_type = ts_node_type(inner);

      std::string kind = "const";
      if (std::strstr(inner_type, "function")) kind = "function";
      else if (std::strstr(inner_type, "class")) kind = "class";
      else if (std::strstr(inner_type, "interface")) kind = "interface";
      else if (std::strstr(inner_type, "type_alias")) kind = "type";
      else if (std::strstr(inner_type, "enum")) kind = "enum";
      else if (std::strcmp(inner_type, "lexical_declaration") == 0 ||
               std::strcmp(inner_type, "variable_declaration") == 0) kind = "const";

      std::string name = find_name(content, inner);
      if (!name.empty()) {
        exports.push_back({name, kind, line});
      }

      // export default
      if (std::strcmp(inner_type, "default") == 0 || std::strstr(inner_type, "default")) {
        // Try to find the name of the default export
        if (j + 1 < cc) {
          TSNode next = ts_node_child(child, j + 1);
          std::string dname = find_name(content, next);
          if (!dname.empty())
            exports.push_back({dname, "default", line});
        }
      }
    }
  }

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return exports;
}

// --- Type hierarchy extraction ---

std::vector<TypeEdge> extract_type_edges(const std::string &content,
                                         const std::string &ext) {
  const TSLanguage *lang = get_language(ext);
  if (!lang) return {};

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  TSTree *tree = ts_parser_parse_string(parser, nullptr, content.c_str(), content.size());
  if (!tree) { ts_parser_delete(parser); return {}; }

  std::vector<TypeEdge> edges;
  TSNode root = ts_tree_root_node(tree);
  uint32_t count = ts_node_child_count(root);

  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(root, i);
    const char *type = ts_node_type(child);
    uint32_t line = ts_node_start_point(child).row + 1;

    // Look for class, interface, struct declarations
    bool is_type_decl = std::strstr(type, "class") || std::strstr(type, "interface") ||
                        std::strstr(type, "struct") || std::strstr(type, "impl") ||
                        std::strstr(type, "trait");
    if (!is_type_decl) {
      // Also check inside export_statement
      if (std::strstr(type, "export")) {
        uint32_t cc = ts_node_child_count(child);
        for (uint32_t j = 0; j < cc; j++) {
          TSNode inner = ts_node_child(child, j);
          const char *it = ts_node_type(inner);
          if (std::strstr(it, "class") || std::strstr(it, "interface")) {
            child = inner;
            type = it;
            line = ts_node_start_point(inner).row + 1;
            is_type_decl = true;
            break;
          }
        }
      }
      if (!is_type_decl) continue;
    }

    std::string name = find_name(content, child);
    if (name.empty()) continue;

    // Scan children for extends/implements clauses
    uint32_t cc = ts_node_child_count(child);
    for (uint32_t j = 0; j < cc; j++) {
      TSNode clause = ts_node_child(child, j);
      const char *ct = ts_node_type(clause);

      std::string edge_kind;
      if (std::strstr(ct, "extends") || std::strcmp(ct, "superclass") == 0 ||
          std::strcmp(ct, "base_class_clause") == 0)
        edge_kind = "extends";
      else if (std::strstr(ct, "implements") || std::strstr(ct, "conformance") ||
               std::strcmp(ct, "type_list") == 0)
        edge_kind = "implements";

      if (edge_kind.empty()) continue;

      // Extract parent type names from the clause
      uint32_t pc = ts_node_child_count(clause);
      for (uint32_t k = 0; k < pc; k++) {
        TSNode parent_node = ts_node_child(clause, k);
        const char *pt = ts_node_type(parent_node);
        // Look for identifiers/type_identifiers
        if (std::strcmp(pt, "identifier") == 0 || std::strcmp(pt, "type_identifier") == 0 ||
            std::strstr(pt, "type") || std::strcmp(pt, "generic_type") == 0) {
          // For generic types, get the base name
          std::string parent_name;
          if (std::strcmp(pt, "generic_type") == 0) {
            // First child is the type name
            if (ts_node_child_count(parent_node) > 0) {
              TSNode base = ts_node_child(parent_node, 0);
              uint32_t s = ts_node_start_byte(base), e = ts_node_end_byte(base);
              parent_name = content.substr(s, e - s);
            }
          } else {
            uint32_t s = ts_node_start_byte(parent_node), e = ts_node_end_byte(parent_node);
            parent_name = content.substr(s, e - s);
          }
          if (!parent_name.empty() && parent_name != name)
            edges.push_back({name, parent_name, edge_kind, line});
        }
      }

      // If no children matched, try the clause text directly
      if (edges.empty() || edges.back().name != name) {
        uint32_t s = ts_node_start_byte(clause), e = ts_node_end_byte(clause);
        std::string clause_text = content.substr(s, e - s);
        // Remove keyword
        auto space = clause_text.find(' ');
        if (space != std::string::npos) {
          std::string parents = clause_text.substr(space + 1);
          // Split by comma
          std::istringstream ss(parents);
          std::string p;
          while (std::getline(ss, p, ',')) {
            // Trim
            auto ps = p.find_first_not_of(" \t");
            auto pe = p.find_last_not_of(" \t{");
            if (ps != std::string::npos && pe != std::string::npos && pe >= ps) {
              std::string parent_name = p.substr(ps, pe - ps + 1);
              // Remove generic params
              auto angle = parent_name.find('<');
              if (angle != std::string::npos) parent_name = parent_name.substr(0, angle);
              if (!parent_name.empty() && parent_name != name)
                edges.push_back({name, parent_name, edge_kind, line});
            }
          }
        }
      }
    }
  }

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return edges;
}
