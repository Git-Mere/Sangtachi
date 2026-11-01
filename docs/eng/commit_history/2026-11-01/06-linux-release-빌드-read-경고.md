# Fix the `read` warning in the Linux Release build

## Why

To run the two-machine Minecraft field test at home, HEAD was built as Release on `mere-galaxy`. The build failed in `wait_for_enter` of
`client/src/platform/posix/console_ctrl.cpp`.

```text
error: ignoring return value of 'ssize_t read(int, void*, size_t)' declared with attribute 'warn_unused_result' [-Werror=unused-result]
```

Ubuntu's g++ 15 defines `_FORTIFY_SOURCE=3` by default when optimization is on (checked with `g++ -O3 -dM -E`). glibc's `read` then carries
`warn_unused_result`, and a `(void)` cast does not silence that attribute. It went unnoticed because only Debug had been built so far
([`linux-prereq.md`](../../linux-prereq.md) section 1 said so).

## What changed

| What | Where |
|------|-------|
| The return value of `read` is taken into a variable and discarded | `client/src/platform/posix/console_ctrl.cpp` |
| The Release configuration is recorded as built on Ubuntu 26.04. It was not run on 24.04 | `linux-prereq.md` section 1 |

Behavior does not change. Neither the bytes read nor a failure is used, as before.

## Verification

| What | Result |
|------|--------|
| `mere-galaxy` Release build (`-DSANGTACHI_BUILD_TESTS=OFF`) | Failed before the fix; passes without warnings after it |
| `mere-galaxy` Debug build and `ctest` | 75 cases, 0 failed. One adapter test was skipped for lack of privilege. `sudo ctest -R linux_tun` passed 4 cases including it |
| Windows | The changed file is not part of the Windows build. `scripts/test.ps1` passed |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). The correctness (A, including whether other `(void)` calls of the same shape exist in `posix/` and `linux/`) and docs/record (B) lenses ran in one pass. The result was `LGTM - no blockers`. The final diff after filling in this section was checked once more.
