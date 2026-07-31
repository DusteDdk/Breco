# Building Breco for Windows (cross-compile on Linux)

This guide produces a portable x86_64 Windows ZIP from an Ubuntu host using the [MXE](https://mxe.cc/) MinGW-w64 shared Qt 6 toolchain.

## Supported target

- Windows 10 version 1809 or later, and Windows 11
- Regular files and directories only in this release
- Raw physical disks and elevated protected-file access are **not** available on Windows (Linux UDisks/pkexec behavior is unchanged)

## One-time host setup

Install MXE build prerequisites:

```bash
sudo ./install_win_crosscompile_dependencies_ubuntu.sh
```

## One-time MXE toolchain build

Bootstrap and build the pinned MXE toolchain (~1–3 hours):

```bash
./bootstrapMxe.sh
```

Or let `setMxe.sh` trigger bootstrap automatically on first use.

The pinned MXE commit is recorded in `bootstrapMxe.sh`. The toolchain installs to `~/mxe`.

## Build, test, and package

```bash
./build-for-windows-on-linux.sh
```

This script:

1. Sources `setMxe.sh` (sets `MXE_CMAKE`, `MXE_COPYDLLDEPS`, etc.)
2. Configures and builds Release binaries with Ninja
3. Runs the CTest suite under Wine with Qt's offscreen platform
4. Installs to `dist/breco-windows-x86_64/`
5. Stages runtime DLLs via MXE's `copydlldeps.py`
6. Copies required Qt `platforms/qwindows.dll` and `imageformats/*` plugins
7. Produces `dist/breco-<version>-win64.zip`

## Manual build steps

```bash
source ./setMxe.sh

"${MXE_CMAKE}" -S . -B build-windows-mxe -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-windows-mxe --parallel

export WINEPREFIX="${HOME}/.wine-breco"
export QT_QPA_PLATFORM=offscreen
export QT_PLUGIN_PATH="${MXE_ROOT}/usr/${MXE_TARGET}/qt6/plugins"
ctest --test-dir build-windows-mxe --output-on-failure
```

## Native Linux regression

After platform-aware CMake changes, verify the Linux build is unaffected:

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Qt LGPL compliance

The Windows package ships Qt 6 as dynamically linked DLLs. Distribute the corresponding Qt source code (or a written offer to provide it) alongside the application when releasing publicly.

## Smoke test on real Windows

After extracting the ZIP on Windows 10/11:

1. Run `breco.exe` — confirm the GUI starts
2. Open a regular file and a directory, run a scan
3. Verify text/bitmap previews and embedded image detection (PNG/JPEG plugins)
