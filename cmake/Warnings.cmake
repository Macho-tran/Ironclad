# Centralized warning configuration for first-party ironclad targets.
# Applied via `ironclad_apply_warnings(<target>)`.
function(ironclad_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
        )
        if(IRONCLAD_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        # MSVC: silence "unreferenced inline function" noise from doctest
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE
                -Wlogical-op
                -Wduplicated-cond
                -Wduplicated-branches
            )
        endif()
        if(IRONCLAD_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
