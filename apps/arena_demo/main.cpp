// SPDX-License-Identifier: MIT
// arena_demo: headless 4-AI driver that doubles as the soak harness.
// During scaffolding this is just a stub that prints a banner; the real
// loop is wired up once Session is in place.
#include <cstdio>

#include <ironclad/ironclad.hpp>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::printf("ironclad arena_demo (scaffold) v%u\n",
                static_cast<unsigned>(ironclad::kVersion));
    return 0;
}
