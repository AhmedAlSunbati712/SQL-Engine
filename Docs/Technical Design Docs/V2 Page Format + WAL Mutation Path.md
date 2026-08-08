# V2 Page Format and WAL Mutation Path

## Status

This document records the chosen incremental path from the current rollback
journal to a physical write-ahead log. It is authoritative for the V2 page
header, the pager-to-B+ tree boundary, page mutation capture, the first logger,
the rollback-journal transition, and startup recovery sequencing.

The implementation remains on the current rollback journal today. The
standalone V2 page representation and common codec are implemented and tested,
but they are not integrated into the pager or B+ tree. The logger, mutation
boundary, WAL cutover, and WAL recovery remain future work.

`V2` is the name of this migration path and its new source files. It is not a
version number stored in the page bytes.

## Goals

- Keep every persistent database page exactly 4096 bytes.
- Add `pageLSN` and a page checksum before integrating WAL.
- Treat the cached 4096 bytes as the authoritative persistent representation;
  the runtime `page_num` mirror is only a cache lookup key.
- Give the B+ tree all 4096 raw cached bytes, not a pager-owned `PageV2`
  object or separately decoded header fields.
- Replace `begin_write` with a mutation boundary that captures exact physical
  before- and after-images.
- Build and validate the logger independently, then run it beside the rollback
  journal while pager paths are converted one at a time.
- Remove the rollback journal only after WAL-before-data, commit durability,
  startup recovery, and crash tests are complete.
- Do not add a periodic checkpointer during the first WAL cutover.
- Do not make a permanent single-writer assumption in the page format, logger,
  or mutation API. Concurrency control is a separate design task.

## Non-Goals for This Stage

- Thread-safety, page latches, transaction lock ownership, or multi-writer undo
  policy.
- Periodic or fuzzy checkpoints.
- WAL reclamation or bounded startup time.
- Raft, networking, snapshots, or cross-process reader consistency during the
  transitional embedded-engine stage.
- Delta or physiological WAL records.
- Automatic migration of existing database files.

## V2 Page Format

One page is exactly 4 KiB:

```text
4096-byte V2 page
├── bytes 0..23: common persistent header
└── bytes 24..4095: 4072-byte page-kind payload
```

This personal project has one current on-disk format. The page does not carry a
format version, header-size field, page generation, flags, or compatibility
padding. A format change updates the implementation and intentionally makes
older database files unsupported.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Page magic (`SLPG`) |
| 4 | 4 | Page number |
| 8 | 8 | `pageLSN` |
| 16 | 4 | CRC32C |
| 20 | 4 | Page kind |

Persistent integers are big-endian. Page number zero is valid and identifies
the database metadata page. LSN zero means that no WAL update has yet been
assigned. The CRC32C covers only the 4072-byte page-kind payload at bytes 24
through 4095; the 24-byte common header is not included.

V2 page kinds are:

| Value | Kind |
| ---: | --- |
| 1 | `DatabaseMetadata` |
| 2 | `Freelist` |
| 3 | `BTreeInternal` |
| 4 | `BTreeLeaf` |

Zero and unknown kinds are invalid. A live leaf is not converted into an
internal page during ordinary root splitting: the B+ tree allocates a new
internal root. Kind changes normally occur during allocation, free, and reuse.

### Why There Is No Page Generation

V2 relies on monotonic LSN order plus logged allocation and initialization:

- A page number is never reused without `PAGE_ALLOC` and `PAGE_INIT` records.
- `PAGE_INIT` gives the reused page image its record LSN.
- Recovery processes retained physical records in increasing LSN order.
- Redo applies a record only when the persisted `pageLSN` is smaller than the
  record LSN.

An old-incarnation record is skipped when the new incarnation is already on
disk. If the old incarnation is still on disk, ordered redo eventually reaches
the later `PAGE_INIT` record and replaces it. A valid future checkpoint starts
after initialization records it no longer needs to retain.

### Payload Ownership

The common page codec treats bytes 24 through 4095 as opaque. Their meaning is
owned by the page-kind-specific subsystem:

- Page zero uses the database metadata payload codec.
- Freelist pages use the pager's freelist payload codec.
- Internal and leaf pages use the B+ tree page codec.

The exact V2 B+ tree and freelist payload layouts are separate decisions. The
common codec can be implemented and tested before those layouts are finalized.

## Raw Bytes Are the Persistent Source of Truth

