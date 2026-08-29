#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
podman run --rm -v "$ROOT":/src:Z -w /src docker.io/library/fedora:44 bash -lc '
  set -euo pipefail
  dnf install -y mingw64-gcc-c++ mingw64-winpthreads-static cmake make zip >/tmp/dnf.log
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake
  cmake --build build -j"$(nproc)"
  ls -la build/dinput8.dll build/sitcom/sounds | head
  rm -rf /tmp/ds3_sitcom_pkg
  mkdir -p /tmp/ds3_sitcom_pkg/sitcom/sounds
  cp build/dinput8.dll /tmp/ds3_sitcom_pkg/
  cp sitcom/config.ini /tmp/ds3_sitcom_pkg/sitcom/
  cp sitcom/chainload.txt /tmp/ds3_sitcom_pkg/sitcom/
  cp sitcom/sounds/* /tmp/ds3_sitcom_pkg/sitcom/sounds/
  mkdir -p dist
  rm -f dist/DS3_Sitcom.zip
  (cd /tmp/ds3_sitcom_pkg && zip -r /src/dist/DS3_Sitcom.zip dinput8.dll sitcom)
  unzip -l /src/dist/DS3_Sitcom.zip
'
