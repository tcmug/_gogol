// debug.h — Debug logging macros (compiled out in Release builds)
#pragma once
#include <cstdio>

#ifdef GOGOL_DEBUG
  #define DBG(fmt, ...) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__)
  #define DBG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
  #define DBG_ERR(fmt, ...) fprintf(stderr, "[ERR] " fmt "\n", ##__VA_ARGS__)
#else
  #define DBG(fmt, ...) ((void)0)
  #define DBG_WARN(fmt, ...) ((void)0)
  #define DBG_ERR(fmt, ...) fprintf(stderr, "[ERR] " fmt "\n", ##__VA_ARGS__)
#endif

// Always-on warnings for data corruption (lightweight, no #ifdef)
#define WARN_CORRUPT(fmt, ...) fprintf(stderr, "[CORRUPT] " fmt "\n", ##__VA_ARGS__)
