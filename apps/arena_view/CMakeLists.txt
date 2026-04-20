find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "arena_view: SDL2 not found; skipping")
    return()
endif()

# Optional ImGui — fetched at configure time. We try v1.90.0 (the
# latest cmake-friendly tag at time of writing). If the fetch fails
# (e.g. offline build), fall back to the line-segment renderer; the
# studio still works perfectly without ImGui.
set(IRONCLAD_HAS_IMGUI 0)
if(IRONCLAD_USE_IMGUI)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90
        GIT_SHALLOW    TRUE)
    # Use the lower-level Populate API so we can detect failure
    # gracefully instead of erroring out the whole CMake run.
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()
    if(imgui_POPULATED)
        set(IRONCLAD_HAS_IMGUI 1)
        # ImGui doesn't ship a CMakeLists; build it as a small static
        # lib here. Includes: core, sdl2 backend, sdl_renderer backend.
        add_library(ironclad_imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
        )
        target_include_directories(ironclad_imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends)
        target_link_libraries(ironclad_imgui PUBLIC SDL2::SDL2)
        # ImGui isn't ours; suppress our strict warnings on it.
        if(NOT MSVC)
            target_compile_options(ironclad_imgui PRIVATE -w)
        else()
            target_compile_options(ironclad_imgui PRIVATE /w)
        endif()
        message(STATUS "arena_view: ImGui ${imgui_VERSION} fetched and built")
    else()
        message(WARNING "arena_view: ImGui FetchContent failed; falling back to line-segment renderer")
    endif()
endif()

add_executable(arena_view
    main.cpp
    render.cpp
    studio.cpp
    ../arena_demo/arena.cpp
)
target_include_directories(arena_view PRIVATE ${CMAKE_SOURCE_DIR}/apps)
if(IRONCLAD_HAS_IMGUI)
    target_compile_definitions(arena_view PRIVATE IRONCLAD_HAS_IMGUI=1)
    target_link_libraries(arena_view PRIVATE ironclad_imgui)
endif()
target_link_libraries(arena_view PRIVATE
    ironclad
    SDL2::SDL2
    SDL2::SDL2main
)
ironclad_apply_warnings(arena_view)
target_compile_features(arena_view PRIVATE cxx_std_20)

if(IRONCLAD_HAS_IMGUI)
    # ImGui's headers contain conversions our strict flags reject;
    # disable the noisiest ones for arena_view only.
    target_compile_options(arena_view PRIVATE
        -Wno-conversion -Wno-sign-conversion
        -Wno-double-promotion -Wno-cast-align
        -Wno-old-style-cast -Wno-shadow)
endif()
