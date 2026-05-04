# mktorrent

A small C++ command-line tool that creates BitTorrent `.torrent` file and
magnet links using libtorrent-rasterbar.

## Features

- Creates `.torrent` files from a file or directory.
- Prints magnet links with `--magnet`; use it with `--output` to create both.
- Supports repeated trackers, web seeds, HTTP seeds, and DHT bootstrap nodes.
- Supports hybrid v1/v2 metadata by default, plus `--v1-only` and `--v2-only`.
- Builds on Linux and Windows.

## Examples

Create a torrent file:

```sh
./mktorrent ./payload -o payload.torrent -t udp://tracker.opentrackr.org:1337/announce
```

Create a torrent file and print a magnet link:

```sh
./mktorrent ./payload -o payload.torrent --magnet
```

Create only a magnet link:

```sh
./mktorrent ./payload --magnet
```

Show all options:

```sh
./mktorrent --help
```

## Dependencies

- CMake 3.20 or newer
- A C++17 compiler
- libtorrent-rasterbar 2.x

On Debian/Ubuntu-like Linux distributions:

```sh
sudo apt install cmake g++ libtorrent-rasterbar-dev pkg-config
```

On Windows with MSVC, install libtorrent via vcpkg:

```powershell
vcpkg install libtorrent:x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

If you install the static vcpkg triplet, use the same triplet when configuring:

```powershell
vcpkg install libtorrent:x64-windows-static
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## Build

Linux:

```sh
cmake -S . -B build
cmake --build build
```

Windows with MSVC and vcpkg:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

The vcpkg triplet used for `vcpkg install` must match the triplet used by CMake.

