# Tier-1 cosim interlock: veroutines against small behavioral SV modules under
# verilator --timing. Built only when verilator is discoverable; otherwise the
# tier-0 suite still stands on its own.

find_package(verilator QUIET)

if(NOT verilator_FOUND)
    message(STATUS "veroutines: verilator not found - tier-1 cosim tests disabled")
    return()
endif()

add_executable(t1_cosim t1_main.cpp)
target_link_libraries(t1_cosim PRIVATE veroutines::veroutines)
target_compile_features(t1_cosim PRIVATE cxx_std_23)
target_compile_options(t1_cosim PRIVATE -O2 -g)

verilate(t1_cosim
    SOURCES t1_top.sv
    TOP_MODULE t1_top
    VERILATOR_ARGS --timing --timescale 1ps/1fs)

add_test(NAME t1_cosim COMMAND t1_cosim)
