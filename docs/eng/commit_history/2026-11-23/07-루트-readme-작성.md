# Write the root README

## Why

The `plan.md` document debt still listed the placeholder text of the root `README.md` (overview, screenshots). With Phases 1-9 done, the owner made it the next task. The goal is that someone seeing the repository for the first time (a recruiter, another student) learns on one screen what the project does and what results it produced.

## What changed

| Place | What |
|------|------|
| `README.md` | Rewritten in English. One-line pitch, document link row, Highlights, About, Features, How It Works (Mermaid diagram and 5 steps), Results, Built With, Getting Started, Telemetry, Repository Layout, What I Learned, License |
| `README.ko.md` | New file. The Korean version of the same content. The top of each file has a language link to the other |
| `plan.md` | Replaced the document debt row for the root `README.md` placeholder text with a row for the GUI screenshot and distribution link |

All build, test, run, environment variable, telemetry, and repository layout content of the old README was carried over. The English version is a translation, and the Korean version keeps the original text, except that the run, build path, test, and telemetry wording that review found wrong was fixed in both. New additions are the administrator rights requirement (linking `windows-prereq.md` chapter 1), the `package-gui.ps1` command that builds the GUI distribution folder, and three steps for connecting with Minecraft (content of `windows-prereq.md` chapters 5 and 11).

## Important decisions

- **The numbers come from `experiments.md`.** The RTT, success rate, loss, and Minecraft numbers in the README summarize `experiments.md` 3.2, 3.4, 6.2, 6.3, and 6.5, and the Results section links to that document. The README is an introduction, not a living document under `docs/`, so it keeps a summary. When the measurements change, the README is updated with them
- **Test counts are written only as "690+" and "260+".** For Catch2 this rounds down the executed result of 693/693 unit tests in the Phase 9 closing record (`2026-11-23/03`); for pytest it rounds down the 264 `def test_` declarations in the `control-server/` and `telemetry-server/` sources as of 2026-11-23
- **There is no screenshot section.** There is no GUI screenshot or distribution download link yet, so it stays as `plan.md` document debt. Instead the two Phase 9 graphs (`docs/assets/phase9/`) are in Results
- The time values (keepalive 15 s, `PING` 5 s, idle 50 s) follow the `protocol.md` chapter 11 timers, and the 6-character room code follows the room code definition in `control_plane.md`

## Verification

| What | Result |
|------|------|
| Relative links in both READMEs (18 distinct targets each) | All exist in the repository. All are files except the `decisions/` folder (checked by script) |
| `docgate.py` | `VERDICT: pass` |
| `docgate.py --claims` | `VERDICT: pass` |

## Cross-model review

