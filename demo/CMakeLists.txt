# Kernel demo: a micro token-ring readout, pure DES, no verilator.
# Run it: ./ring [mean_gap_ns] [sim_us] [hot_mult] [print_every]  (see ring.cpp).

add_executable(ring ring.cpp)
target_link_libraries(ring PRIVATE veroutines::veroutines)
target_compile_options(ring PRIVATE -O2 -g -Wall -Wextra)
