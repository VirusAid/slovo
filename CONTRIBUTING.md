# Contributing to slovo

Thanks for your interest!

## Ground rules

- The library stays **single-header** and **zero-dependency** by default.
  New transports or integrations must be opt-in behind a macro (like
  `SLOVO_USE_CURL`).
- C++17, no compiler extensions. CI builds with GCC, Clang and MSVC at
  `-Wall -Wextra` / `/W4` — warnings are treated as bugs.
- Every change needs tests in `tests/test_slovo.cpp`. Network-dependent
  behavior is tested through a scripted `slovo::Transport`, never against
  live APIs.

## Workflow

1. Fork, branch from `main`.
2. `cmake -B build && cmake --build build && ctest --test-dir build`
3. Open a PR with a clear description of the problem and the fix.

## Reporting bugs

Open an issue with your platform, compiler, the provider/endpoint you're
talking to, and a minimal snippet. For streaming bugs, a raw SSE capture
helps enormously.
