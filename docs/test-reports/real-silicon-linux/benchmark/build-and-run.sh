#!/bin/bash
# Build and run the ttkmd driver-level microbenchmark.
# Linux:  needs the tt-kmd test headers (ioctl.h) on the include path.
#   g++ -std=c++17 -O2 -I/path/to/tt-kmd/test ttkmd_bench.cpp -o ttkmd_bench
#   ./ttkmd_bench [/dev/tenstorrent/N] [dram_byte_addr]
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# ioctl.h lives in the tt-kmd test dir; adjust if your checkout is elsewhere.
INC="${TTKMD_TEST_DIR:-~/tt-kmd/test}"
g++ -std=c++17 -O2 -Wall -I"$INC" "$HERE/ttkmd_bench.cpp" -o "$HERE/ttkmd_bench"
"$HERE/ttkmd_bench" "$@"
