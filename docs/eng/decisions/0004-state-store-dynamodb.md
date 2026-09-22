# 0004. Use Amazon DynamoDB Instead of SQLite as the Control Plane State Store

- Status: accepted
- Date: 2026-09-22
- Related: [`../architecture.md`](../architecture.md) 3.3, [`../spec.md`](../spec.md) C-3, NFR-5, [`../roadmap.md`](../roadmap.md) Phase 3, [ADR 0003](0003-telemetry-service-split.md)

## Context

The original design kept room and peer state in a SQLite file inside the EC2 instance. `spec.md`
C-3 read "a single AWS EC2 instance and SQLite", and the external dependency table in
`architecture.md` chapter 11 listed `sqlite3` under the Python standard library. Having no
third-party dependency at all was the strength of that choice.

We decided to move the store to a managed service. There are two reasons.

1. **State is not tied to the instance.** A SQLite file lives on that EC2 instance's disk.
   Replacing or adding instances means moving the state with them. NFR-3 requires room state to
   survive a server restart, and with SQLite that assumes "the same instance comes back up with the
   same disk".
2. **[ADR 0003](0003-telemetry-service-split.md) made it two services.** Keeping SQLite means
   picking one of two options. Sharing one file makes the file lock a shared point between the two
   services, so the split is tied back together at the store. Separate files avoid that, but
   context item 1 stays on both sides and there are two backup targets. **Neither is forced.** The
   point is that neither option solves context item 1.

## Decision

**Use Amazon DynamoDB.** The five items below are part of this decision. Every justification is a
sentence from official AWS documentation, and the source is given with each item.

**1. Reads whose value drives a verdict set `ConsistentRead=true` on the table.**

DynamoDB's default read is eventually consistent. "Eventually consistent is the default read consistent
model for all read operations."
([HowItWorks.ReadConsistency](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/HowItWorks.ReadConsistency.html))
If `get_peers` reads this value with the default, **a candidate the peer just registered can be
missing from the response.** The client then starts punching without the peer's endpoint and ends
with `HOLE_PUNCH_TIMEOUT`. The cause is read consistency, not NAT, but the failure code looks like
NAT.

**A GSI is not used for that verdict.** "Strongly consistent reads are only supported on tables and
local secondary indexes. Strongly consistent reads from a global secondary index or a DynamoDB
stream are not supported." (same document)

The price is double the cost. "Eventually consistent reads are half the cost of strongly consistent reads."
(same document)

**2. Virtual IP assignment uses a conditional-write claim. No atomic counter.**

One IP is the key of one item, claimed with `attribute_not_exists()`. "DynamoDB evaluates a
condition expression on a write operation against the item identified by the request's key, so
attribute_not_exists(Id) is true only when no item with that Id value already exists."
([Expressions.ConditionExpressions](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Expressions.ConditionExpressions.html))

Drawing a sequence number from an atomic counter was rejected. The documentation says it is not
idempotent. "With an atomic
counter, the updates are not idempotent." And "An atomic counter would not be appropriate
where overcounting or undercounting can't be tolerated ... it is safer to use a conditional
update instead of an atomic counter."
([WorkingWithItems](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/WorkingWithItems.html))
`10.100.0.0/24` has only 254 assignable addresses, so if a retry skips a number the address leaks.

**A failed condition still consumes write capacity.** "If a ConditionExpression evaluates to false during a
conditional write, DynamoDB still consumes write capacity from the table." (same document)
So we do not blindly iterate over free addresses and try each. The search method is fixed together
with the table design.

**3. Room expiry verdicts are not delegated to TTL.**

The AWS documentation only **describes** the deletion time as "within a few days" of expiry and
**sets no upper bound.**
"DynamoDB automatically deletes expired items within a few days of their expiration time."
And items stay visible after the expiry time until they are deleted.
"If they are not filtered, they'll continue to show in read and write operations until they are
deleted by the background process."
([TTL](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/TTL.html),
[ttl-expired-items](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/ttl-expired-items.html))

**TTL is used only to reclaim storage.** "Is this room alive" is decided by the application
looking at the expiry time directly.

**4. Capacity mode starts as provisioned.**

