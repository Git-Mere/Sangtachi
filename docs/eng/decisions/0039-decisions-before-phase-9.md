# 0039. Decisions before Phase 9

- Status: Accepted
- Date: 2026-11-16
- Related: [`../roadmap.md`](../roadmap.md) Phase 9, [`../architecture.md`](../architecture.md) 3.4, 3.5, 6.3, and chapter 9, [`../concurrency.md`](../concurrency.md) chapters 4 and 6, [`../protocol.md`](../protocol.md) 4.5, [`../control_plane.md`](../control_plane.md) 7.6, [`../spec.md`](../spec.md) FR-14, NFR-10, and C-3, [ADR 0001](0001-no-direct-connection-fallback.md), [ADR 0003](0003-telemetry-service-split.md), [ADR 0004](0004-state-store-dynamodb.md)

## Context

`roadmap.md` Phase 9 lists two things to decide before starting: the telemetry service spec (collection schema, authentication,
upload period, retention, DynamoDB table) and resource isolation within one instance. For aggregation and graphs it also asks what to
upload, how to mark the environment, the graph library, and the FR-14 wording. Reading the documents turned up more gaps.

- The client has no way to receive the telemetry service address. `architecture.md` 3.5 only said "Phase 9"
- `architecture.md` 3.4 said the method for checking received identifiers belongs to `roadmap.md` Phase 9, but that section does not
  have it. Control plane items are deleted a day after the room expires, so at analysis time nothing is left to check against
- `protocol.md` 4.5 said the loss rate is uploaded on a 10-second period. That value was set separately from the upload period
- DynamoDB's free capacity (RCU 25, WCU 25) is per account. The control plane table uses all of it

How other services handle the same kind of data was checked in official documentation. Teams call quality records and Twilio call
summaries are produced after the call ends. Sentry, Azure Application Insights, and OpenTelemetry batch and send every few tens of
seconds while running. Real-time collection is kept as a separate feature for operators watching calls in progress. This project's
purpose is after-the-fact analysis.

## Decision

The repository owner chose the following for each item.

| Item | Choice |
|------|---------|
| When to send | Once per attempt when establishment ends, every 60 seconds during `CONNECTED`, and once when the session ends. Nothing is sent from the lobby |
| Values of periodic records | Carry only that interval's values. RTT is the interval's samples as is; loss and throughput are interval differences. Sending cumulative values every time was rejected |
| Session end record | Carries the last interval's values and the totals for the whole session |
| Who sends | Both host and player. The host has a separate session per player, so its records are separate too |
| Retry | None. The current contract (NFR-3) stays as is |
| Last upload on shutdown | Waits only within the current join limit (2 seconds). The limit is not raised |
| Record grouping key | A random value made at process startup, the attempt number, and the send sequence number. The server only adds |
| Jitter | Applies the RFC 3550 smoothed jitter formula to differences of consecutive RTT samples. The original formula is for differences of one-way delay, so this is a variant |
| Addresses and network information | Not uploaded. Public addresses, candidates, network fingerprints, and network types alike. Instead of the "irreversible pseudonym" of ADR 0001 follow-up action 2, "not uploaded" was chosen |
| Display names | Not uploaded. People and machines are told apart by `peer_id` and the process value |
| Device identifier | None. Restarting the process gives a new value |
| NAT observations | Whether the local and public addresses differ, whether the two STUN servers saw the same endpoint, whether the counterpart has the same public IP. All three are bool |
| Build information | OS, GUI or console, test build or not. The client has no build version string now, so it is not carried |
| Trial environment | Not carried in telemetry. Whoever measures writes the UTC time range and environment of each batch in a local file that is not published, and the analysis script splits by time range. [`experiments.md`](../experiments.md) carries only the date, per ADR 0001 |
| Consent | No consent dialog. The collected items are written in the README and the package guide. The telemetry service's logs do not keep the source IP |
| Service address | Startup argument `--telemetry` and a built-in default. Port 8001 |
| Transport format | Reuses the control plane's HTTP subset and JSON as is. There is one operation, `report` |
| Authentication | None. Input limits block abuse |
| Input limits | Headers 2048 bytes, body 4096 bytes, 30 tokens per source IP refilled at 30 per minute, 5 per second for the whole server, 32 concurrent connections, 5-second read |
| Identifier check | On receipt, read the `ROOM` item in the control plane table once and attach the result to the record. Nothing is rejected |
| Storage | A separate table. Not deleted until the report is submitted |
| Capacity | Split the account's free capacity. Lower the control plane table to RCU 20, WCU 20 and set the telemetry table to RCU 5, WCU 5 |
| IAM | Add telemetry permissions to the same instance role. An instance can have only one role attached |
| Resource isolation | systemd resource limits (`MemoryMax=256M`, `CPUQuota=20%`, `IOWeight=50`), journald total 200MB, the input limits above |
| Deployment | A separate systemd unit. `tools/cp-deploy` is extended to choose a target |
| Aggregation and graphs | Amazon CloudWatch. The service puts custom metrics and the owner views them in a dashboard. Only the free limit of 10 custom metrics is used, with no dimensions |
| Exact tables | Attempt counts, Wilson intervals, counts per failure code, and comparisons per mapping behavior are built from DynamoDB by an analysis script in `tools/` |

