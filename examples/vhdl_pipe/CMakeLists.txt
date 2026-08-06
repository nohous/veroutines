# vhdl_pipe: bring a VHDL DUT into veroutines - ghdl synth (no yosys) -> verilator
# -> a veroutines C++ testbench - as one CMake build. GhdlSynth.cmake makes the
# synthesized netlist a first-class build dependency; each DUT is a subdirectory
# declaring one add_vhdl_verilated_test(). Standalone project under
# veroutines/examples, built on its own (its synth step precedes verilate), not
# as part of the veroutines example build.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#   (run.sh does this after loading the oss-cad-suite tools.)

cmake_minimum_required(VERSION 3.20)
project(vhdl_pipe CXX)

# Emit build/compile_commands.json for clangd (Vdut.h and the veroutines include
# path resolve only through it). run.sh symlinks it to the project root.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# find_package(verilator) reads VERILATOR_ROOT; opt in to the modern lookup so
# the env/cache both-set case does not warn.
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

# Default to the veroutines repo two levels up (examples/vhdl_pipe -> root);
# overridable to point the pipe at another checkout.
get_filename_component(_veroutines_default "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VEROUTINES_ROOT "${_veroutines_default}"
    CACHE PATH "veroutines source tree")

set(VEROUTINES_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(VEROUTINES_BUILD_DEMO     OFF CACHE BOOL "" FORCE)
set(VEROUTINES_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${VEROUTINES_ROOT}" veroutines-build EXCLUDE_FROM_ALL)

find_package(verilator REQUIRED)
find_program(GHDL ghdl REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GhdlSynth)

enable_testing()

# probe ships with the repo; advacam DUTs are present only in a local checkout.
add_subdirectory(duts/probe)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/duts/queues/CMakeLists.txt")
    add_subdirectory(duts/queues)
endif()
