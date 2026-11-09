#!/bin/sh
# Build HermesVita.vpk with the vitasdk Docker image (nothing to install but Docker).
#   scripts/build-vpk.sh            -> dist/HermesVita.vpk
set -e
cd "$(dirname "$0")/.."
rm -rf .build && mkdir -p .build dist
docker run --rm -v "$PWD/app:/src:ro" -v "$PWD/.build:/out" -w /out vitasdk/vitasdk:2026.08 sh -c \
  'cmake -S /src -B /out/b -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake >/dev/null && cmake --build /out/b 2>&1 | grep -E " error|undefined|Built target .*vpk"'
cp .build/b/hermes_vita.vpk dist/HermesVita.vpk; cp .build/b/build_id.txt dist/HermesVita.build
ls -l dist/HermesVita.vpk
