// version.h — Build version stamp
// The version string lives in a single .cpp TU so both client and server
// resolve to the same symbol (linked from the same object file).
#pragma once
extern const char* gogol_build_version();
#define GOGOL_BUILD_VERSION gogol_build_version()
