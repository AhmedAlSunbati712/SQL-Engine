# StoneleafDB

StoneleafDB is a personal systems project centered on building a crash-safe distributed transactional storage engine in C++ from first principles.

The long-term goal is not just to build a local storage engine. The goal is to build the storage and commit path of a distributed database, starting from the lowest-level durability and recovery mechanisms and then layering replication and distributed coordination on top.

The project is inspired by SQLite on the single-node side, but the target system is broader: a transactional storage engine with a replicated commit plane, fault-tolerant recovery, and room for later work on partitioning and distributed execution.

## Project Focus

This project is organized around two layers that have to fit together cleanly.

- A single-node storage engine that handles pager behavior, caching, journaling, durability, and recovery
- A distributed coordination layer that replicates the commit path and keeps writes durable and ordered across nodes

The current implementation is concentrated on the first layer because the distributed layer is only interesting if the local recovery path is solid.

## Current Implementation

The codebase currently implements the core storage-engine pieces needed before replication can sit on top.

- Pager and page-cache management
- Pinned vs unpinned page tracking
- Cache-coherent LRU eviction
- Dirty-page tracking
- Checksummed rollback journaling
- Two-phase commit flow
- Hot-journal recovery
- Disk I/O and journal encoding primitives

These components are the base that a distributed commit layer would rely on. The point of this phase is to make local commit, rollback, and recovery precise before adding replication, failover, and distributed durability semantics.

## Distributed Architecture

The distributed direction of the project is explicit.

- Add WAL and checkpointing so the storage engine has a stronger write and recovery path
- Add a replicated commit plane backed by Raft for log replication, leader election, failover, and recovery under node crashes
- Keep replication and commit coordination separate from the lower-level pager and cache logic so the system has clean subsystem boundaries

Sharding is a later step. It belongs after the replicated commit path is stable, because partitioning adds a different class of problems around routing, ownership, rebalancing, and cross-shard semantics.

## What Is In The Repo

- [Docs/System Constraints](Docs/System%20Constraints): high-level scope and constraints for each subsystem
- [Docs/Technical Design Docs](Docs/Technical%20Design%20Docs): lower-level design notes, invariants, and control flow
- [src](src): implementation
- [include](include): public headers
- [tests](tests): unit tests

## Current Status

At the moment, the repository contains and tests the low-level engine components that the distributed system will build on.

- Pager
- Page cache
- Doubly-linked LRU structure
- Disk I/O helpers
- Endian utilities
- Journal codec

The next major steps are broader end-to-end recovery tests, WAL/checkpointing, and the replicated commit path.

## Build And Test

```sh
make test
```

## Design Notes

This repository is intentionally design-first. The implementation follows the invariants and control-flow notes in the docs rather than growing as an unstructured code prototype.

That is also why the work starts with pager, recovery, and durability concerns. In a distributed database, replication cannot rescue a weak local storage engine. The single-node commit path has to be correct before the distributed path can be trusted.
