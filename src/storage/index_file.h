// index_file.h — f16 (half-precision float) conversion helpers for embedding storage.
#pragma once
#include <cstdint>

// Convert float32 to float16 (IEEE 754 half-precision).
uint16_t float_to_f16(float f);

// Convert float16 back to float32.
float f16_to_float(uint16_t h);
