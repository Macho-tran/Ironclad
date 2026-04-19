// SPDX-License-Identifier: MIT
// Minimal smoke test that just verifies the test framework + library
// link together. Real subsystem tests live in their own files.
#include "doctest.h"

#include <ironclad/ironclad.hpp>

TEST_CASE("ironclad version is set") {
    CHECK(ironclad::kVersion == 1);
    CHECK(ironclad::kDefaultTickHz >= 30);
    CHECK(ironclad::kDefaultRingSize >= 8);
}
