# Erase the console `Loading...` when preparation ends

## Why

The repository owner saw this in the console. The startup notice `Loading...` stayed on screen after preparation ended and the lobby guide
appeared. It was written as a line of its own, ending the line.

## What changed

| What | Where |
|------|-------|
| `Loading...` does not end the line. It is erased before the next line or the input prompt appears in its place. It is now shown right after the log file is opened instead of right after the argument check | `architecture.md` 3.5 Lobby guide |
| The screen side gains `loading`. It is called right after the log sink is attached. The GUI does nothing | `client_process.hpp`, `client_process.cpp`, `gui_ui.hpp` |
| The input prompt gains `status` and `clear_status`. `line`, `show` and `close` erase it first | `console.hpp`, `console.cpp` |
| The console screen writes `Loading...` with `status`. It holds the input prompt in a `shared_ptr` shared with the log screen function. The startup failure line on standard error, the notice about files next to the executable and the startup failure window notice also erase it first | `main.cpp` |

- Erasing is `\r`, as many spaces as characters, `\r`. No terminal control sequences such as cursor movement are used
- Only `clear_status` may be called from another thread, because the standard error log screen calls it. It takes the width with an atomic exchange so that exactly one caller erases
- When standard output is redirected, `Loading...` is not written, as before

## Verification

| What | Result |
|------|--------|
| One case in `console_test` | The first output after `status` erases it exactly once |
| Real bytes in a pseudo terminal (`script`) on `mere-galaxy` | The lobby guide comes after `Loading...\r          \r`. Started to use the adapter without privilege, it is `Loading...\r          \rERROR adapter.error reason=not_elevated` |
| Windows `scripts/test.ps1` | `all tests passed`. 665 ctest cases |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## Cross-model review

Codex (`gpt-6.1-sol`). A correctness and threads lens and a document consistency lens in one run. One blocker, two warns. Only the repair diff was run again with a regression lens, and it was LGTM.

| Finding | Disposition |
|---------|-------------|
| blocker: the log sink lives for the process, but the screen function holds the console screen object on the stack. A log after `main` touches freed memory | Accepted. It broke the contract of `ClientUi::log_screen` ("does not hold this object"). The input prompt is held in a `shared_ptr` and the screen function keeps a copy |
| warn: an `ERROR` for failing to open the record file or the log file comes before the sink is attached, so it is still appended to `Loading...` | Accepted. `Loading...` is shown right after the log file is opened. Failures before that come before `Loading...` appears |
| warn: the atomic exchange claims the erase before writing, so another thread's `ERROR` can come out before the erase | Rejected. `ERROR` lines during startup come from the same thread as the screen side. They overlap only when another thread (`[control]`) writes an `ERROR` in that short gap, and even then only the screen layout is off. Locking all output costs more |
