# Changelog

## 0.2.0 - 2026-09-21

- Add optional per-key TTL with persistent millisecond deadlines.
- Keep existing raw binary values compatible using a separate metadata column family.
- Atomically update values and metadata with RocksDB WriteBatch.
- Protect reads, overwrites and cleanup with striped key locks.
- Add bounded background cleanup with cursor progress and interruptible shutdown.
- Test stale-iterator races, background deletion, clock boundaries, corruption and restart.
- Add Debug/Release GitHub Actions workflow and TTL/project-scope documentation.

Upgrade warning: back up the stopped database before upgrading. Databases opened
by 0.2 gain a metadata column family and cannot be directly reopened by 0.1.x.
TTL requires a 0.2 server; old servers ignore the new optional request field.

## 0.1.0 - 2026-09-20

- Persistent Put/Get/Delete RPC service, binary CLI and recovery tests.
- Multi-threaded benchmark client, WAL comparison runner and reproducible sample.
- Learning guide, practical labs and third-party license inventory.
