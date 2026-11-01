# 0036. Change the Phase 8 Minecraft verification kit to `field-kit-p8`

- Status: Accepted
- Date: 2026-11-01
- Related: [ADR 0034](0034-decisions-before-phase-8.md), [ADR 0035](0035-linux-console-client.md), [`../roadmap.md`](../roadmap.md) Phase 7 and 8 verification, [`../experiments.md`](../experiments.md) 3.2

## Context

[ADR 0034](0034-decisions-before-phase-8.md) set the work order as "the Minecraft verification is done first with `field-kit-p7b`". The reason is that
the `roadmap.md` Phase 7 verification says NFR-2 holds only if the Minecraft test and the Phase 7 file transfer run on the same tunnel build.

After that, [ADR 0035](0035-linux-console-client.md) added the Linux console client. For the repository owner to do the two-machine field test alone
with a Windows machine and a Linux machine, a Linux build is needed, and `field-kit-p7b` has no Linux client.

## Decision

**The Phase 8 Minecraft verification uses `field-kit-p8`.** This is the repository owner's decision.

- `field-kit-p8` is the Windows GUI distribution and the Linux Release console. The sources that go into both builds must equal the tagged commit. The Windows distribution of the first field test was built from the commit before it, and the only difference from the tagged commit is one Linux platform file that is not part of the Windows build
- **The same-build condition of NFR-2 is kept by running the file transfer again with that kit.** When the kit changes, it runs once that day before Minecraft with the same two executables
- The rest of ADR 0034 stands. T-7 uses three machines

## Alternatives

**Alternative 1. Keep `field-kit-p7b`.** Rejected. The Linux machine cannot be used and there is no GUI. The two-machine field test would again be tied to another person's time.

**Alternative 2. Use `field-kit-p7b` on Windows and a new build on Linux, and judge NFR-2 on the ground that the tunnel data path has not changed
since `field-kit-p7b`.** Rejected. That ground is a judgement from reading `git diff` and is weaker than measuring again with the same build. The file
transfer takes a few minutes.

## Consequences

- The owner alone, with two machines, ran 30 minutes of continuous play, the difference between game-displayed latency and tunnel RTT, block placement and chat, and the Wi-Fi cut from the Minecraft verification
- The file transfer runs again for every kit. When the receiving side is Linux, the receiving side of `Test-Transfer.ps1` does not run, so a separate receiver with the same wire is used
