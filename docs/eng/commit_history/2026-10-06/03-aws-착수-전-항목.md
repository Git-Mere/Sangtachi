# Closed the Five AWS Account Before-Start Items

## Why

Among the `roadmap.md` Phase 3 before-start items, five that need an AWS account were left. They are
Elastic IP and DNS, credentials, free tier, deployment configuration values, and the two clock
assumptions. The repository owner created and checked them directly in the console and on the
instance and reported the values, and the agent judged them.

## What Was Checked

| Item | Result | Basis |
|------|--------|-------|
| Clock assumption 1 | Pass. `clock_gettime(CLOCK_MONOTONIC)`, `adjustable=False` | `time.get_clock_info('monotonic')` on the instance |
| Clock assumption 2 | Pass. `boot_id` differed before and after a reboot | The two values were compared |
| Credentials | Pass. IAM role, no credentials file | `aws sts get-caller-identity`, `ls ~/.aws/credentials` |
| Free tier | Pass. DynamoDB 25 WCU / 25 RCU / 25 GB are Always Free | The account's Free Tier screen. That item of "What could not be confirmed" in ADR 0004 is closed |
| Table | Keys `pk` and `sk`, TTL `ttl`, no index | Console |
| Elastic IP | Attached | Console. The `describe-addresses` judgement command was not run |
| Security group | TCP 8000 allowed from everywhere | Console |

## What Was Fixed During Judgement

Five of the values the owner first reported disagreed with the documents, and the owner fixed them
in the console.

| What disagreed | Handling |
|----------------|----------|
| Table keys were `PK` and `SK` | The table was deleted and recreated with `pk` and `sk`. A key schema cannot be changed after creation, and this is smaller than changing the documents |
| TTL off | Turned on with `ttl` |
| No TCP 8000 in the security group | Opened |
| No `dynamodb:ConditionCheckItem` in the IAM policy | Added. A ConditionCheck inside a transaction needs this permission separately (confirmed by fetching the original text of the transaction IAM section of the AWS DynamoDB Developer Guide) |
| Capacity RCU 5, WCU 5 | Raised to 25 and 25. Calculation below |

**The capacity was set by working out the open item of `control_plane.md` chapter 10, "Do renewal
writes fit in the free tier".** The renewal writes of one room below capacity are at worst 2.4
WCU/s, and one polling player is at worst 8 RCU/s, so 5 was not enough. That the Always Free limit
is 25 and 25 per region and payer account was confirmed by fetching the original text of the AWS
DynamoDB pricing page, and that burst capacity is 300 seconds, from the Developer Guide. The
calculation itself was not measured, and a CloudWatch check was hung on the `roadmap.md` Phase 3
verification.

**No domain is created.** This is the owner's decision. The client is given an IPv4 literal. It is
an operation `windows-prereq.md` section 10 already allowed.

## What Changed

| File | Content |
|------|---------|
| `control_plane.md` 7.4 | "The two assumptions were not checked" changed to the check results |
| `control_plane.md` 7.6 | Deployment environment table, the six IAM actions, capacity and load calculation |
| `control_plane.md` chapter 10 | The three closed open items removed (table, capacity and region; the actual Elastic IP and DNS values; renewal writes within the free tier) |
| `roadmap.md` Phase 3 | The five before-start items removed. The deployment work item points at 7.6. A "Deployment" group in the verification (no permission errors, CloudWatch capacity) |
| `windows-prereq.md` sections 6 and 10 | Verification status. "Both with DNS" in section 10 changed to a condition of an operation that uses DNS |
| `plan.md` | Next to do item 1 removed |
| `.gitignore` | `deploy/*.local.md`. And one line for a key file name that existed only in the working tree |

**The actual values were not committed.** The account number, Elastic IP, instance ID, role name,
region, table name and `boot_id` values are in `deploy/aws.local.md`, which is ignored. The staged
diff was scanned for those values and none were found. The key file name was already an ignore
target and is not the key content.

## Verification

| What | Result |
|------|--------|
| `python tools/docgate/docgate.py` | `link` 0. The rest are `mirror` and `parity` because the English mirror is not made yet |
| Sensitive values in the staged diff | Scanned for the nine values above, 0 found |
| `git status --ignored deploy` | All of `deploy/` is ignored |

## Cross-Model Review

### Round 1 — Consistency + Record and Leak Bundle (warn 2, nit 1)

Both lenses were covered.

| # | Grade | Lens | Finding | Handling |
|---|-------|------|---------|----------|
| 1 | warn | record | A strongly consistent `Query` rounds the byte sum up in 4KB units, so assuming 1KB per item cannot guarantee 1 RCU per call | Accepted. Recalculated with worst-case values. One poller 8 RCU/s (15KB, 4 RCU), `host_report` reads 1.2 RCU/s. Written that four polling at once reach at worst 32 RCU/s, briefly over 25 |
| 2 | warn | consistency | The raw CloudWatch consumed capacity value is a sum over the period, so it cannot be compared directly with 25 per second | Accepted. 1-minute `Sum` / 60 and the `Sum` of `ThrottledRequests` |
| 3 | nit | record | The record says four values disagreed, while the table has five | Accepted |


### Round 2 — Repair Check (warn 1)

| # | Grade | Lens | Finding | Handling |
|---|-------|------|---------|----------|
| 1 | warn | repair | The polling worst case is based on a full room's 15KB, not 12KB, so it is 4 RCU, 8 RCU/s | Accepted. Four polling at once is at worst 32 RCU/s |

### Round 3 — Repair Check (warn 3)

| # | Grade | Lens | Finding | Handling |
|---|-------|------|---------|----------|
| 1 | warn | repair | The worst case of the `host_report` read missed a room that has just become full (15KB) | Accepted. 8 RCU per call, at worst 1.6 RCU/s |
| 2 | warn | repair | 32 RCU/s is the value for polling alone but was written as if it were all reads of the room | Accepted. Written as the polling-only value, with `host_report` reads added separately |
| 3 | warn | repair | It asserted that burst capacity absorbs it | Accepted. Fetched and confirmed the original AWS guide text ("consume burst capacity for background maintenance ... without prior notice"), and wrote that it is not guaranteed and that a throttle gives `internal` |

### Round 4 — Repair Check (warn 1)

| # | Grade | Lens | Finding | Handling |
|---|-------|------|---------|----------|
| 1 | warn | repair | 1.6 RCU/s is the value for periodic calls only and leaves out the immediate call at session end in 4.6 | Accepted. Written as the periodic-only value, and that each immediate call adds 8 RCU per call |

### Round 5 — Repair Check

`LGTM - no blockers`.
