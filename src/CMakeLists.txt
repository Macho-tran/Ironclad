# Public headers live in include/ironclad ; we only list .cpp here.
add_library(ironclad STATIC
    placeholder.cpp
)

target_include_directories(ironclad
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ironclad
    PUBLIC
        ironclad_xxhash
)

ironclad_apply_warnings(ironclad)

set_target_properties(ironclad PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBLE_INLINES_HIDDEN ON
)
