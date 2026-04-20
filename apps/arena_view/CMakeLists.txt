find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "arena_view: SDL2 not found; skipping")
    return()
endif()

add_executable(arena_view
    main.cpp
    render.cpp
    studio.cpp
    ../arena_demo/arena.cpp
)
target_include_directories(arena_view PRIVATE ${CMAKE_SOURCE_DIR}/apps)
target_link_libraries(arena_view PRIVATE
    ironclad
    SDL2::SDL2
    SDL2::SDL2main
)
ironclad_apply_warnings(arena_view)
target_compile_features(arena_view PRIVATE cxx_std_20)