The cached page duplicates only the page number as runtime cache identity. Its
serialized page number, kind, `pageLSN`, checksum, and payload remain encoded
in the 4096-byte image and are read and written through `V2PageCodec`.

The pager sets the runtime `page_num` after initializing a new page or
validating a disk-loaded page. It must equal the page number encoded in `data`.
The encoded value remains the persistent authority; the runtime mirror exists
to preserve the current cache's direct page-number lookup behavior.

```cpp
struct PageV2 {
    std::array<char, 4096> data{};

    // Runtime-only cache state. Never serialized.
    std::uint32_t page_num = 0;
    std::uint32_t refs_num = 0;
    bool is_dirty = false;
    bool need_flushing = false;
};
```

`V2PageCodec` provides checked readers, writers, and whole-page validation:

```cpp
namespace V2PageCodec {
    std::uint32_t page_num(std::span<const char, 4096> page);
    std::uint64_t page_lsn(std::span<const char, 4096> page);
    V2PageKind page_kind(std::span<const char, 4096> page);

    void set_page_lsn(std::span<char, 4096> page, std::uint64_t lsn);
    void set_page_kind(std::span<char, 4096> page, V2PageKind kind);
    void update_checksum(std::span<char, 4096> page);
    V2PageCodecResult validate(std::span<const char> page);
}
```

The codec validates exact size, magic, page kind, and payload CRC32C before the
pager exposes a disk-loaded page. The pager separately compares the decoded
page number with the page number it requested from disk. The common-header
`pageLSN` does not receive checksum protection in this format.

## Pager-to-B+ Tree Boundary

`PageV2` remains internal to the pager and cache. The B+ tree never owns or
receives that cache object, and the pager does not separately return decoded
header fields. A pinned read exposes one read-only
`std::span<const char, 4096>` over the complete cached page. A mutation exposes
one mutable `std::span<char, 4096>` over the same complete page.

`BTreePage` receives all 4096 raw bytes. It uses `V2PageCodec` to read and
validate the common page kind from those bytes, then interprets bytes 24
through 4095 as its B+ tree payload. It can change the page kind through the
codec when a deliberate reinitialization requires that. The pager does not
pass page number, page kind, `pageLSN`, CRC, and payload as separate values.

This does not remove checking responsibilities from `BTreePage`.
Common-header validation by the pager is additive; `BTreePage` still rejects
an invalid B+ tree kind, malformed cell layout, impossible offsets, and other
B+ tree page corruption.

The full-page span is non-owning. Its writes modify the cached bytes directly,
and it becomes invalid when the owning pin or mutation ends.

The existing `BTreePage` assumes its B+ tree type byte is at offset zero, so it
cannot consume the V2 page unchanged. Its V2 adaptation must preserve the
4096-byte interface while shifting page-specific decoding to byte 24 and
recalculating capacity. That remains a later, explicit migration step; the
standalone page codec does not modify the legacy B+ tree.

## Page Mutation Boundary

`begin_mutation` is the intended replacement for `begin_write`. During the
incremental migration, both entry points coexist so unconverted code keeps the
rollback-journal behavior unchanged. Each converted call site uses a complete
`begin_mutation`/`finish_mutation` pair; after the last conversion,
`begin_write` is removed.

### Mutation Object

One mutation owns one additional 4 KiB before-image. The cached page itself is
the working after-image; there is no separate after-image array.

```cpp
class PageMutationV2 {
  public:
    PageMutationV2(const PageMutationV2 &) = delete;
    PageMutationV2 &operator=(const PageMutationV2 &) = delete;
    PageMutationV2(PageMutationV2 &&) noexcept;
    PageMutationV2 &operator=(PageMutationV2 &&) noexcept;
    ~PageMutationV2();

    std::span<char, 4096> bytes();

  private:
    friend class Pager;

    std::span<char, 4096> cached_bytes;
    std::array<char, 4096> before_image{};
    bool finished = false;
};
```

The sketch omits small lifecycle fields such as the prior dirty state.
Crucially, the mutation does not contain a `PageV2*`, `PageImageV2`, copied
header fields, or a second after-image. `cached_bytes` points directly at the
pager's cached 4096 bytes, `before_image` is the one owned copy, and `bytes()`
is exactly what the pager returns to the B+ tree.

The B+ tree uses the common codec on `bytes()` when it needs to inspect or
change the page kind. `finish_mutation` owns assignment of `pageLSN` and the
checksum. It also validates that immutable identity bytes such as magic and
page number were not accidentally changed and that the encoded page number
still equals the owning cache page's runtime `page_num`.

