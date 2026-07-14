# Cross-boundary combinational settle (CONTRACT.md C3/C5/C8). Pure comb RTL, so
# no --timing is needed - the model just re-evaluates each delta round.

add_executable(comb_partition main.cpp)
target_link_libraries(comb_partition PRIVATE veroutines::veroutines)
target_compile_features(comb_partition PRIVATE cxx_std_23)
target_compile_options(comb_partition PRIVATE -O2 -g)

verilate(comb_partition
    SOURCES comb_slave.sv
    TOP_MODULE comb_slave)

add_test(NAME comb_partition COMMAND comb_partition)
