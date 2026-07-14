# UART link: behavioral RTL transmitter + C++ oversampling receiver. Clocked
# RTL with no delays, so no --timing is needed; the C++ side drives the clock.

add_executable(uart_link main.cpp)
target_link_libraries(uart_link PRIVATE veroutines::veroutines)
target_compile_features(uart_link PRIVATE cxx_std_23)
target_compile_options(uart_link PRIVATE -O2 -g)

verilate(uart_link
    SOURCES uart_tx.sv
    TOP_MODULE uart_tx)

add_test(NAME uart_link COMMAND uart_link)
