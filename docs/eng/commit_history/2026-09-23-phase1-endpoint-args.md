# Phase 1 Endpoint Type and Startup Argument Parsing

**Target commit:** this commit
**Phase:** 1 (core UDP networking)

"Implement the endpoint representation type (parse, compare, print)" from the `roadmap.md` Phase 1
task list went in, together with the startup input of `architecture.md` 3.5.

## The Documents Were Fixed First

3.5 wrote the same thing in two places in two different ways. The table wrote the role argument as
"Phase 1~2 takes it and only stores it, and starts up without it", while the error paragraph below
wrote "role missing" as a startup failure with exit code 2. Instead of picking one side in the
code, the document was cleaned up first.

| What | Before | After |
|------|-----|-----|
| Missing verdict | "role missing" could be read as always an error | Missing means absent in a range where the "required" column of the table demands it. If the role value is neither `host` nor `player`, that is a format violation in any range |
| Check timing | Not decided | An argument that is given is checked even if that range does not use it. "Only stores it" means it is not used, not that it is not checked |
| Source of the format | The error paragraph listed only a few examples | A table says which document owns each value. `--room` is `control_plane.md` 2.1, `peer_id` is 2.2, `peer_token` is 2.3, and the default port is 2.6 |
| Counterexample list | None | `tests/` owns it. Putting it in the document means lining up the same table in two places |

## What Went In

| Target | Content |
|------|------|
| `network::Endpoint` | IPv4 address and port. Parse, compare (`==`, `<=>`), print as `IPv4:port` |
| `parse_ipv4` | Dotted decimal with 4 octets only. What it accepts is fixed as a list |
| `parse_port` | ASCII decimal. 0~65535 |
| `hamychi::parse_args` | The six inputs of 3.5. Errors come back as fixed tokens |
| `main.cpp` | On an argument error, one line of `ERROR args.invalid reason=<token> arg=<raw>` and exit code 2 |

**The address conversion of the standard library is not used.** `inet_addr` accepts 3-octet forms
like `1.2.3`, hex like `0x7f.1`, and octal like `010`. If those pass, the address the user typed
and the address we use are different. It was written as an allow list instead of an exclude list.

**Ports 1~65535 are judged by the argument side, not by `Endpoint`.** `protocol.md` chapter 6 binds
the socket with port 0, so the representation type has to hold 0.

## Verification

The Catch2 cases grew from 4 to 25 and all pass. The 7 `build.ps1 -SelfTest` cases are unchanged.
On top of that, **18 cases that run the executable directly** were added as
`scripts/cli-check.ps1`. They look at what a unit test cannot see: the exit code and the log line
that goes out on standard error. `scripts/test.ps1` runs them right after `ctest`.

21 mutation tests were run. What the case table actually protects is written from that result.

| Target | Mutants | Killed | Survived |
|------|:---:|:---:|:---:|
| `endpoint.cpp` | 5 | 3 | 2 |
| `args.cpp` | 16 | 16 | 0 |

The 16 in `args.cpp` were not all killed from the start. In the first run of 14, one mutant on the
label length survived and the cases were filled in. Two more guards came in while taking the review
findings, which made 16.

### How the Three Surviving Mutants Were Handled

**1. The colon count check (`endpoint.cpp`). Deleted.** It was dead code. A colon after the first
colon goes into the port part, and `parse_port` rejects it because it is not a digit. There can be
no colon before the first colon. It cannot change the result for any input. **It was worse because
the comment said "IPv6 is filtered out here".** The place that really filters is the format check
of the address and of the port.

**2. The colon required check (`endpoint.cpp`). Kept.** This one also cannot change the result
today. `npos + 1` wraps to 0, the port side gets the whole string, and `parse_port` rejects it
because dots are mixed in. **That behavior is an accident.** If `parse_port` gets even slightly
more permissive, the address string is read as a port. A case cannot catch this spot, so the fact
was written into a code comment.

**3. The label length check (`args.cpp`). Cases were filled in.** Unlike the two above, this one
was a real hole. There was no case at all that tested a label longer than 63 characters. Boundary
cases with 63/64-character labels and 253/254-character hosts were added, and then it was killed.

**The reasons a mutant survives split into three.** Dead code, a mutant with the same behavior, and
a hole in the cases. The three must not be handled as one thing. The first is deleted, the second
is kept with the reason written down, and only the third gets more cases.

## Cross-Model Review

Codex was given the lens "code that parses untrusted input directly". Of 3 findings, **1 was taken
and 2 were rejected.** The two rejected ones misread the code path, but **both pointed at spots
where the tests were empty,** so cases were filled in while rejecting them.

| Grade | Finding | Handling |
|------|------|------|
| warn | `--server` accepts `999.999.999.999` as a name. `--peer` rejects the same value, so the two parsers disagree | **Taken.** If the last label is all digits, it is rejected. If the value is a literal, that check is skipped. Such a value fails as a literal too and cannot be resolved as a name |
| warn | `--peer 192.0.2.1:80:90` gives `BadHost` | **Rejected.** It gives `BadPort`. The address only looks up to the first colon and already succeeds. **No case walked that path,** so a case was put there |
| warn | `a.example:` gives `BadPort`, but `MissingPort` exists separately, so they cannot be told apart | **Rejected.** They are already split. `MissingPort` is no port slot at all, and `BadPort` is a slot with a wrong value. Merging them gives one token two meanings. That contract was written into `args.hpp` and an `a.example:` case was added |

**A wrong finding pointed at an empty spot.** The inputs that the two rejected findings named were
on a path that no case had walked until then.

### Round 2 — Final (Scenario Lens + Record Lens)

Two lenses were put on the whole diff. Both findings were taken.

| Grade | Finding | Handling |
|------|------|------|
| warn | `arg=<raw>` is not wrapped, so a value with a space loses the boundary of `name=value` | **Taken.** The value is wrapped in quotes, and `"` and `\` are escaped. Arguments with a space and a tab were added to the CLI cases |
| warn | This record says "10 executable cases" were verified, but the diff does not hold that artifact | **Taken.** That check was run by a temporary script outside the repo. It was brought in as `scripts/cli-check.ps1` and the cases were grown to 18 |

**The record lens caught that the basis of a "verified" sentence was outside the repo.** Left as it
was, the next session would read a claim it cannot reproduce.

## What Is Left

Phase 1 still has logs and counters, the UDP socket wrapper, and the `[loop]` event loop.
`sanitize` in `main.cpp` moves into the log module when that module comes in. The order is held by
`plan.md`.