The mutation keeps the page pinned until it finishes or cancels. Concurrency
protection will be added when the separate concurrency design is implemented.

### Beginning a Mutation

`begin_mutation(page_num)`:

1. Finds and pins the cached page.
2. Performs the existing transaction-start and rollback-journal first-write
   bookkeeping while the rollback journal remains active.
3. Copies the current 4096 bytes into `before_image`.
4. Records enough prior runtime dirty state to restore it on cancellation.
5. Returns a move-only mutation whose full-page span targets the cached bytes.

The rollback-journal image and mutation before-image have different meanings:

- The rollback journal retains the page's first image in the transaction.
- Each WAL mutation retains the page image immediately before that physical
  change.

### Finishing a Mutation

After the B+ tree modifies the raw cached bytes, `finish_mutation`:

1. Reserves the `PAGE_UPDATE_FULL` record LSN.
2. Writes that LSN into the cached page's `pageLSN` field.
3. Recomputes the cached page checksum.
4. Appends a record containing `before_image` and the current cached 4096 bytes.
5. Marks the cached page dirty.
6. Marks the mutation finished and releases its mutation pin.

Appending copies the complete record into logger-owned memory before
`finish_mutation` returns. The cache may change again afterward, so the logger
must not retain a span into the cached page as the durable after-image.

Appending does not synchronize the WAL. Commit and cache-spill paths perform
the required durability waits later.

If record reservation or append fails, `finish_mutation` restores the complete
`before_image`, including the old kind, `pageLSN`, checksum, and payload, and
restores the prior runtime dirty state. An unfinished mutation's destructor
performs the same restoration so early returns cannot leave an unlogged page
change in the cache.

### Repeated Mutations

Every finished physical change receives its own record:

```text
R1: X   -> X_m
R2: X_m -> X_mm
```

The second `begin_mutation` copies the already-installed `X_m` image. Redo in
LSN order produces `X_m` and then `X_mm`. Undo follows the transaction's
`prevLSN` chain in reverse and restores `X_m` and then `X`. Recovery must not
skip repeated page numbers with a hash set.

### Allocation, Free, and Kind Changes

Allocation accepts the desired page kind. Whether the page comes from file
extension or the freelist, the pager initializes the cached header and payload
through a mutation before returning the mutation's full 4096-byte span to the
B+ tree.

Freeing a page similarly uses a mutation to change the kind to `Freelist` and
write the freelist payload. A rare deliberate reinitialization uses
`set_page_kind()` on the mutation and rebuilds the payload before finishing.
Header and payload changes therefore appear in one full-page after-image and
one eventual 4096-byte database write.

## Logger Module

The logger is implemented as an independent module before it becomes the
authoritative recovery mechanism.

WAL lives in a log directory. Each segment has:

- A store containing length-prefixed encoded records.
- An index mapping each relative LSN to its store offset.
- A base LSN persisted in segment metadata and reflected in its file identity.
- Configured store and index size limits.

The store is an append-only sequence of independently framed records. Each
frame begins with a four-byte unsigned big-endian payload length followed by
exactly that many opaque payload bytes. The store owns its file descriptor,
uses positional I/O, and returns the length-prefix offset from append for the
index to persist.

On open, the store scans framing through the final complete record. A partial
length prefix or partial payload at the physical end of the file is reported
as an incomplete tail. The caller must explicitly repair that tail by
truncating to the reported last-valid boundary before another append. This
framing scan cannot distinguish a corrupted length value from a genuinely
truncated payload; the WAL record codec supplies the magic, checksum, type,
and LSN validation needed to classify interior corruption.

Absolute LSN is:

```text
absolute LSN = segment base LSN + relative LSN
```

Each authoritative store record encodes its relative LSN so the index can be
rebuilt. Absolute LSN zero remains the "none" value used by an untouched
page's `pageLSN`, so the first segment base and initial `next_lsn` are one.
Relative LSN zero is the first record in any segment. When a segment is empty,
its next absolute LSN is its base LSN. Otherwise:

```text
next LSN = segment base LSN + last valid relative LSN + 1
```

On rollover, the new segment's base LSN is the logger's current `next_lsn` and
its first record again has relative LSN zero.

The logger completes both the store-record append and its corresponding index
entry before testing the active segment's limits. If either resulting file
size exceeds its configured limit, that complete record remains in the active
segment and the next record begins a new segment. Records are never divided
between segments. `max_index_bytes` must be an integer multiple of the fixed
index-entry width; configuration validation is added when that entry format is
finalized. The store is the correctness authority. The index is validated
against the store and must be rebuildable after a torn or incomplete index
write.

