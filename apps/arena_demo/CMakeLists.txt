add_executable(arena_demo
    main.cpp
    arena.cpp
    arena_udp.cpp
)
target_link_libraries(arena_demo PRIVATE ironclad Threads::Threads)
ironclad_apply_warnings(arena_demo)
