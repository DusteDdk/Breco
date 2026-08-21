#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
source ./setMxe.sh

BUILD_DIR="build-windows-mxe"
GIT_SHA="$(git rev-parse --short HEAD)"
PACKAGING_DIR="${BUILD_DIR}/dist/breco-win-${GIT_SHA}"

MXE_BIN="${MXE_ROOT}/usr/${MXE_TARGET}/bin"
MXE_QT_BIN="${MXE_ROOT}/usr/${MXE_TARGET}/qt6/bin"
MXE_QT_PLUGINS="${MXE_ROOT}/usr/${MXE_TARGET}/qt6/plugins"

echo "==> Configuring Windows cross-build"
"${MXE_CMAKE}" -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON

echo "==> Building"
cmake --build "${BUILD_DIR}" --parallel

if command -v wine >/dev/null 2>&1; then
  echo "==> Running cross-target tests under Wine (failures do not block packaging)"
  export WINEPREFIX="${HOME}/.wine-breco"
  export WINEDEBUG=-all
  export QT_QPA_PLATFORM=offscreen
  export QT_PLUGIN_PATH="${MXE_QT_PLUGINS}"
  if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure; then
    echo "==> Wine/CTest reported failures; continuing with packaging" >&2
  fi
else
  echo "==> Skipping Wine tests (wine not installed)"
fi

echo "==> Packaging portable Windows directory: ${PACKAGING_DIR}"
rm -rf "${PACKAGING_DIR}"
mkdir -p "${PACKAGING_DIR}/platforms" "${PACKAGING_DIR}/imageformats"

cp "${BUILD_DIR}/breco.exe" "${BUILD_DIR}/brecodump.exe" "${PACKAGING_DIR}/"

echo "==> Resolving and copying runtime DLL dependencies (MXE copydlldeps)"
python3 "${MXE_COPYDLLDEPS}" \
  -C "${PACKAGING_DIR}" \
  -L "${MXE_BIN}" "${MXE_QT_BIN}" \
  -- "${PACKAGING_DIR}"

echo "==> Copying required Qt platform and image-format plugins"
cp "${MXE_QT_PLUGINS}/platforms/qwindows.dll" "${PACKAGING_DIR}/platforms/"
cp "${MXE_QT_PLUGINS}/imageformats/"*.dll "${PACKAGING_DIR}/imageformats/"

echo "==> Copying user documentation and examples"
mkdir -p "${PACKAGING_DIR}/docs" "${PACKAGING_DIR}/examples"
cp README.md "${PACKAGING_DIR}/docs/"
cp docs/BrecoLang.md "${PACKAGING_DIR}/docs/"
cp examples/png.breco "${PACKAGING_DIR}/examples/"

required_files=(
  "breco.exe"
  "brecodump.exe"
  "platforms/qwindows.dll"
  "docs/README.md"
  "docs/BrecoLang.md"
  "examples/png.breco"
)
for required in "${required_files[@]}"; do
  if [[ ! -f "${PACKAGING_DIR}/${required}" ]]; then
    echo "Required packaging artifact missing: ${PACKAGING_DIR}/${required}" >&2
    exit 1
  fi
done

echo "Portable Windows package ready: ${PACKAGING_DIR}"
echo "Copy this directory to Windows and run breco.exe or brecodump.exe from it."
echo "User docs: docs/README.md (app guide), docs/BrecoLang.md (language reference)."
