# Started DynamoDB local with Docker once

## Why

This is item 1 of "Next to do" in `plan.md`. When ADR 0014 decided to start DynamoDB local with Docker, it
said the image tag and the run command would be put in after starting it for real once. The repository
owner started Docker Desktop.

## What was done

| What | Result |
|------|--------|
| `docker info` | Server 29.8.2, linux |
| Image | `amazon/dynamodb-local:3.3.1`. The most recent tag on Docker Hub with the same digest as `latest` |
| Run | The two commands in `control-server/README.md`. Stopping and starting it again was also run |

The commands were put in `control-server/README.md`. ADR 0014 said "put it in the tool", and the README of
that tool was taken as that place. It is two command lines, so there was no reason to wrap them in a script.

## What was confirmed by measurement

The behaviour the store layer relies on was run once each with `boto3`. It was a throwaway script and was
not kept in the repo.

| What | Result | Why it was checked |
|------|--------|--------------------|
| Request without credentials | `NoCredentialsError`. The request is not sent | `control_plane.md` 7.6 said "local needs no credentials". **That was wrong.** Fixed |
| Type of a number attribute when read | `Decimal` | The type conversion rule of 6.3 |
| Transaction cancellation reasons | `['None', 'ConditionalCheckFailed', 'None']`. One per item, at the same position as the item put in | The 6.3 cancellation reason table identifies items by position |
| `size(candidates) = :zero` condition | True on an empty list | The 6.3 server reclamation condition |
| `attribute_not_exists(candidates) OR size(candidates) = :zero` | Also true when the attribute is missing | The same condition |
| Turning TTL on | `update_time_to_live` accepts it | 6.2 |

Whether the TTL of DynamoDB local actually deletes items was not checked (ADR 0004 "What could not be
confirmed" item 3).

## What changed

| File | Content |
|------|---------|
| `control_plane.md` 7.6 | The credentials paragraph. The server still takes no variables, the local test harness puts values that are not real keys into the standard `boto3` variables, and the store tests run only when the endpoint is loopback |
| `control-server/README.md` | "Not there yet" changed to a "DynamoDB local" section. Commands, tag, digest, reason for the port choice |
| `plan.md` | Removed item 1 and moved the numbers up. Added the store test harness to the new item 1 |

The loopback check has no code yet. It goes in when the store tests are made. Item 1 of `plan.md` says so.

## Cross-model review

Codex, once. correctness + record bundle. `none` for both lenses.
