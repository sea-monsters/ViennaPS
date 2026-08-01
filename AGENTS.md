# Repository Guidelines

## Project Structure & Module Organization

ViennaPS is a header-only C++20 process and topography simulation library.
Public headers live in `include/viennaps`; `lib/` contains optional precompiled
specializations, and `gpu/` contains GPU support. CMake integration is under
`cmake/`, runnable C++/Python examples are under `examples/`, and automated
tests are under `tests/`. Python bindings, stubs, scripts, and Python tests
are under `python/`. Assets used by documentation and examples are in
`assets/`; CI workflows are in `.github/workflows/`.

## Build, Test, and Development Commands

Use an out-of-tree `build/` directory. Dependencies such as ViennaTools
components are fetched by CMake when needed.

```bash
cmake -S . -B build -DVIENNAPS_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure -E "Benchmark|Performance"
```

To build examples, add `-DVIENNAPS_BUILD_EXAMPLES=ON`. To build Python
bindings locally, use `python -m pip install .`; GPU builds require a matching
CUDA toolkit and driver. The CI formatting check is run with:
`cmake --build build --target format-check`.

## Coding Style & Naming Conventions

Follow the LLVM C++ coding guidelines and the repository’s `.clang-format`
configuration: two-space indentation, an 80-column preference, and no tabs.
Keep C++ source and example names in the existing lowerCamelCase style (for
example, `trenchOxidation.cpp`). Use descriptive `snake_case` names for Python
tests and scripts, such as `test_basic_functionality.py`. Preserve nearby
formatting and CMake conventions when editing build files.

## Testing Guidelines

CTest is the primary C++ test runner. Add C++ tests as focused subdirectories
under `tests/`, with a local `CMakeLists.txt`; Python tests belong under
`python/tests` or beside the relevant example/test fixture. Run the focused
test first, then the full non-benchmark CTest command above. No repository-wide
coverage threshold is currently documented.

## Commit & Pull Request Guidelines

This checkout has no local commit history, so its historical commit convention
cannot be verified. Use short, imperative subjects (for example, `Fix trench
oxidation boundary handling`) and keep each commit focused. Pull requests
should explain the motivation, affected modules, configuration or dependency
changes, and exact validation commands; link an issue when applicable and add
before/after images or generated-output comparisons for visual simulation
changes.
