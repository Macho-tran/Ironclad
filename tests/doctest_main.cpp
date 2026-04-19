// SPDX-License-Identifier: MIT
// Single translation unit that defines doctest's main(). Linked into every
// test binary; no other test .cpp should define DOCTEST_CONFIG_IMPLEMENT.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
