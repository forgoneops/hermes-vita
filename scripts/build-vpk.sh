#!/bin/sh
# Build Hermes_Pocket.vpk with the vitasdk Docker image (nothing to install but Docker).
#   scripts/build-vpk.sh            -> dist/Hermes_Pocket.vpk
set -e
cd "$(dirname "$0")/.."
rm -rf .build && mkdir -p .build dist
docker run --rm -v "$PWD/app:/src:ro" -v "$PWD/.build:/out" -w /out vitasdk/vitasdk:2026.08 sh -c \
  'cmake -S /src -B /out/b -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake >/dev/null && cmake --build /out/b 2>&1 | grep -E " error|undefined|Built target .*vpk"'
cp .build/b/hermes_pocket.vpk dist/Hermes_Pocket.vpk; cp .build/b/build_id.txt dist/Hermes_Pocket.build
ls -l dist/Hermes_Pocket.vpk
