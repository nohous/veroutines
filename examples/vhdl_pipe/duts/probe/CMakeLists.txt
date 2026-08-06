# probe: the self-contained signal-survival DUT (register, comb net, record,
# sub-hierarchy). Also the pipe's smoke test - it runs to completion and traces.
add_vhdl_verilated_test(probe
    TOP probe
    VHDL "${CMAKE_CURRENT_SOURCE_DIR}/probe.vhd"
    TB   "${CMAKE_CURRENT_SOURCE_DIR}/probe_tb.cpp")
