#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
arrow_dir=$(python3 -c 'import pyarrow; print(pyarrow.get_library_dirs()[0])')
mkdir -p build
"${CXX:-g++}" -O3 -DNDEBUG -std=c++20 -pthread -Wall -Wextra -Wpedantic ${EXTRA_CXXFLAGS:-} \
    -isystem "$arrow_dir/include" src/main.cpp src/output.cpp -L"$arrow_dir" \
    -Wl,-rpath,'$ORIGIN' -Wl,-rpath,"$arrow_dir" \
    -l:libparquet.so.2200 -l:libarrow.so.2200 -o build/shopping-sessions
