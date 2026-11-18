# Embedding the telemetry address in the GUI distribution

## Why

[`architecture.md`](../../architecture.md) 3.5 Startup Inputs says the person who makes a release build embeds both the control server address and the telemetry service address. `scripts/package-gui.ps1` only took the server address, so the GUI distribution was always a build with telemetry off. The owner asked for a GUI distribution with telemetry on.

## What changed

| What | Where |
|------|------|
| Optional argument `-Telemetry`. It is passed as `SANGTACHI_DEFAULT_TELEMETRY`. If omitted it is empty, so the build has telemetry off as before | `scripts/package-gui.ps1` |
| The build command and the `-Telemetry` description | `windows-prereq.md` section 4 |

The script outside the repo that fills `latest_build` on this PC was also changed to pass this argument. It is outside the repo and not part of this commit.

## Verification

| What | Result |
|------|------|
| `docgate.py` | `VERDICT: pass` |
| Built the distribution with `package-gui.ps1 -Telemetry <Elastic IP>` | `kDefaultTelemetry` in the generated header is that address. `gui_main.cpp` was recompiled. 16 files, closure check passed. The GUI needs administrator rights and was not launched |

## Cross-model review

Codex (`gpt-6.1-sol`). Lenses: script correctness (A), Korean-English agreement (B), docs matching the script (C), public repo exposure (D). `LGTM - no blockers` on the first pass.
