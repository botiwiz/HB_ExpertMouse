#!/bin/sh
# Builds the hb_viewer binary into build/. Needs: g++, pkg-config, libgtk-3-dev.
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pthread -Ithird_party \
    -o build/hb_viewer src/*.cpp $(pkg-config --cflags --libs gtk+-3.0)