The log manager is the owner of LSN allocation. At runtime it holds
`next_lsn`, `written_lsn`, and `durable_lsn`. It reconstructs `next_lsn` at
open by scanning the authoritative store through the final valid record; it
does not read that value from a database page and does not maintain a second
persisted "current LSN" counter that could disagree with the WAL.

Every database page, including page zero, still has its own `pageLSN`. Page
zero's value means only "the newest WAL action reflected in this metadata page
image." It is not the global last or next LSN. The WAL segment base plus the
last valid relative LSN is the persistent source from which the log manager
reconstructs the allocator.

The logger abstraction also owns existing segments, the active segment,
record append, sequential scan, record lookup needed by `prevLSN`, and
durability tracking.

The first physical record family includes transaction boundaries, full-page
updates, allocation, initialization, free, and the metadata/freelist page
updates needed to make those operations reversible. Exact record headers and
payload layouts are finalized with the logger codec rather than inferred from
the current rollback journal.

## Incremental Rollback-Journal-to-WAL Transition

### Stage 1: V2 Page Format — Complete

Implement V2-named page types, the common page codec, CRC32C, corruption tests,
and raw full-page views without changing the legacy pager or B+ tree.

### Stage 2: Standalone Logger — Next

Implement store/index segments, LSN allocation, append, rollover, scan,
lookup, durability tracking, and torn-tail tests without pager integration.

### Stage 3: Shadow Mutation Logging

Add `begin_mutation` and `finish_mutation` beside `begin_write`. Convert pager
and B+ tree modification paths one at a time. Converted operations append WAL
records, while the rollback journal remains the authoritative commit and crash
recovery mechanism.

The transitional engine is intentionally awkward: it may force database pages
according to the rollback-journal policy even though it also emits WAL. That
temporary behavior is removed only after all physical mutations are logged.

### Stage 4: WAL Cutover

After allocation, free, metadata, root, leaf, internal, split, merge, and file
length changes all emit complete WAL records:

- Remove `begin_write` and make `begin_mutation` the only page-write boundary.
- Append and synchronize `TXN_COMMIT` before acknowledging commit.
- Stop forcing dirty database pages at commit.
- Before any dirty page write, make WAL durable through that page's `pageLSN`.
- Permit uncommitted dirty pages to reach disk under STEAL.
- Remove rollback-journal commit, abort, and hot-journal recovery only after
  equivalent WAL crash tests pass.

## Startup Recovery Without Regular Checkpoints

The first WAL cutover has startup recovery but no periodic checkpointer. The
component is a recovery manager, even if it shares low-level page-application
helpers with a future checkpointer.

On database open, before accepting embedded callers, recovery:

1. Scans retained WAL from the first segment.
2. Reconstructs transaction state and per-transaction `lastLSN` values.
3. Redoes required physical records in LSN order when the persisted page LSN is
   smaller than the record LSN.
4. Undoes loser transactions through their `prevLSN` chains using full
   before-images.
5. Synchronizes required recovery output before opening the database.

The exact concurrent-writer undo and compensation-record policy is a separate
decision that must be complete before concurrent writers are enabled. The
page format, logger, and mutation API must not encode an exactly-one-writer
restriction.

Without regular checkpoints:

- WAL is not reclaimed.
- Recovery scans the complete retained history.
- Startup time and disk usage grow over time.

Those costs are accepted for the initial cutover. Periodic checkpointing,
checkpoint metadata, and safe segment reclamation are a later milestone.

## Durability Rules

- `finish_mutation` appends but does not synchronize the WAL.
- A cached after-image is never written to the database before WAL is durable
  through its `pageLSN`.
- Commit is not acknowledged before its commit record is durable.
- Database pages are not forced at commit.
- WAL append buffers must own record bytes; they cannot retain spans into
  mutable cached pages.
- A failed or abandoned mutation restores its before-image before releasing
  its pin.

## Immediate Implementation Order

Stage 1's common V2 page representation, codec, CRC32C implementation, and
corruption tests are complete. They remain standalone and do not modify the
legacy pager, B+ tree, rollback journal, or transaction flow.

The next bounded task is Stage 2's standalone logger. Mutation integration
remains a later, separately tested task. No replacement `BTreePageV2` or legacy
pager/B+ tree adaptation is part of the logger milestone.
