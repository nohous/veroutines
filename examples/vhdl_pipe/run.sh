#!/bin/bash
# Put the oss-cad-suite tools (ghdl, verilator) on PATH, then configure, build,
# and run the tests via CMake + ctest. Extra args pass to ctest, so
# `./run.sh -R queues` runs one test and `./run.sh -V` runs verbose.
set -euo pipefail

source /etc/profile.d/modules.sh
module load oss-cad-suite

cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
ln -sf build/compile_commands.json compile_commands.json   # for clangd
cmake --build build -j
ctest --test-dir build --output-on-failure "$@"
