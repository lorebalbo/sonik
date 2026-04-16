#!/bin/bash
set -e
cd "$(dirname "$0")"
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
echo ""
echo "Build complete. Run:"
echo "  ./build/Sonik_artefacts/Debug/Sonik.app/Contents/MacOS/Sonik"
