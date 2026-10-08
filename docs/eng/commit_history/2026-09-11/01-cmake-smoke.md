# 2026-09-11 CMake Smoke Test

> This record was not written at the time of commit `31e4240`; it was added retroactively on
> 2026-10-08, reconstructed only from that commit's diff. The diff does not show the intent or the
> verification at the time, so this record does not state them.

## Changes

- Added `CMakeLists.txt`. `cmake_minimum_required(VERSION 3.20)`, project name `Hamychi`, C++20 required
  (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`), one executable `Hamychi`
- Added `src/main.cpp`. It prints one line, `hello again!`, and returns 0
- The executable gets `include/` as an include path. The tree of this commit has no `include/` folder

The commit message is `test Cmake`.

## Afterwards

`789da5e` (Phase 1 build skeleton and Winsock2 initialization) deleted `src/main.cpp`, added
`client/src/main.cpp`, and rewrote the root `CMakeLists.txt`. `0195005` renamed the project from `hamychi`
to `sangtachi`.

## Verification

No build or run result from that time remains.

## Cross-model Review

There is no review record from that time.
