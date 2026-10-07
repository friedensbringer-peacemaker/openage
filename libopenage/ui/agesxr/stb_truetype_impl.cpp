// Copyright 2026-2026 the openage authors. See copying.md for legal info.

// stb_truetype implementation (third party, public domain / MIT) in its own
// translation unit, so its warnings do not affect our code (CMake: -w here).
// The XR layer (native/xr/stb_truetype_impl.c) does the same as a C file.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