Codex (`codex exec`). An evidence lens (checking the README's numbers and facts against `experiments.md`, `spec.md`, `protocol.md`, `architecture.md`, `windows-prereq.md`, and the scripts) and a lens for Korean/English parity and content lost from the old README.

| Finding | Handling |
|------|------|
| (blocker) The run section starts the console without `--server`, and the `.env` note has nothing to do with that run | Applied. Wrote host and player commands with the role argument and `--server`, administrator rights, and running the GUI distribution, and described `.env.example` as the template for local environment variables |
| "Every pair" in the 4-person room reads as if players had tunnels to each other | Applied. Changed to the 3 host-to-player pairs |
| +3.3 ms reads as a value for the whole 34 minutes | Applied. Stated it is the median of 21 pairs in a window of about 2 minutes 46 seconds |
| The control plane outage test was one same-LAN tunnel | Applied |
| 700+ is the number of `TEST_CASE` declarations in the source. The Phase 9 closing record has 693/693 unit tests | Applied. Changed to "690+ Catch2 unit tests" |
| DigiPen, solo, and capstone are not confirmed in the repository | Rejected. `CLAUDE.md` says "CSP 400 capstone", and the owner confirmed the solo project and the school |
| Reads as if firewall setup goes away | Applied. Now says only the router step goes away, and links administrator rights and the firewall exception to `windows-prereq.md` chapter 15 |
| Two matching STUN responses read as proof of destination-independent mapping | Applied. Says it asks two servers in parallel and that a match is the condition hole punching needs |
| MTU 1400 reads as a guarantee against fragmentation | Applied. Says it did not fragment on the measured path and links `protocol.md` chapter 12 |
| The 49/50 table omits that the trials used `--no-adapter` | Applied. The top of Results states how trials ran and that they ran without the virtual adapter |
| 0/10 was written like a failure count | Applied. Changed to "connected 0 of 10 times" |
| `-BuildDir` does not enforce outside the repository | Applied. Says output is outside the repository "by default" and `-BuildDir` is used as given |
| (nit) `build-root.local` reads the first non-blank, non-comment line | Applied |
| Without `-Telemetry`, telemetry can still be turned on with `--telemetry` | Applied |
| (nit) `ctest` parallel count and serial `[slow]` cases are missing | Rejected. That spot carries over the old README wording, and the detailed rules belong to the top of `scripts/test.ps1` |
| Reads as if `cli-check` and `e2e-check` also run with `-Asan` | Applied. Says they run only without `-Filter` and `-Asan` |
| (nit) The English version lost the `CLAUDE.md` pointer for when to run and the `.env` copy command | Applied |
| In the telemetry summary, setup time exists only on success and the counterpart's `peer_id` is missing | Applied. Also fixed in the original Korean text |
| Loss is sent as expected and accepted counts, and duration exists only in `end` | Applied. Also fixed in the original Korean text |
| Saying the STUN observation "explained" the campus failure is overstated | Applied. Says it narrowed the cause but cannot tell which of the two it was |
| (nit) `plan.md` removed the GUI screenshot placeholder without a record | Applied. Document debt has a row for the GUI screenshot and distribution link again |

After the fixes, the whole staged diff was reviewed again with the evidence lens and a record and handoff consistency lens.

| Finding | Handling |
|------|------|
| Two matching STUN responses were still written as proof of destination-independent mapping | Applied. Changed to evidence that "the mapping did not change between those two destinations". Fixed the lessons section too |
| The packaged GUI takes no arguments, so `--telemetry` cannot turn it on | Applied. Says `-Telemetry` sets the GUI's address and `--telemetry` applies only to the console |
| Angle-bracket placeholders fail if pasted into PowerShell as shown | Applied. Replaced with variables, `example.com` example values, and a valid example room code |
| The README still lacks the same-LAN scope of the outage test | Applied |
| No fragmentation is a maximum-size test on one path, not a gameplay observation | Applied. Linked `experiments.md` chapter 2 with its scope |
| (nit) "6자리" reads as a numeric code | Applied. "영문과 숫자로 된 6자 코드" |
| (nit) The `--server` requirement was tied to Debug | Applied. Now tied to a build without a compiled-in server address |
| (nit) One of the 18 targets is a folder | Applied |
| (nit) 693 is an execution result and 264 a declaration count, but both were written as execution results | Applied |
| (nit) The top table of `plan.md` lacked the distribution link | Applied |

The same two lenses were run a third time.

| Finding | Handling |
|------|------|
| How It Works puts STUN before room creation and virtual IP assignment | Applied. The room and virtual IP are now step 1, and STUN and candidate exchange step 2 |
| Calling the STUN match evidence for the hole punching condition is still too strong. The spec uses it to classify supported networks | Applied. Now says it meets the `spec.md` supported network condition, with a link |
| Narrowing the campus failure to mapping behavior is also a definite claim | Applied. Now says the different public ports were a clue and the cause is not confirmed |
| The telemetry section writes `--telemetry` as if the GUI also took it | Applied. The console takes the argument or the build default; the GUI only the build default set with `-Telemetry` |
| Requirements lack Git (fetches Catch2) and PowerShell 7 (`pwsh`) | Applied |
| (nit) The sentence on the source of the test counts mixes declaration counts and execution results | Applied |
| (nit) The `plan.md` document debt row does not pair what is added where | Applied |

Final confirmation review.

| Finding | Handling |
|------|------|
| The supported network condition needs each peer to see the same endpoint, and the client can ask further servers | Applied. Now says when each peer sees the same endpoint from two different STUN servers |
| The telemetry section promises a full list but omits abort results and the end record's result and totals, among others | Applied. The section now says it is a summary and that the full field list is `architecture.md` 6.3. Added abort to results and result and totals to the end record |
| `10.100.0.1:25565` was written as a general, game-agnostic address | Applied. Says to connect on the game server's port, with 25565 as the Minecraft Java example |
| (confirmation re-run) Adapter address and route setup was placed after hole punching. The client sets them up before STUN, once room creation or joining succeeds | Applied. Moved to step 1; step 4 covers only forwarding |
| (confirmation re-run) The packaging script stops if `Licenses\LICENSE` two levels above the kit root is missing | Applied. Says the standard Qt installer layout is needed |
| (confirmation re-run) The packaging command lacks `-ExecutionPolicy Bypass` and is blocked under the default execution policy | Applied. Matches the build and test commands |
| (confirmation re-run) `cmake --fresh` in the packaging script needs CMake 3.24 or later | Applied. Added to the packaging requirements |

Finally the whole staged diff was run again. `LGTM - no blockers`.