# Building from source

UFS2Xplorer is a C++20 project. The core libraries (crypto, disk, filesystem, pkg, license) are deliberately Qt free and depend only on the standard library and OpenSSL, so they build fast and are unit tested without a display. Only the UI links Qt.

## Requirements

- A C++20 compiler (MSVC 2022 on Windows)
- CMake 3.25 or newer, and Ninja
- Qt 6 (Core, Widgets, Network), OpenSSL, and Catch2 3, supplied via vcpkg

Install the dependencies into vcpkg once (classic mode):

```
vcpkg install qtbase openssl catch2 --triplet x64-windows
```

## Windows (easy way)

A build script wraps the whole thing. From the project root:

```
build.cmd            build (incremental)
build.cmd -Test      build and run the tests
build.cmd -Run       build and launch the app
build.cmd -Clean     wipe the build directory and rebuild from scratch
build.cmd -Config Debug
build.cmd -Target ps3hdd_pkg_info
```

It finds Visual Studio and Ninja itself and configures the build directory on first run. The finished exe is `build/rel/bin/UFS2Xplorer.exe`.

## Manual configure

If you would rather run CMake directly, from a Visual Studio x64 developer prompt:

```
cmake -S . -B build/rel -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DPS3HDD_BUILD_UI=ON -DPS3HDD_BUILD_TESTS=ON ^
  -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build/rel
ctest --test-dir build/rel --output-on-failure
```

`VCPKG_MANIFEST_MODE=OFF` matters: the repo has a `vcpkg.json`, so without it CMake goes into manifest mode and tries to rebuild Qt from source. Classic mode reuses the packages you already installed above.

## Core only (no Qt)

To build just the Qt free core and its tests, leave `-DPS3HDD_BUILD_UI=OFF`. Only OpenSSL and Catch2 are needed.

## Layout

```
src/ps3hdd_crypto/      AES-XTS/CBC/CTR, SHA1-XOR, big-endian helpers
src/ps3hdd_disk/        raw disk access, disk sources
src/ps3hdd_cryptodisk/  decrypting disk views (XTS / CBC)
src/ps3hdd_app/         GameOS partition discovery and mount
src/ps3hdd_fs/          UFS2 reader, writer, consistency checker
src/ps3hdd_pkg/         PKG parse and install
src/ps3hdd_license/     RAP to RIF, act.dat, ECDSA
src/ps3hdd_mms/         XMB metadata database (work in progress)
src/ps3hdd_ipc/         elevated disk broker (client and server)
src/ps3hdd_ui/          Qt application
tools/                  command line utilities and diagnostics
tests/                  Catch2 tests
docs/                   this folder
```