// index_file.cpp — f16 (half-precision float) conversion helpers.
#include "storage/index_file.h"
#include <cstring>

// --- float16 conversion (IEEE 754 half-precision) ---

uint16_t float_to_f16(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = x & 0x7FFFFF;
  if (exp <= 0) return sign;
  if (exp >= 31) return sign | 0x7C00;
  return sign | (exp << 10) | (mant >> 13);
}

float f16_to_float(uint16_t h) {
  uint32_t sign = (h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  if (exp == 0) {
    float f = 0.0f;
    uint32_t r = sign;
    std::memcpy(&f, &r, 4);
    return f;
  }
  if (exp == 31) exp = 255;
  else exp = exp - 15 + 127;
  uint32_t result = sign | (exp << 23) | (mant << 13);
  float f;
  std::memcpy(&f, &result, 4);
  return f;
}