## Alternatives

**Send cumulative values every time.** Sending the whole session state every time, as Sentry does, means the next upload fills in a
lost one. Rejected. To put them into CloudWatch the server would have to read the previous value and take the difference, and reducing
RTT to a histogram makes the median as coarse as the bucket width. Interval values lose only the lost interval, and if a later record
arrives, what was lost shows as a gap in the send sequence number.

**Send once after the session ends.** Rejected. If the process dies suddenly, the whole session is missing.

**Only the host sends.** Rejected. The host does not know about player-side failures before establishment (`STUN_DISCOVERY_FAILED`,
join failure). Loss is counted by the receiving side, so only one direction would remain. The setup time the user waited through is the
player-side value.

**Detect the environment in code.** Grouping networks by a gateway MAC hash, or telling carriers apart by the ASN of the source IP, was
rejected. Which network it is does not matter to the owner. The former can be reversed even when hashed, and the latter needs a data
file on the server.

**Do not check identifiers.** Rejected. There is no authentication, so anyone can insert records. Records arrive while the room is alive
or less than a day after it expires, so they can be checked on receipt.

**Relation to ADR 0003 alternative 2.** That alternative was telemetry asking the control plane, and it was rejected because the
control plane would take on telemetry's load and failures. This decision reads that table without going through the control server
process. The check keeps running even if the control server stops. The table's read capacity is shared. At 5 per second for the whole
server, it is at most 2.5 RCU per second. So this is treated as narrowing the wording of Splits 2 and 3, not reopening that alternative.

**Reject.** Not accepting records that fail the check was rejected. A failure that ended before a room was set up has no room to check.
Rejecting it would drop failures and inflate the success rate.

**Shared key authentication.** Rejected. The key can be extracted from the distributed executable.

**Same capacity as the control plane.** Setting the telemetry table to 25/25 as well was rejected. Free capacity is per account, so the
excess is billed. Putting only telemetry on on-demand was also rejected. It is small, but the cost is not 0.

**Wait 3 seconds for the last upload.** Raising the join limit to 3 seconds was rejected. On console window close and logoff, Windows
ends the process right after cleanup, so raising it does not help, and only the other paths shut down later.

**The service draws the graphs.** Putting a graph library in the service was rejected. It needs the instructor's approval under NFR-5
and uses t3.micro CPU credits.

**Dimensions in CloudWatch.** Using mapping behavior or same-LAN status as dimensions was rejected. Each combination of name and
dimension values is one metric, so it immediately exceeds the free limit of 10. Environments are chosen by the dashboard's time range.

## Consequences

**Gains.**

- The client's upload path fits into the current isolation structure (`concurrency.md` chapter 6) as is
- The server only adds, so there is no overwrite or ordering decision
- Graphs add no new dependency. `boto3` is already approved
- No collected item points to an individual

**Costs.**

- Losing one upload loses that interval's values. At most about a dozen RTT samples
- On console window close, logoff, and system shutdown, the session end record may often be missing
- Without authentication, fake records can get in. The check only sees whether the room exists. It cannot stop someone who knows a
  live room code
- Because it is the same role, the control server process can also write to the telemetry table, and the telemetry service reads the
  control plane table
- The control plane table's capacity drops from 25 to 20. That is still more than the measured peak of 7.1 reads and 2.6 writes
- CloudWatch cannot split views by condition. Exact tables come only from running the analysis script
- CloudWatch 1-minute data is rolled up to 5-minute resolution after 15 days and to 1-hour resolution after 63 days. The originals
  stay in DynamoDB

**Not confirmed.**

- Whether the systemd limits and input limits above let both services hold up together on t3.micro. To be checked with load after
  deployment
- Whether the CloudWatch dashboard percentiles are accurate enough to put in `experiments.md`. To be compared once against the
  analysis script's values
