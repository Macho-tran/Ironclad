# Each test_*.cpp becomes its own doctest binary. This is slower to link
# than one big binary, but gives us per-subsystem isolation in CTest.
function(ironclad_add_test name)
    add_executable(${name} ${name}.cpp doctest_main.cpp)
    target_link_libraries(${name} PRIVATE
        ironclad
        ironclad_doctest
    )
    ironclad_apply_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 60
        LABELS "unit"
    )
endfunction()

# Empty initial test so `ctest` is non-trivial during scaffolding.
add_executable(test_smoke test_smoke.cpp doctest_main.cpp)
target_link_libraries(test_smoke PRIVATE ironclad ironclad_doctest)
ironclad_apply_warnings(test_smoke)
add_test(NAME test_smoke COMMAND test_smoke)
set_tests_properties(test_smoke PROPERTIES TIMEOUT 30 LABELS "unit")

# Subsystem unit tests, registered via the helper.
ironclad_add_test(test_fixed)
ironclad_add_test(test_vec2)
ironclad_add_test(test_rng)
ironclad_add_test(test_hash)
ironclad_add_test(test_ecs)
ironclad_add_test(test_snapshot)
ironclad_add_test(test_delta)
ironclad_add_test(test_range_coder)
ironclad_add_test(test_netsim)
ironclad_add_test(test_session_rollback)
ironclad_add_test(test_lag_comp)
ironclad_add_test(test_recorder)
ironclad_add_test(test_desync)

# Soak smoke: 60s, 4 AI clients, simulated 150ms RTT / 5% loss / 30ms
# jitter / 2% reorder. Asserts 0 desyncs + bounded rollback + bandwidth
# under the KPI cap. Tagged "soak" so CI can run it as a separate step.
add_executable(test_soak_smoke
    test_soak_smoke.cpp
    doctest_main.cpp
    ../apps/arena_demo/arena.cpp)
target_link_libraries(test_soak_smoke PRIVATE ironclad ironclad_doctest)
target_include_directories(test_soak_smoke PRIVATE ${CMAKE_SOURCE_DIR})
ironclad_apply_warnings(test_soak_smoke)
add_test(NAME test_soak_smoke COMMAND test_soak_smoke)
set_tests_properties(test_soak_smoke PROPERTIES
    TIMEOUT 180
    LABELS  "soak")
