# 0016. Path and Line Format of the Local Record File

- Status: accepted
- Date: 2026-10-07
- Related: [`../architecture.md`](../architecture.md) chapter 9, [`../spec.md`](../spec.md) M-6, FR-13, FR-16, [`../concurrency.md`](../concurrency.md) chapter 5, chapter 7, [`../windows-prereq.md`](../windows-prereq.md) section 4, [`../roadmap.md`](../roadmap.md) Phase 4, [ADR 0015](0015-refer-to-members-by-display-name.md)

## Context

`architecture.md` chapter 9 decided only four contracts for the local record file (content,
timing, append-only, independence) and two kinds of lines (establishment line, end line). It did
not decide the path, the line format, the notation for an empty value, or how the two lines of
one attempt are tied together. So `spec.md` M-6 was "cannot be judged yet". `roadmap.md` Phase 4
kept this as a decision to make before starting, and said to also decide how an attempt aborted
by `leave` is written.

## Decision

The repository owner decided the following.

**1. The path is `records\` under the executable folder.** There is no startup argument that
changes the path.

**2. Each process creates a new file.** The name is `<UTC startup time>-<PID>.log`. Files are not
deleted or rotated.

**3. A line is `<name>=<value>` pairs joined by spaces.** The order of the nine fields is fixed,
and there is a format version `v`. An empty value is `-`.

**4. The fields are `v`, `kind`, `ts`, `attempt`, `role`, `peer`, `result`, `rtt_us`, and
`setup_ms`.** RTT is an integer in microseconds. The display name is not included. The setup time
`setup_ms` is added. The content contract of chapter 9 grows by that much.

**5. An attempt is one session, or one join or room creation that ended before a session
existed.** The host's pairs are counted separately. A host that created a room and saw nobody
come has no lines.

**6. An attempt given up before it reached `CONNECTED` is an establishment line with
`result=ABORTED`.** The trigger is this side's `leave` or shutdown event, or the other side's
`CLOSE` (`reason` `0x00` or `0x01`). If the other side's `CLOSE` reports a failure (`0x02` or `0x03`),
it is `PEER_FAILED`. This holds for both the establishment line and the end line.

**7. Failing to open the file at startup is a startup failure.** A write failure while running is
a `WARN` and the counter `record_write_failed`, and the tunnel is kept. After that nothing is
written to that file. Each line is written to the OS immediately, and the disk cache is not
flushed.

**8. The line-count verdict applies only to runs that went through the whole shutdown procedure
and had no write failure.** Any other run cannot be judged.

## Alternatives

**Alternative 1. A default path under `%LOCALAPPDATA%` and a `--record-dir` argument.**
Dropped. Its advantage is that it works even in a placement where the executable folder cannot be
written. The repository owner chose the option that is easier to find. The cost is the write
precondition in [`windows-prereq.md`](../windows-prereq.md) section 4.

**Alternative 2. The current working folder.**
Dropped. Records get scattered depending on where it was run from.

**Alternative 3. Every process shares one file.**
Dropped. There are tests that start several processes on one machine, and no guarantee that
concurrent appends do not interleave has been confirmed.

**Alternative 4. TSV.**
Dropped. pandas reads it directly. The cost is that it relies on column order, so it breaks easily
when fields are added.

**Alternative 5. JSON Lines.**
Dropped. It is self-describing. The cost is JSON writing code on the C++ side, and using a library
needs approval under `spec.md` NFR-5.

**Alternative 6. Write RTT as an integer in milliseconds.**
Dropped. RTT on the same LAN becomes 0 and gets mixed up with "what is absent is not written as 0".

**Alternative 7. Do not write aborted attempts.**
Dropped. It makes an exception to "an establishment line for every attempt", and the abort
disappears from the record.

**Alternative 8. Write the abort as one failure code.**
Dropped. A person giving up gets aggregated as a NAT failure.

**Alternative 9. Keep running and only leave a `WARN` when the file cannot be opened.**
Dropped. That run cannot be judged by M-6, and that is found out late.

**Alternative 10. Write the other side's `CLOSE` as `ABORTED` regardless of `reason`.**
Dropped. It saves one value. The cost is that "a person turned it off" and "it failed", which
`protocol.md` 5.6 split for Phase 9, are merged in this file.

## Consequences

**What we gain.**

- M-6 can be judged. A machine counts it from the line count and fields
- Phase 9 can match the records of two machines by UTC time and `peer_id`

**What we pay.**

- The executable folder must be writable. Running under `Program Files` without administrator
  rights is a startup failure
- Record files accumulate, and deleting them is up to a person
- The `setup_ms` start point differs between host and player, so the two values are not compared
  with each other
- `[loop]` pauses briefly while writing a line. With two lines per attempt this is taken as small
  and was not measured
