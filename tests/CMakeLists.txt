# Tier-0 kernel tests: standalone DES, no verilator. Every binary runs under
# ASan/UBSan so a teardown leak fails the suite (CONTRACT.md C12). Each t0_*.cpp
# is one test with its own main. t0_tasks_nosan repeats the task test without
# sanitizers at -O2: ASan blocks the symmetric-transfer tail call, and only the
# unsanitized build pins the full constant-stack chain depth (C10).

set(t0_warn -Wall -Wextra)
set(t0_sanitize -fsanitize=address,undefined)

file(GLOB t0_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/t0_*.cpp")

foreach(src IN LISTS t0_sources)
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE veroutines::veroutines)
    target_compile_options(${name} PRIVATE -O2 -g ${t0_warn} ${t0_sanitize})
    target_link_options(${name} PRIVATE ${t0_sanitize})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# Unsanitized deep-chain pin (C10): full symmetric-transfer depth at -O2.
add_executable(t0_tasks_nosan t0_tasks.cpp)
target_link_libraries(t0_tasks_nosan PRIVATE veroutines::veroutines)
target_compile_options(t0_tasks_nosan PRIVATE -O2 -g ${t0_warn})
add_test(NAME t0_tasks_nosan COMMAND t0_tasks_nosan)

add_subdirectory(t1)
