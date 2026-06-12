#!/bin/bash
set -e
cd "$(dirname "$0")"

# Prefer the Ninja generator for NEW build directories (faster dependency
# scanning and job scheduling than Make). An existing build/ keeps whatever
# generator it was created with: switching generators requires wiping the
# directory, which would force a full re-download and rebuild of every
# dependency.
GENERATOR_ARGS=()
if [ ! -f build/CMakeCache.txt ] && command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

# Cap parallelism at the physical core count: an unbounded `make -j` can
# oversubscribe memory with heavy C++ translation units.
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"

cmake -B build -DCMAKE_BUILD_TYPE=Debug "${GENERATOR_ARGS[@]}"
cmake --build build --parallel "$JOBS"
echo ""
echo "Build complete. Run:"
echo "  ./build/Sonik_artefacts/Debug/Sonik.app/Contents/MacOS/Sonik"
