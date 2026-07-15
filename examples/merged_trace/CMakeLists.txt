# Full-hierarchy cosim trace: verilator's own tracer (--trace) dumps the whole
# model, veroutines' wave dumps the C++ side, scripts/merge_vcd.py merges them.

add_executable(merged_trace main.cpp)
target_link_libraries(merged_trace PRIVATE veroutines::veroutines)
target_compile_features(merged_trace PRIVATE cxx_std_23)
target_compile_options(merged_trace PRIVATE -O2 -g)

verilate(merged_trace
    SOURCES ../uart_link/uart_tx.sv
    TOP_MODULE uart_tx
    TRACE
    VERILATOR_ARGS -O2)

add_test(NAME merged_trace COMMAND merged_trace)
set_tests_properties(merged_trace PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Pin the merger: merge the two VCDs and check the result carries both an RTL
# internal (shifter) and a C++ signal (seen).
find_program(PYTHON3 python3)
if(PYTHON3)
    add_test(NAME merged_trace_merge
        COMMAND ${CMAKE_COMMAND}
            -DPYTHON=${PYTHON3}
            -DMERGE=${CMAKE_SOURCE_DIR}/scripts/merge_vcd.py
            -DDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_merge.cmake)
    set_tests_properties(merged_trace_merge PROPERTIES DEPENDS merged_trace)
endif()
