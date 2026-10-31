# 0002. Normal shutdown on Linux was blocked by standard input

- Status: Resolved
- Found: 2026-10-31
- Related: [ADR 0035](../decisions/0035-linux-console-client.md), [`../concurrency.md`](../concurrency.md) chapter 7 Shutdown,
  [`../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md`](../commit_history/2026-10-31/02-linux-콘솔-클라이언트.md)

## Symptom

When a shutdown signal (`SIGINT`) was sent to the Linux console client, cleanup finished up to `adapter.close`. But the process did not end. It ended only when a line
arrived on standard input or standard input was closed. In a terminal this means pressing Ctrl+C does not end it until Enter is pressed.

It was reproduced on `mere-galaxy` with standard input held open as a pipe.

- Two threads remained after the signal. The main thread was stuck in `futex_do_wait`, and `[console]` in a pipe read (`anon_pipe_read`)
- When the pipe was closed, it ended 23.9 seconds after the signal

## Investigation

| Suspected | Seen | Judgement |
|-----------|-------|------|
| Something joins `[console]` | `[console]` is a detached thread (`main.cpp`). The design does not join it either (`concurrency.md` chapter 7 Shutdown) | No |
| `exit` collides with a stdio read | Measured with a small program without the client. `main` returns while another thread is reading standard input. Standard input is a pipe kept open for 4 seconds | Yes. Table below |

| Read in the other thread | Until the process ends |
|--------------------|----------------------|
| `std::getline(std::cin, line)` | 4006ms (when the pipe closes) |
| `fgets(stdin)` | 4006ms |
| `read(0)` | 308ms (right after the 300ms wait in `main`) |

## Cause

**Confirmed.** While a thread reading through stdio is blocked, `exit` does not finish. Reading the same place with `read(0)` lets it finish. Seen on glibc 2.43
(Ubuntu 26.04). This symptom has never been seen on Windows.

**Inferred.** The blocked stdio read stays holding the stdin lock, and `exit` waits for that lock while flushing the stdio streams. That matches the main thread
waiting in `futex`. The glibc source was not looked at.

## Handling

Reading one line of standard input was moved into the platform function `platform::read_stdin_line`.

- The Windows version is the same `std::getline(std::cin, line)` as before
- The Linux version reads with `read(0)` without going through stdio and splits lines. The meaning is the same as `std::getline`
- The rule is written in the `[console]` item of `concurrency.md` chapter 7 Shutdown

The option of ending with `std::_Exit` after cleanup was rejected. It would change the Windows shutdown path too.

The cross review pointed out one more thing. The static buffer the Linux version keeps leftover bytes in is destroyed at `exit`, and the detached `[console]` may still
be reading at that time. It was fixed so that the buffer is not destroyed.

## Result

- Two tests were added. They check that line splitting matches `std::getline`, and that a child process ends even with a reading thread blocked on standard input.
  The tests caught two mutations (reverting to `getline`, dropping the remaining line before EOF)
- On the real machine it was ended with `SIGINT` with standard input held open. `--no-adapter` ended in 58ms, and the `sudo` adapter host in 0.13 seconds. In the
  two-direction field test it was 80ms and 81ms
- Commit `573f56a`

The time values were copied from the measurement script output at the time. The raw output was not kept in the repository.
