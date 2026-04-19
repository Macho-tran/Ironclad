add_executable(arena_demo
    main.cpp
    arena.cpp
)
target_link_libraries(arena_demo PRIVATE ironclad)
ironclad_apply_warnings(arena_demo)
