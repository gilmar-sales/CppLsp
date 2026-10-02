# CppLsp

C++ project: a Biome/Oxlint-style engine with a standalone LSP server for VS Code.
Structure mirrors [Freyr](https://github.com/gilmar-sales/Freyr).

## Dependency policy

- **`cpplsp_core` (`core/`, `include/CppLsp/`): standard library only.** No third-party
  code, no exceptions, no RTTI. Enforced by `test/src/CorePolicySpec.cpp`.
- **Outside core, third parties are allowed**: [Skirnir](https://github.com/gilmar-sales/Skirnir)
  with its `simdjson` dependency for `compile_commands.json` and LSP JSON-RPC,
  GoogleTest `v1.17.0` for tests, and Google Benchmark `v1.9.5` for benches — via `FetchContent`.

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
./build/cpplsp parse src/main.cpp
./build/cpplsp parse --json --std c++23 src/main.cpp
./build/cpplsp lint --semantic --compile-commands build/compile_commands.json src
```

Files are analyzed concurrently with a standard-library `std::jthread` worker
set and an atomic work index; results are emitted in sorted path order. The
current formatter only normalizes brace-depth indentation. `check` exits 1 if
lint diagnostics are found; I/O or CLI errors exit 2.

`parse` dumps the grammar tree (node kinds with byte offsets) and reports
syntax errors as `CPPLSP900` diagnostics; it exits 1 when syntax errors are
found. `lint` and `check` also include `CPPLSP900` syntax diagnostics from the
same parser. The parser dialect comes from the matching `compile_commands.json`
entry (`-std=`/`/std:`) and its `-D` macros, defaulting to C++20; an explicit
`--std <c++20|c++23|c++26>` overrides the database.

## VS Code extension

The independent extension is in `vscode-extension/` and talks to `cpplsp-lsp` over
LSP JSON-RPC stdio:

```bash
cmake --build build --target cpplsp-lsp
cd vscode-extension
npm install
npm run compile
```

Open `vscode-extension/` in VS Code and press F5 to launch the Extension Development
Host. It provides C/C++ diagnostics, `NULL` quick fixes, and the current
brace-indent formatter. Set `cpplsp.serverPath` if the server is not found in the
workspace build directory or `PATH`. This replaces the lint/format portion of
Microsoft's extension; IntelliSense, debugging, and build integration are not
implemented yet.

## Layout

```text
CMakeLists.txt          # root: ccache, FetchContent(Skirnir/simdjson), core + CLI + LSP + tests + benches
core/                   # cpplsp_core: zero-dependency engine (MappedBuffer, Arena, LineTable, Lexer, Preprocessor, CST, rules, formatter)
include/CppLsp/         # core public headers (STL-only)
src/cli.cpp             # cpplsp lint/check/format CLI
lsp/main.cpp            # cpplsp-lsp: JSON-RPC over stdio
semantic/               # Skirnir JSON source + compile database reader + local type oracle
vscode-extension/       # VS Code extension manifest and LSP client
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
- **Compilation database / semantic seed**: `cpplsp_semantic` uses Skirnir's
  `JsonFileSource` (backed by `simdjson`) to read `compile_commands.json` (`arguments` or `command`) and
  extracts `-D`, `-U`, and `-I` options. Its local type oracle recognizes built-ins
  and declarations in the current file, so it can distinguish simple `A * b;` forms
  when `A` is known. It does not open include directories or implement full C++ name
  lookup; `--semantic` reports when a per-file compilation context is available.
