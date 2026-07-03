# KratosRTC - Building Instructions

KratosRTC uses CMake and builds the vendored dependencies in this repository. When `ENABLE_QUIC=ON`, it also needs BoringSSL for `lsquic`.

The default build downloads a pinned BoringSSL commit with CMake `FetchContent`, so clean machines and CI use the same crypto dependency revision.

## Requirements

- CMake 3.16+
- A C++17 compiler
- Perl, required by `lsquic`
- Go, required by BoringSSL
- zlib development headers

Linux example:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build perl golang zlib1g-dev
```

macOS example:

```bash
brew install cmake ninja go
```

## Standard QUIC Build

```bash
cmake -S . -B build -G Ninja -DENABLE_QUIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The first configure/build will fetch and compile the pinned BoringSSL revision.

## Use A Local BoringSSL

Use this when building offline or when reusing an existing BoringSSL checkout:

```bash
cmake -S . -B build -G Ninja \
  -DENABLE_QUIC=ON \
  -DBORINGSSL_DIR=/path/to/boringssl \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

If `BORINGSSL_DIR` points to BoringSSL sources, KratosRTC builds it with `add_subdirectory`. If it points to a prebuilt tree, also pass `BORINGSSL_LIB` when the libraries are not under the usual `ssl` and `crypto` subdirectories:

```bash
cmake -S . -B build -G Ninja \
  -DENABLE_QUIC=ON \
  -DKRATOSRTC_BORINGSSL_BUILD_FROM_SOURCE=OFF \
  -DBORINGSSL_DIR=/path/to/boringssl \
  -DBORINGSSL_LIB=/path/to/boringssl/build \
  -DCMAKE_BUILD_TYPE=Release
```

## Non-QUIC Build

```bash
cmake -S . -B build -G Ninja -DENABLE_QUIC=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Useful Options

- `ENABLE_QUIC=ON`: enable QUIC transport support.
- `KRATOSRTC_FETCH_BORINGSSL=ON`: fetch the pinned BoringSSL revision automatically.
- `KRATOSRTC_BORINGSSL_GIT_TAG=<commit>`: override the pinned BoringSSL commit for experiments.
- `BORINGSSL_DIR=/path/to/boringssl`: use a local BoringSSL source or build directory.
- `NO_TESTS=ON`: skip test targets for faster SDK builds.
- `NO_EXAMPLES=ON`: skip example targets.
- `NO_MEDIA=ON`: build a smaller DataChannel-focused SDK.
- `NO_WEBSOCKET=ON`: build without WebSocket support.

## CI Baseline

The GitHub Actions build uses the same baseline command:

```bash
cmake -S . -B build -G Ninja -DENABLE_QUIC=ON -DNO_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```
