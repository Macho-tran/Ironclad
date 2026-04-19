# Apps subdirectory. arena_demo is the headless soak/demo harness; it is
# always built when IRONCLAD_BUILD_DEMO is on. arena_view is the optional
# SDL2 visual frontend.
if(IRONCLAD_BUILD_DEMO)
    add_subdirectory(arena_demo)
endif()

if(IRONCLAD_BUILD_SDL_DEMO)
    add_subdirectory(arena_view)
endif()
