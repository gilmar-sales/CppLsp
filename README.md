# CppLsp

C++ project bootstrapped with CMake + GoogleTest + ccache, consuming
[Baldr](https://github.com/gilmar-sales/Baldr) (commit da `v0.1.0`) via `FetchContent`.
Structure mirrors [Freyr](https://github.com/gilmar-sales/Freyr).

## Requirements

- C++26 compiler (GCC 16+)
- CMake 3.29+
- Git (used by `FetchContent`)
- Ninja + ccache (recommended)

## Configure / Build / Test

```bash
cmake -G Ninja -S . -B build
cmake --build build --config Debug
ctest --test-dir build --build-config Debug --output-on-failure
./build/test/Tests_run
```

## Run

```bash
./build/cpplsp-server
curl http://localhost:8080/json
# { "message": "Hello, World!" }
```

The `examples/HelloWorld` target is the verbatim
[Baldr get-started](https://gilmar-sales.github.io/Baldr/get-started/) sample:

```bash
./build/examples/HelloWorld/HelloWorld
```

## Layout

```text
CMakeLists.txt          # root: ccache, FetchContent(baldr v0.1.0), lib + server + tests + examples
include/CppLsp/         # public headers
src/                    # library sources + main.cpp (cpplsp-server, Baldr Hello World)
examples/HelloWorld/    # standalone Baldr get-started sample
test/                   # GoogleTest suite (v1.17.0, mesma versao do Freyr)
```
