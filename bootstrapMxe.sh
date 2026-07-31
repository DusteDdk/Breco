#!/usr/bin/env bash
set -euo pipefail

MXE_ROOT="${HOME}/mxe"
MXE_TARGET="x86_64-w64-mingw32.shared"
MXE_COMMIT="221403b640229649ae397718ff2757347fe1385e"

if [[ -d "${MXE_ROOT}" && -n "$(ls -A "${MXE_ROOT}")" ]]; then
  echo "${MXE_ROOT} is not empty; leaving it alone."
  exit 0
fi

mkdir -p "${MXE_ROOT}"
git clone https://github.com/mxe/mxe.git "${MXE_ROOT}"

cd "${MXE_ROOT}"
git checkout "${MXE_COMMIT}"
make -j"$(nproc)" MXE_TARGETS="${MXE_TARGET}" \
  cc \
  cmake \
  qt6 \
  qt6-qtimageformats \
  qt6-qtsvg
