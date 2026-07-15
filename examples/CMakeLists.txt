# Illustrative cosim examples - a verilated behavioral model paired with a C++
# behavioral model. Built only when verilator is discoverable.

find_package(verilator QUIET)

if(NOT verilator_FOUND)
    message(STATUS "veroutines: verilator not found - examples disabled")
    return()
endif()

add_subdirectory(comb_partition)
add_subdirectory(uart_link)
add_subdirectory(merged_trace)
