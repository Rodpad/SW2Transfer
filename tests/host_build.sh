#!/bin/sh
# Builds the servers for the Mac (with AddressSanitizer) into build-host/.
cd "$(dirname "$0")/.." && mkdir -p build-host && \
clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -o build-host/sw2transfer-host \
  tests/host_main.cpp source/net.cpp source/vfs.cpp source/http.cpp source/ops.cpp
