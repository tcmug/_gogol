// types.h — Shared data types used across layers
#pragma once
#include <cstdint>
#include <string>

// Function-level metrics computed from AST
struct FunctionMetrics {
    std::string name;
    std::string file;     // set by caller
    uint32_t line;
    uint16_t complexity;  // cyclomatic complexity
    uint16_t lines;       // lines of code
    uint8_t params;       // parameter count
    uint8_t returns;      // return statement count
    uint8_t max_depth;    // max nesting depth
};
