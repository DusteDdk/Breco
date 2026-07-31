#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  echo "Source this script instead: source ./setMxe.sh"
  exit 1
fi

BRECO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export MXE_ROOT="${HOME}/mxe"
export MXE_TARGET="x86_64-w64-mingw32.shared"

if [[ ! -d "${MXE_ROOT}" || -z "$(ls -A "${MXE_ROOT}" 2>/dev/null)" ]]; then
  "${BRECO_ROOT}/bootstrapMxe.sh" || return 1
fi

export PATH="${MXE_ROOT}/usr/bin:${MXE_ROOT}/usr/x86_64-pc-linux-gnu/bin:${PATH}"
export MXE_CMAKE="${MXE_ROOT}/usr/bin/${MXE_TARGET}-cmake"
export MXE_OBJDUMP="${MXE_ROOT}/usr/bin/${MXE_TARGET}-objdump"
export MXE_COPYDLLDEPS="${MXE_ROOT}/tools/copydlldeps.py"

if [[ ! -x "${MXE_CMAKE}" ]]; then
  echo "MXE CMake wrapper not found: ${MXE_CMAKE}"
  echo "The MXE toolchain may still be building. Check with: pgrep -af 'make.*MXE_TARGETS'"
  return 1
fi

MXE_QT6_CONFIG="${MXE_ROOT}/usr/${MXE_TARGET}/qt6/lib/cmake/Qt6/Qt6Config.cmake"
if [[ ! -f "${MXE_QT6_CONFIG}" ]]; then
  echo "MXE Qt6 is not built yet: ${MXE_QT6_CONFIG}"
  echo "Wait for bootstrapMxe.sh to finish (gcc + qt6 packages), then retry."
  return 1
fi