The documentation's general recommendation is on-demand. "On-demand mode is the default and recommended throughput
option."
([on-demand-capacity-mode](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/on-demand-capacity-mode.html))
But the free tier capacity applies only to provisioned. The pricing page lists 25 WCU / 25 RCU /
25 GB per month and states "It uses provisioned capacity and the DynamoDB Standard table class."
([pricing](https://aws.amazon.com/dynamodb/pricing/))
This is a semester project, so **running inside the free limit** ranks above avoiding capacity
planning. **We do not say the cost becomes 0.** The scope and expiry conditions of the free tier
are item 2 under "Not verified" below, and we check them directly in the account before Phase 3
starts.

**Mode switching is limited.** "You can switch tables from provisioned capacity mode to on-demand
mode up to four times in a 24-hour rolling window."
([bp-switching-capacity-modes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/bp-switching-capacity-modes.html))

**5. No access keys on EC2. Attach an IAM role.**

"If you're running on an EC2 instance, use AWS IAM roles." And "if you've launched an EC2
instance with an IAM role configured, there's no explicit configuration you need to set in Boto3
to use these credentials."
([boto3 credentials](https://docs.aws.amazon.com/boto3/latest/guide/credentials.html))

## Alternatives

**Alternative 1. Both services share one SQLite file.**
Rejected. Zero third-party dependencies was a big advantage. But the file lock becomes a shared
point between the two services, tying the split back together at the store. **The option of
separate files is alternative 2 below.** As written in context item 2, keeping SQLite means
choosing between these two; sharing is not forced.

**Alternative 2. One SQLite file per service.**
Rejected. It solves the shared lock, but state is still tied to the instance disk. Context item 1
above stays as it is.

**Alternative 3. A managed relational DB such as RDS.**
Rejected. What we store is rooms and peers, a simple key-value structure, and no joins are needed.
The relational engine's features are not needed, yet the instance cost and operating burden come
with it.

**Alternative 4. Use DynamoDB but keep the default (eventually consistent) read.**
Rejected. The cost is half, but the failure mode in decision item 1 above **disguises itself as a
NAT failure.** Phase 9 is a project that measures success rate per NAT behaviour; if failures
caused by store consistency are counted as NAT failures, the whole result is polluted. We choose
being right at a higher price over being wrong cheaply.

## Consequences

**What we gain.**

- State is not tied to the instance disk. Room state survives an instance replacement (NFR-3)
- **The file lock as a shared point disappears** ([ADR 0003](0003-telemetry-service-split.md) split 1).
  **This does not mean "the store is not shared."** The two services still use the same DynamoDB.
  What disappears is only the relation where one process holds a lock and the other waits.
  **Whether to split tables, narrow IAM permissions per service, or separate failure domains is
  not decided.** It is decided together with the telemetry spec in Phase 9
- There is no longer a reason to keep access keys in source or documents

**What we pay.**

- **A third-party dependency now exists.** `boto3` is not in the standard library (it is not in
  `sys.stdlib_module_names`, and `pip install boto3` is the official procedure.
  [boto3 quickstart](https://docs.aws.amazon.com/boto3/latest/guide/quickstart.html)).
  `spec.md` NFR-5 requires **prior approval** for non-default third-party dependencies. **It was
  approved.** It is the second approved dependency after Wintun
- **Reads cost twice as much.** This is the price of decision item 1
- **Local tests do not catch consistency defects.** DynamoDB local is an official tool, but the
  documentation says "Read operations
  are eventually consistent. However, due to the speed of DynamoDB local running on your
  computer, most reads appear to be strongly consistent."
  ([DynamoDBLocal.UsageNotes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/DynamoDBLocal.UsageNotes.html)).
  **A defect that omits `ConsistentRead` passes locally.** It has to be checked on a real table.
  The same document says `TransactionConflictException` does not occur locally either
- **Exceeding the free tier is billed.** Requests above provisioned 25/25 are throttled or cost
  money

**Not verified.**

The following three could not be confirmed in official documentation. **We do not write them as
if confirmed.**

1. A numeric upper bound or SLA for TTL deletion delay. The documentation only says "within a few days"
2. A sentence that **directly states** the DynamoDB free tier of 25 WCU / 25 RCU / 25 GB is
   permanent. The pricing page lists it as a monthly benefit and simply does not state an expiry.
   How the 2025 free tier restructuring differs by account creation date was also not verified
3. TTL behaviour in DynamoDB local

Items 1 and 3 are already avoided by decision item 3 (TTL is not used for expiry verdicts). Item 2
is a cost risk, so we check it directly in the account before Phase 3 starts.
