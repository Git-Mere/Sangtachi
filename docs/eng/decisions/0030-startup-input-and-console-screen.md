# 0030. Startup input and console screen

- Status: Accepted
- Date: 2026-10-22
- Related: [`../architecture.md`](../architecture.md) 3.5, chapter 8 and chapter 9, [`../windows-prereq.md`](../windows-prereq.md) 1, 4 and 14, [`../roadmap.md`](../roadmap.md) Phase 8, [`../control_plane.md`](../control_plane.md) 2.7, [ADR 0015](0015-refer-to-members-by-display-name.md)

## Context

`roadmap.md` Phase 8 asks to decide, before starting, the startup input path the GUI uses and when privilege elevation happens. While
the repository owner waited for the Phase 7 two-machine measurement, the parts of that which do not depend on the GUI structure were
decided first. The starting point was that the executable was cumbersome to use.

- Every run needed `--server <address>` and `--name <name>` typed. There is one control server and its address is fixed
- Double-clicking in Explorer was a startup failure for lack of administrator rights (`adapter.error reason=not_elevated`), and the
  window closed at once so the reason was not visible
- Nothing told what could be typed in the lobby. The log came out in the same window and filled the screen
- The sentences shown to humans were in Korean

## Decision

The repository owner decided the following.

**1. The default control server address is built in.** It is the CMake value `SANGTACHI_DEFAULT_SERVER`. `--server` overrides it. If
empty, `--server` is required as before. The repository is public, so the real address is not kept in the repository; whoever makes a
release build gives it at configure time. Test builds leave it empty.

**2. `--name` is no longer required.** Without it, the name is `user_` followed by 8 ASCII lowercase letters and digits chosen with a
CSPRNG. It is changed with the lobby `name` command. It is an underscore rather than a hyphen because the character set of
`control_plane.md` 2.7 has no hyphen.

**3. When standard output is a console, the lobby guide is emitted.** That is the lobby guide, the room guide, the new command `help`,
the rejection reason lines, `Loading...`, and the input prompt `> `. When standard output is redirected, none of them is emitted.

**4. Every line shown to humans is in English.** That is the guides and the failure sentences of `architecture.md` chapter 8.

**5. The log goes to a log file.** It is opened under `logs\` next to the executable with the same name as the record file. Standard
error gets every line if redirected, and only the `ERROR` lines during startup if it is a console.

**6. Release builds include the administrator rights manifest.** It is the CMake option `SANGTACHI_REQUIRE_ADMIN`. Test builds do not
include it.

**7. When startup fails in a window used alone, the process waits for Enter.** Two guide lines say that the reason is in the `ERROR`
line and give the log file name.

## Alternatives

**Alternative 1. Keep the server address as a source constant.**
Rejected. It is the simplest. The cost is that the address stays in a public repository. It collides with the reason `deploy/*.local.md`
is ignored.

**Alternative 2. Keep the server address in a configuration file next to the executable.**
Rejected. The address could be changed without rebuilding. The cost is opening the configuration file that `architecture.md` 3.5 decided
not to have in Phase 1~5, and which file a run used does not appear in the record.

**Alternative 3. Keep `--name` required, with no default name.**
Rejected. Alternative 8 of ADR 0015 was rejected because everyone using the same default name runs into `name_taken`. An 8-character random
suffix has 36 to the 8th power possibilities, so a collision inside one room (at most five people) practically does not happen. If one does,
the name is changed with `name` after the `name_taken` `FAIL`. The remaining cost is that a name no person chose can show up in the
`<counterpart>` of a `FAIL` line and in the member list.

**Alternative 4. Always emit the guides to standard output.**
Rejected. Scripts reading standard output would have to tell for every line whether it is a guide. Whether it is a console already
separates humans from scripts.

**Alternative 5. Leave the log on the screen.**
Rejected. The guides get buried. Scripts and field procedures that redirect standard error still get every line under the changed rule.

**Alternative 6. Without administrator rights, the client requests UAC elevation itself and relaunches.**
Rejected. A `--no-adapter` run from a non-elevated shell would still work in release builds. The cost is the code for passing arguments on
the relaunch path, the new window, and handling refusal. The manifest has no code.

**Alternative 7. Keep the guides in Korean.**
Rejected. The repository owner decided that the lines shown to humans are in English.

## Consequences

**Gains.**

- A release build reaches the lobby with only a double-click and UAC consent
- The standard output and standard error that test scripts read stay the same

**Costs.**

- A release build cannot be launched from a non-elevated shell. Neither can `--no-adapter`
- When the default address changes, the release build is rebuilt
- Every run adds one file to `logs\`. Deleting them is up to a person
- The `field-kit-p7` kit is a tag from before this decision, so it does not have the changed behavior. It comes in from the next kit
