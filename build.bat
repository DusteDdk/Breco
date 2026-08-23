@echo off
setlocal

cd /d "%~dp0"
set "BUILD_DIR=build-msvc-release"
set "BUILD_CONFIG=RelWithDebInfo"
set "CONAN_DIR=%BUILD_DIR%\conan"
set "CONAN_PROFILE=conan\profiles\windows-release.profile"

conan install . --profile:all="%CONAN_PROFILE%" --build=missing -of="%CONAN_DIR%"
if errorlevel 1 exit /b 1

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 ^
	-DCMAKE_TOOLCHAIN_FILE="%CONAN_DIR%\conan_toolchain.cmake" ^
	-DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
	-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO="Release;" ^
	-DBUILD_TESTING=ON
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --parallel 8
if errorlevel 1 exit /b 1

call "%CONAN_DIR%\conanrun.bat"
if errorlevel 1 exit /b 1
set "QT_QPA_PLATFORM=offscreen"
ctest --test-dir "%BUILD_DIR%" -C "%BUILD_CONFIG%" --output-on-failure
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --target dist --parallel 8
if errorlevel 1 exit /b 1

echo Distribution created in %CD%\dist
