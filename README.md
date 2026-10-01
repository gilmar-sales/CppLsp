# CppLsp

C++ project: a Biome/Oxlint-style engine for C++ plus a Baldr-based server scaffold.
Structure mirrors [Freyr](https://github.com/gilmar-sales/Freyr).

## Dependency policy

- **`cpplsp_core` (`core/`, `include/CppLsp/`): standard library only.** No third-party
  code, no exceptions, no RTTI. Enforced by `test/src/CorePolicySpec.cpp`.
- **Outside core, third parties are allowed**: [Baldr](https://github.com/gilmar-sales/Baldr)
  (commit da `v0.1.0`) for the server, its `simdjson` target for `compile_commands.json`, GoogleTest `v1.17.0` for tests,
  Google Benchmark `v1.9.5` for benches — all via `FetchContent`.

## Requirements

- C++26 compiler (GCC 16+)
- CMake 3.29+
- Git (used by `FetchContent`)
- Ninja + ccache (recommended)

## Configure / Build / Test / Bench

```bash
cmake -G Ninja -S . -B build
cmake --build build --config Debug
ctest --test-dir build --build-config Debug --output-on-failure
./build/test/Tests_run
./build/bench/CoreBench
```

## CLI

```bash
./build/cpplsp lint --jobs 8 src include
./build/cpplsp lint --json src/main.cpp
./build/cpplsp lint --fix src/main.cpp
./build/cpplsp check src include
./build/cpplsp format src/main.cpp
./build/cpplsp format --write src include
./build/cpplsp lint --semantic --compile-commands build/compile_commands.json src
```

Files are analyzed concurrently with a standard-library `std::jthread` worker
set and an atomic work index; results are emitted in sorted path order. The
current formatter only normalizes brace-depth indentation. `check` exits 1 if
lint diagnostics are found; I/O or CLI errors exit 2.

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
CMakeLists.txt          # root: ccache, FetchContent(baldr), core + server + tests + examples + benches
core/                   # cpplsp_core: zero-dependency engine (MappedBuffer, Arena, LineTable, Lexer, Preprocessor, CST, rules, formatter)
include/CppLsp/         # core public headers (STL-only)
src/main.cpp            # cpplsp-server (Baldr Hello World)
src/cli.cpp             # cpplsp lint/check/format CLI
semantic/               # compile database reader (simdjson) + local type oracle
examples/HelloWorld/    # standalone Baldr get-started sample
test/                   # GoogleTest suite, incl. core dependency-policy guard
bench/                  # Google Benchmark suite (I/O, arena, line table, lexer, preprocessor, CST, rules, formatter) + corpus
```

## Implemented core stages

- **Lexer**: byte-lossless token spans with trivia, comments, identifiers, numbers,
  maximal-munch C++ punctuators and ordinary/raw literals; malformed literals/comments
  recover to EOF.
- **Preprocessor subset**: classifies and retains directive spans, evaluates nested
  `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif`, handles `#define` /
  `#undef` for object-like macros, and expands those macros outside comments/literals.
  Integer comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) are supported in `#if`.
  `#include` is deliberately opaque: no headers are opened. Function-like macros,
  token pasting/stringification and full C++ preprocessor expression semantics are
  not implemented yet; they are left unexpanded rather than guessed.
- **CST foundation**: iterative delimiter grouping for `()`, `[]`, `{}` over lossless
  lexer tokens. Green nodes are flat immutable-style spans linked by parent index;
  unmatched/mismatched delimiters produce diagnostics and recovery continues. This
  is structural scaffolding, not yet a C++ declaration/expression grammar.
- **Rule engine**: `CPPLSP001` replaces `NULL` with `nullptr` outside comments,
  literals and directives; `CPPLSP002` removes trailing spaces/tabs. Diagnostics
  carry byte ranges and 1-based line/column, with non-overlapping text edits applied
  from right to left. Rules can be enabled independently.
- **Local semantic rule**: `CPPLSP101` reports unused simple local variables when
  run with `--semantic --compile-commands ...`. It only covers basic declarations
  in function bodies (built-in/current-file types); complex declarators, parameters,
  captures, shadowing and include-provided types are outside the current subset.
- **Formatter foundation**: brace-depth indentation using lexer tokens, configurable
  spaces/tabs, preserving CRLF, blank lines, comments, literals, and preprocessor
  directives. It intentionally does not yet reflow lines or normalize operator spacing.
- **Compilation database / semantic seed**: `cpplsp_semantic` uses Baldr's existing
  `simdjson` target to read `compile_commands.json` (`arguments` or `command`) and
  extracts `-D`, `-U`, and `-I` options. Its local type oracle recognizes built-ins
  and declarations in the current file, so it can distinguish simple `A * b;` forms
  when `A` is known. It does not open include directories or implement full C++ name
  lookup; `--semantic` reports when a per-file compilation context is available.
