# Concurrency, WAL, and Raft Design Review

## Status of This Document

This document resolves the open design questions for the first replicated
StoneleafDB architecture.

The decisions here intentionally keep the first version narrow:

- One database, not a sharded database.
- One server process per node.
- One shared storage engine and buffer pool inside each node.
- Repeatable Read isolation.
- Phantom reads are allowed.
- One Raft group.
- One ordered Raft apply loop per node.
- One storage writer per node.
- STEAL / NO-FORCE local persistence.
- A physical local WAL and a separate logical Raft log.

The following parts of the older replicated-KV roadmap are superseded by this
review:

- Transactions do not use a database-wide
  `SHARED -> RESERVED -> PENDING -> EXCLUSIVE` state machine.
- A writer does not drain every reader before modifying shared pages.
- Ordinary writes do not require transaction-private page copies.
- Transaction isolation uses key locks.
- Safe access to in-memory pages uses short-lived page latches.

The server still takes one OS-level exclusive ownership lock on
`<database>.lock` for its entire lifetime. That lock only prevents two server
processes from opening the same database. It does not coordinate transactions
inside the server.

## Chosen Isolation Level

The first version provides Repeatable Read and permits phantoms.

StoneleafDB will implement this using rigorous two-phase locking:

- A point read acquires a shared (`S`) lock on its exact key.
- A put, update, or delete acquires an exclusive (`X`) lock on its exact key.
- A transaction never needs both modes on the same key: `X(key)` already
  includes permission to read and write that key.
- Both shared and exclusive locks remain held until commit or complete abort.
- Latches are released as soon as the individual B+ tree operation no longer
  needs them.

Holding every granted lock until transaction end, regardless of whether that
lock is `S` or `X`, is sometimes called rigorous 2PL. It is slightly stronger
than the minimum required by strict 2PL, which only requires exclusive locks
to be retained.

A repeated point lookup therefore sees the same value. An exact-key miss also
acquires an `S` lock on that key, so another transaction cannot insert that
exact key until the reader ends. This is a useful and inexpensive guarantee
even though general range phantoms remain allowed.

A scan acquires an `S` lock on each key before returning that key to the
caller. Those locks remain held until transaction end. Another transaction may
still insert a different key into the scanned interval because no gap or range
lock protects the interval. Repeating the scan may therefore return additional
keys.

Allowing phantoms does not permit corrupted, duplicated, or arbitrarily skipped
cursor results. Concurrent split and merge handling still needs a correct
cursor algorithm. For the first version, a scan may hold a shared tree
structure latch for its lifetime while splits and merges take that latch
exclusively. Point operations can still use page-level latch crabbing. A later
version can replace this conservative scan rule with sibling-link validation,
page generations, and restartable cursor traversal.

## Locks, Latches, and Pins

These are three different mechanisms.

### Transaction Locks

A transaction lock protects a logical key and provides isolation across
multiple operations.

Its lifetime is the transaction lifetime:

```text
begin transaction
    -> acquire S locks for keys read but not written
    -> acquire X locks directly for keys written
    -> perform any number of B+ tree operations
    -> make commit durable or finish rollback
    -> release every key lock
```

Lock ownership for one key is represented by its strongest granted mode:

```text
no lock -> S(key)          // read-only use
no lock -> X(key)          // write use; also permits reads
S(key)  -> X(key)          // conversion only if a read later becomes a write
```

The normal `WriteBatch` path knows its write set in advance and therefore
acquires `X` directly. It does not acquire `S` first. If a future interactive
transaction reads a key and later decides to write it, the lock manager needs
an explicit `S -> X` conversion. Concurrent conversions can deadlock, so that
path requires deadlock handling or a rule requiring write intent to be
declared up front.

Transaction locks are owned by transaction IDs, not thread IDs. A request may
be cancelled or eventually resumed by another worker, so a thread is not the
correct durable identity.

### Page Latches

A page latch protects the bytes and metadata of one cached `Page` object while
an operation is examining or changing them.

Its lifetime is normally microseconds and ends with the current tree
operation, not the transaction:

```text
pin cached page
    -> acquire page latch
    -> validate or modify page
    -> release page latch
    -> unpin cached page
```

Page latches have only the modes needed for memory safety:

- Shared/read latch.
- Exclusive/write latch.

They do not have SQLite-style `RESERVED` or `PENDING` modes. A writer-preferring
or phase-fair read/write latch can prevent starvation internally, but that is
an implementation property rather than a transaction-visible lock state.

### Pins

A pin prevents `PCache` from evicting and deleting a cached `Page` object while
a caller uses it. A latch protects the object's contents, but a pin protects
the lifetime of the object itself. A pin alone does not make concurrent access
to the page bytes safe.

Holding a page latch without also holding a pin is invalid. Every latch guard
owns a pin for at least the complete lifetime of that latch:

```text
lookup and increment refs_num while holding the PCache mutex
    -> acquire the cached Page's latch
    -> use the Page
    -> release the latch
    -> decrement refs_num while holding the PCache mutex
```

Eviction also runs under the `PCache` mutex and may select only a page whose
`refs_num` is zero. Consequently, a page cannot be removed from `cache_map` or
deleted while any thread holds either a shared or exclusive latch on it.

This is already the purpose of `Page::refs_num`, `Pager::ref_page()`,
`Pager::unref_page()`, `PCache::pin_page()`, and `PCache::unpin_page()` in the
current implementation. The multithreaded redesign should preserve that model
and make the lookup, reference-count transition, and unpinned-list update
atomic.

## Why Latches Cannot Provide Transaction Isolation

Consider this Repeatable Read transaction:

```text
T1: latch leaf in shared mode
T1: read key "account" = 100
T1: release leaf latch

T2: latch the same leaf in exclusive mode
T2: change "account" to 50
T2: release leaf latch and commit

T1: latch the leaf again
T1: read key "account" = 50
```

Every page access was race-free, but T1 observed two values for the same key.
The latch disappeared between operations, so it did not provide Repeatable
Read. An `S` key lock held by T1 until transaction end would force T2's `X`
key lock to wait.

Latches would be even less suitable if T2 released its page latch before
commit: without the key lock, T1 could observe T2's uncommitted value and later
see it disappear after an abort.

## Why Transaction Locks Cannot Replace Latches

Suppose two transactions operate on different keys that happen to occupy the
same leaf:

```text
T1 owns X("apple")
T2 owns S("zebra")
```

The logical locks are compatible because the keys differ. T1 may nevertheless
split the shared leaf while T2 is reading it. Without page latches, T2 could
observe:

- A new key count with the old cell directory.
- A parent pointer to a new child before that child is initialized.
- A cell array while another thread is moving its elements.
- A cached `Page` object while another thread is evicting and deleting it.

Key locks cannot protect these physical data structures. Page latches make
each physical state transition safe, while key locks preserve the logical
transaction result.

## In-Memory Key Lock Manager

The key lock table should be a sharded hash table, not one permanent lock
object per key in the database.

```cpp
enum class KeyLockMode : std::uint8_t {
    Shared,
    Exclusive,
};

struct KeyLockWaiter {
    TransactionId transaction_id;
    KeyLockMode mode;
    bool granted = false; // Protected by the owning shard mutex.
};

struct KeyLockState {
    // More than one transaction can hold S(key).
    std::unordered_set<TransactionId> shared_owners;

    // At most one transaction can hold X(key).
    std::optional<TransactionId> exclusive_owner;

    // Requests that could not be granted immediately, in arrival order.
    std::deque<std::shared_ptr<KeyLockWaiter>> waiters;

    // Waiters sleep here. They always recheck `granted` after waking.
    std::condition_variable changed;
};

struct KeyHash {
    std::size_t operator()(const Key &key) const noexcept;
};

struct KeyEqual {
    bool operator()(const Key &lhs, const Key &rhs) const noexcept {
        return keycodec::equal(lhs, rhs);
    }
};

struct KeyLockShard {
    std::mutex mutex;
    std::unordered_map<
        Key,
        std::shared_ptr<KeyLockState>,
        KeyHash,
        KeyEqual
    > keys;
};

class KeyLockManager {
  private:
    static constexpr std::size_t SHARD_COUNT = 64;
    std::array<KeyLockShard, SHARD_COUNT> shards;
};
```

### Why These Fields Exist

The repository already implements `Key`; there is no separate `EncodedKey`
type. `Key` contains a `KeyType`, encoded payload size, and canonical payload
bytes. The constructors in `keycodec` already produce the representations used
by the B+ tree:

- Unsigned integers are big-endian.
- Signed integers are sign-bit biased and big-endian.
- Strings and byte keys retain their bytes.
- Boolean keys use one canonical byte.

The lock manager should reuse `Key`, `keycodec::equal()`, and the ordering from
`keycodec::compare()`. It only needs to add `KeyHash`, which hashes the key type
and every byte in `Key::data`. Validated keys with identical type and data must
always produce the same hash. A string key containing the bytes `"7"` must not
conflict with an integer key whose displayed value is `7`.

The four explicit `unordered_map` template arguments above are:

```text
Key                                  key type
shared_ptr<KeyLockState>             value type
KeyHash                              hash function
KeyEqual                             equality function
```

`shared_owners` is a set because several transactions may read the same key:

```text
shared_owners = {T1, T2, T8}
exclusive_owner = none
```

`exclusive_owner` is optional because either nobody owns `X(key)` or exactly
one transaction owns it:

```text
shared_owners = {}
exclusive_owner = T3
```

The manager must never have both a nonempty `shared_owners` set and an
`exclusive_owner`, except transiently inside carefully implemented lock
conversion code. Lock conversion is deferred from the first `WriteBatch`
implementation.

`waiters` contains requests that conflict with current owners or with an
earlier waiter. It is FIFO so an exclusive waiter cannot be starved by an
unending stream of new readers.

`changed` allows blocked transactions to sleep. It is not itself a lock. A
waiting thread calls `wait()` with the shard mutex, which atomically releases
that mutex while sleeping and reacquires it before checking its request again.

The map stores `shared_ptr<KeyLockState>` so the per-key state has a stable
lifetime while a thread is sleeping or waking, even if the map itself grows or
the entry is later removed.

### What a Shard Does

A single mutex around every key would make unrelated lock requests contend.
Instead, the key hash chooses one of a fixed number of shards:

```text
shard_index = KeyHash{}(key) % SHARD_COUNT
```

Each shard owns:

- One short-held mutex.
- A hash table containing only currently active key-lock states assigned to
  that shard.

Two unrelated keys in different shards can be granted concurrently. Two keys
that happen to land in the same shard briefly serialize while their metadata
is inspected, but they do not remain logically locked against one another.
The shard mutex is released before B+ tree access, WAL work, disk I/O, or
transaction execution.

### Shared Acquisition

To acquire `S(key)` for transaction T1:

1. Receive a validated `Key`; the key-value boundary has already converted
   client `KeyInput` using the existing `keycodec` constructors.
2. Hash it to a shard.
3. Lock the shard mutex.
4. Find or create the key's `KeyLockState`.
5. Grant immediately when there is no exclusive owner and no earlier
   exclusive waiter by adding T1 to `shared_owners`.
6. Otherwise append an `S` waiter and sleep on `changed`. The wake-up path
   adds T1 to `shared_owners` before marking the request granted.
7. After immediate or queued grant, record `S(key)` in T1's transaction
   context.
8. Release the shard mutex.

The check for an earlier exclusive waiter matters. Without it, new readers
could continuously bypass a waiting writer.

### Exclusive Acquisition

To acquire `X(key)` for transaction T3:

1. Receive the validated `Key` and locate its shard with `KeyHash`.
2. Lock the shard mutex.
3. Find or create its `KeyLockState`.
4. Grant immediately only when there are no shared owners, no exclusive owner,
   and no earlier waiter; set `exclusive_owner = T3`.
5. Otherwise append an `X` waiter and sleep.
   The wake-up path sets `exclusive_owner = T3` before marking it granted.
6. After immediate or queued grant, record `X(key)` in T3's transaction
   context.
7. Release the shard mutex.

An `X` owner does not also appear in `shared_owners`. Its exclusive ownership
already includes permission to read.

### Fair Wake-Up

When an owner releases a key, the manager examines the front of `waiters`:

- If the first waiter requests `X`, grant only that one request.
- If the first waiter requests `S`, grant the consecutive group of shared
  requests at the front, stopping before the first `X`.

For example:

```text
Current owners: S(T1), S(T2)
Waiters:        X(T3), S(T4), S(T5)

T1 releases:   T3 still waits because T2 remains
T2 releases:   grant X(T3)
T3 releases:   grant S(T4) and S(T5) together
```

This permits concurrent readers without starving the queued writer.

### Transaction-Owned Lock List

Every transaction context maintains the keys and strongest modes it owns:

```cpp
struct HeldKeyLock {
    Key key;
    KeyLockMode mode;
};

struct TransactionContext {
    TransactionId transaction_id;
    std::vector<HeldKeyLock> held_key_locks;
    // WAL and transaction state omitted here.
};
```

This list answers, "What must be released when this transaction finishes?"
Commit, complete abort, cancellation, and connection cleanup all walk this
list and release every lock.

Repeated acquisition is handled by strongest mode:

```text
T owns S(key), requests S(key) -> already satisfied
T owns X(key), requests S(key) -> already satisfied
T owns X(key), requests X(key) -> already satisfied
T owns S(key), requests X(key) -> conversion; deferred initially
```

The initial `WriteBatch` knows its write set before execution, so it acquires
`X` directly and never needs `S -> X` conversion.

### Release and Cleanup

To release a lock:

1. Lock the relevant shard mutex.
2. Verify that the transaction is actually an owner.
3. Remove it from `shared_owners` or clear `exclusive_owner`.
4. Remove the eligible request or shared group from the front of `waiters`.
5. Install those transactions in `exclusive_owner` or `shared_owners`.
6. Mark their waiter objects `granted`.
7. Notify `changed`.
8. If there are no owners and no waiters, erase the key from the shard map.
9. Release the shard mutex.

An empty key state is removed, so memory use is proportional to keys currently
held or awaited, not to the number of keys stored in the database.

Cancellation while waiting removes that transaction's waiter under the shard
mutex and reruns the grant procedure in case it was blocking later requests.

### Deadlock Policy

A multi-key `WriteBatch` sorts keys using `keycodec::compare()` and acquires
`X` locks in that order. Sorting the complete write set prevents deadlocks
between write batches. The initial network API has single-operation point
reads and forward ordered scans, not interactive read transactions that
request arbitrary keys in arbitrary order.

The lock manager should normally block on a condition variable. Repeatedly
trying a fixed number of times wastes CPU and makes outcomes depend on
scheduling. Requests may have a deadline or cancellation token. If the engine
later allows interactive transactions with less predictable lock acquisition,
it must add wait-for-graph deadlock detection or a prevention policy such as
wound-wait.

### What Range Locks Would Look Like Later

Range locks would be ordered by key-space interval, not by acquisition time.
A future manager could maintain an augmented balanced interval tree whose
nodes contain:

```text
[start_key, end_key)
lock mode
transaction ID
maximum end key in subtree
```

The augmented maximum endpoint allows efficient overlap searches. B+ tree
engines also commonly use next-key locks: a record lock combined with a lock
on the gap immediately before it. Both approaches must define conflicts
between point locks and intervals.

None of this is needed for the first version because phantoms are explicitly
allowed.

## In-Memory Latch Placement

StoneleafDB does not need to replace the current page cache with a fixed array
of reusable buffer frames. The existing cache owns heap-allocated `Page`
objects in:

```cpp
std::unordered_map<int, Page *> cache_map;
```

The incremental design adds synchronization and loading state to that existing
model:

```cpp
enum class CachedPageState : std::uint8_t {
    Loading,
    Valid,
    Evicting,
};

struct Page {
    char data[PAGE_SIZE];
    int page_num;
    int refs_num;
    bool is_dirty;
    bool need_flushing;
    Lsn page_lsn;
    CachedPageState state;
    std::shared_mutex content_latch;
    std::condition_variable state_changed;
};
```

`PCache` needs one mutex protecting `cache_map`, `refs_num` transitions, the
unpinned-page list, cache length, and victim selection. A single short-held
mutex is acceptable for the first correct version. It can be sharded only if
profiling later identifies it as a bottleneck.

The current `get()` followed later by `ref_page()` is unsafe with multiple
threads because eviction can occur between those operations. Replace that
sequence with one cache operation that looks up and pins the `Page` while
holding the cache mutex. The returned pointer then remains valid until the
caller unpins it.

On a cache miss, the loader publishes one pinned `Page` with state `Loading`,
releases the cache mutex, and performs disk I/O. Another thread requesting the
same page finds that object and waits for `Valid` instead of loading a duplicate
copy. Disk I/O must not occur while holding the cache mutex.

Callers should receive RAII guards:

- `ReadPageGuard` pins a cached `Page` and owns its shared content latch.
- `WritePageGuard` pins a cached `Page` and owns its exclusive content latch.
- Destruction releases the latch and then the pin.

`PCache` and the pager own cached-page lifetime, pins, and the mechanics of
guards. The B+ tree owns the tree-specific policy: which page to latch, in what
order, when a child is safe, and when an ancestor can be released.

Latch crabbing can temporarily pin an entire unsafe root-to-leaf path plus a
sibling and a newly allocated page. `PCache` must therefore be able to satisfy
that working set. A fixed capacity that returns `NoVictim` when every cached
page is pinned can make a split fail or self-block. The first multithreaded
version should permit bounded temporary cache overflow, or reserve enough
emergency capacity for the maximum structural-operation working set. Waiting
for an eviction is unsafe when the current operation itself owns all pins that
must be released before an eviction can occur.

## Latch Crabbing

### Point Read

For a point read:

1. Acquire the transaction's `S` key lock.
2. Pin and shared-latch the root.
3. Identify the next child.
4. Pin and shared-latch the child.
5. Validate that the parent-to-child relationship is still usable.
6. Release and unpin the parent.
7. Repeat until the leaf is reached.
8. Copy the value out of the leaf.
9. Release and unpin the leaf.
10. Retain the `S` key lock until transaction end.

If a latch is temporarily unavailable, the thread normally waits. A latch
timeout can cancel the operation, but ordinary latch contention should not
abort and roll back a transaction after an arbitrary retry count.

### Insert

An insert acquires `X(key)` even when the key does not exist. The lock prevents
two transactions from concurrently inserting different values for the same
key.

The recommended implementation uses an optimistic first pass:

1. Acquire `X(key)`.
2. Descend with shared latch crabbing, releasing each parent after its child is
   pinned and shared-latched.
3. Keep the leaf pinned, release its shared latch, and acquire its exclusive
   latch.
4. Revalidate the leaf after acquiring the exclusive latch.
5. If the leaf has enough free bytes, modify it and finish.
6. If the leaf is unsafe, release and unpin it.
7. Restart from the root using the pessimistic structural pass described
   below.
8. Release every page latch when this insert operation ends.
9. Retain `X(key)` until transaction commit or abort.

Safety must be based on free encoded bytes, not only key count, because keys
and values have variable encoded sizes.

The pessimistic structural pass pins and exclusively latches pages from the
root downward. After acquiring a child, it releases ancestors above that child
when the child is safe. If every node is unsafe, it reaches the leaf while
retaining a pinned exclusive-latch chain from the root and therefore already
owns every ancestor required for split propagation.

Neither pass climbs upward to acquire a latch. The optimistic pass either
finishes at the leaf or releases everything and restarts. This avoids taking
an exclusive root latch for ordinary leaf-only writes; exclusive ancestor
latches are paid for only when a split can propagate.

There is no reserved root latch, pending child latch, or latch promotion that
drains all readers. The parent is retained only while the current structural
operation could propagate into it. It is never retained until transaction
commit.

During a split, the old child, new child, and affected parent remain latched
until:

- Both child images are valid.
- Their sibling links are valid, if sibling links are used.
- The parent separator and child pointer are installed.

They can then all be released in a documented bottom-up order. If the root
changes, a small root metadata latch protects publication of the new root.

### Update

An update acquires `X(key)` directly. There is no reserved key-lock phase and
no later upgrade.

The first pass uses shared latch crabbing followed by an exclusive leaf latch.
An update cannot automatically be assumed to be non-structural: replacing a
variable-length value may cause the encoded leaf to overflow or may free
substantial space. The safe implementation treats a size-changing update like
a delete followed by an insert inside the same operation. If that can require
structural repair, it releases the leaf and restarts with pessimistic
exclusive latch crabbing.

### Delete

A delete also acquires `X(key)` directly.

The first pass uses shared latch crabbing followed by an exclusive leaf latch.
If deletion cannot underflow the leaf, it finishes there. If the leaf may
underflow, it releases the leaf and restarts pessimistically from the root.
During that structural pass, ancestors above a safe child are released and
unsafe ancestors are retained for borrowing or merging.

The child, chosen sibling, and parent remain latched until the merge or
redistribution and separator update are complete. Those latches are released
after the delete operation; `X(key)` remains until transaction end.

## Visibility of Uncommitted Writes

For the chosen first version, replicated writes are serialized by the Raft
apply loop, but reads may run concurrently. The writer may update shared
cached `Page` objects in place.

The visibility rule is:

- Before changing a key, the apply transaction holds `X(key)`.
- A reader must acquire `S(key)` before looking up that key.
- Therefore, no other transaction can read a key while its value is
  uncommitted.
- Readers of different keys may continue, even when those keys occupy the same
  physical page.
- Page latches ensure those readers see a physically valid tree while a split,
  merge, or page update occurs.

This avoids both transaction-private copies and a database-wide reader drain.
It depends on retaining the first-version single-writer rule. Physical undo is
safe because no second writer can commit a different change to the same page
between the original update and its undo.

If multiple storage writers are introduced later, this design must be
revisited. Physical page undo could overwrite another transaction's committed
changes. A multi-writer design usually needs logical user-level undo,
redo-only system transactions for tree structure changes, stronger lock
hierarchies, or MVCC.

## Operation Summary

### Get

```text
acquire S(key)
    -> traverse with shared latch crabbing
    -> copy result
    -> release all page latches and pins
    -> retain S(key) until commit/abort
```

### Put or Insert

```text
acquire X(key), whether present or absent
    -> shared-latch-crab from root to leaf
    -> exclusively latch and revalidate leaf
    -> modify immediately if leaf is safe
    -> otherwise release and restart with X-latch crabbing
    -> append local WAL before making changed page state stealable
    -> release page latches and pins
    -> retain X(key) until commit/abort
```

### Update

```text
acquire X(key)
    -> shared-latch-crab from root to leaf
    -> exclusively latch and revalidate leaf
    -> restart with X-latch crabbing only if structure may change
    -> log and update
    -> release page latches and pins
    -> retain X(key) until commit/abort
```

### Delete

```text
acquire X(key)
    -> shared-latch-crab from root to leaf
    -> exclusively latch and revalidate leaf
    -> delete immediately if underflow is impossible
    -> otherwise restart with X-latched parent and sibling path
    -> log and update
    -> release page latches and pins
    -> retain X(key) until commit/abort
```

## Local WAL

### When Logging Happens

For an ordinary page change:

1. Acquire the required key lock and page latches.
2. Copy the page's before-image.
3. Construct the after-image.
4. Reserve an LSN and append the update record to the in-memory WAL buffer.
5. Install the after-image in the cached `Page`.
6. Set the page's `pageLSN` to the update record's LSN.
7. Mark the page dirty.
8. Release the page latches.

Appending is not the same as synchronizing. The operation may append while it
holds page latches, but it must not wait for a disk `fsync` while holding them.

The WAL must become durable in two cases:

- Before a database page with `pageLSN = N` is written, WAL must be durable
  through at least `N`.
- Before commit is acknowledged locally, the commit record must be durable.

This is the WAL-before-data rule:

```text
durable_wal_lsn >= page.page_lsn
```

It is what makes STEAL safe. If an uncommitted page reaches the database and
the node crashes, recovery is guaranteed to have the before-state needed to
undo it.

### Physical B+ Tree Changes

The first local WAL should use full physical page before- and after-images.
This is intentionally simple and matches the current pager's before-image
model.

A leaf split may log separate records for:

- The old leaf's new image.
- Initialization of the new leaf.
- The parent separator and child-pointer change.
- A root change, if one occurs.
- Page allocation or freelist metadata changes.

All records belong to one transaction and are connected by `prevLSN`.

Redo installs the recorded after-images when the on-disk `pageLSN` is older
than the record. Undo walks `prevLSN` backward, restores before-images, and
emits compensation log records (CLRs). A CLR describes the physical action
that repeats the undo and carries `undoNextLSN`, allowing recovery to resume
if it crashes during rollback.

For a multi-page split or merge, the B+ tree must keep the affected page
latches until the in-memory tree is structurally valid. Each page change is
logged before that page becomes dirty. No WAL `fsync` is needed inside the
latch critical section.

Full-page logging has high write amplification, but it is the right first
correct implementation. Delta or physiological records can be added after
correctness and benchmarks exist.

### What Is Logical and What Is Physical

The boundary is deliberate:

| Log | Contents | Purpose |
| --- | --- | --- |
| Local WAL | Physical page changes, transaction records, CLRs | Local crash recovery |
| Raft log | Logical `Put` and `Delete` batches | Replicate the state machine |

Physical WAL records are never sent to replicas. Different nodes may assign
different page IDs, split at different moments after snapshot installation,
or have different local LSNs. Replicating physical pages would unnecessarily
couple consensus to one exact storage layout.

## Raft Concepts Needed for This Project

### What Raft Does

Raft makes several nodes agree on one ordered sequence of state-machine
commands despite crashes, lost messages, duplication, and leader changes.

Each node is in one of three roles:

- A follower accepts replication and voting requests.
- A candidate requests votes when it believes the leader has failed.
- A leader accepts client writes and replicates log entries.

Time is divided into monotonically increasing terms. At most one candidate can
win a majority in a term. Each log entry is identified by its term and index.

### What "Raft Log Order" Means

"Raft order" should be called Raft log order.

Within one Raft group, every committed command has a monotonically increasing
log index:

```text
index 41: Put("a", "one")
index 42: WriteBatch[Put("b", "two"), Delete("c")]
index 43: Put("a", "three")
```

Every healthy node applies committed entries in exactly this index order.
That total order is why all replicas reach the same logical key-value state.

An entry may arrive at a follower before it is committed. It becomes committed
only after the leader knows that a voting majority has durably stored it and
Raft's term rules permit advancing the commit index.

### The Raft Apply Loop

Each node has one local apply loop. It does not send entries to followers; the
Raft replication runtime does that.

The apply loop consumes committed entries and applies them to the local
storage engine:

```text
while last_applied < commit_index:
    entry = raft_log[last_applied + 1]
    apply entry as one local storage transaction
    atomically persist:
        key-value changes
        request deduplication result
        last_applied index and term
    last_applied += 1
```

"One ordered apply loop performs local writes" therefore means:

- There is one such loop on every node.
- It processes only committed entries.
- It processes them in increasing index order.
- It is the only component that performs replicated B+ tree writes locally.
- Connection threads propose writes but do not mutate the B+ tree directly.

Serial apply preserves the initial one-writer storage design. Raft networking,
WAL writing, page flushing, checkpointing, and reads may still execute on
other threads.

### What Is Sent to Followers

The leader sends Raft `AppendEntries` RPCs. Conceptually an RPC contains:

```text
group ID
leader term and leader ID
previous log index and term
leader commit index
zero or more entries:
    entry term
    entry type
    versioned logical command bytes
    length and checksum
```

An entry's position supplies its Raft index; the exact on-wire framing may be
owned by the selected Raft library.

The StoneleafDB command payload is a deterministic, versioned encoding:

```text
command version
client ID
request ID
operation count
repeated operations:
    Put: encoded key + encoded value
    Delete: encoded key
payload checksum
```

Keys and values use the canonical StoneleafDB type-tagged encoding. A complete
atomic `WriteBatch` is one Raft entry. Interactive client transactions that
span several network round trips are deferred.

Followers first persist Raft entries in their Raft log. They acknowledge
storage to the leader, learn the advancing commit index, and then make the
committed entries available to their local apply loop. Each apply loop creates
its own physical WAL records while applying the logical command.

### Raft Persistence and Local WAL Are Separate

A node stores two logs:

1. The Raft log records the replicated logical history.
2. The local WAL records physical changes to that node's database pages.

The apparent double logging is acceptable for the first version:

```text
logical command durable in Raft
    -> command becomes committed
    -> local apply produces physical WAL
    -> local WAL commit becomes durable
    -> local last_applied advances atomically
```

If a node crashes during local apply, ARIES undoes an incomplete local
transaction. Because `last_applied` did not advance, the apply loop reapplies
the committed Raft entry after recovery.

### Synchronous Versus Asynchronous Replication

Raft is quorum-synchronous for committed writes:

- In a three-voter group, the leader normally needs durable storage on any two
  voters, including itself.
- It does not wait for every follower.
- Replication to a slow or disconnected remaining follower continues
  asynchronously.

The leader should answer a successful write only after:

1. The entry is committed by a voting majority.
2. The leader's local apply loop has durably applied it.

Returning immediately after merely sending the entry would be asynchronous
replication, not a committed Raft write. Such a response could be lost after a
leader failure even though the client was told it succeeded.

Raft is therefore neither "wait for all replicas" nor "send and immediately
return." It waits for the minimum quorum required for safety.

### Reads

The first version serves linearizable reads through the leader:

1. Use Raft ReadIndex, or an equivalent confirmed current-term barrier.
2. Wait until the leader's local `last_applied` reaches that index.
3. Execute the local read using the transaction key-lock and page-latch rules.

ReadIndex establishes that the node is still the leader and that its state
machine is caught up to an appropriate committed point. It does not replace
local transaction isolation.

## Replicated Transaction Timeline

The first network API exposes atomic `WriteBatch`, not an interactive
distributed transaction.

```text
1. Client sends WriteBatch(client_id, request_id, operations) to leader.
2. Leader validates sizes, encoding, and request identity.
3. Leader checks the durable deduplication state for an already applied retry.
4. Leader encodes the complete batch as one logical Raft command.
5. Leader proposes the command to Raft.
6. Raft appends it locally and replicates it to followers.
7. A voting majority durably stores it.
8. Raft marks the entry committed.
9. Every node eventually exposes that committed entry to its apply loop.
10. The leader's apply loop starts one local storage transaction.
11. It acquires X key locks in canonical key order.
12. It performs B+ tree changes with page latches.
13. Each local page change generates physical WAL.
14. The same local transaction updates the deduplication record and
    last_applied Raft index/term.
15. It appends and synchronizes the local WAL commit record.
16. It releases key locks.
17. The leader returns the stored result to the client.
```

Followers execute steps 9 through 16 independently. They do not send their
physical WAL to one another.

If the client times out, it retries the same `(client_id, request_id)`. The
deduplication record lets the leader return the original result without
applying the command twice.

## Checkpointing

Checkpointing is still required, but fuzzy online checkpointing can be
deferred.

Without checkpoints:

- Recovery eventually scans the entire local WAL.
- The WAL grows for the lifetime of the database.
- Old segments cannot be safely recycled using a simple durable recovery
  anchor.
- A server that stays up for months can fill its disk even if pages are
  regularly flushed.

Taking a checkpoint only when the server restarts does reduce the next restart
time, but it does not bound WAL growth during a long-running process.

The first implementation should use a sharp checkpoint:

1. Pause new Raft application.
2. Finish the current apply transaction.
3. Flush WAL through the selected checkpoint LSN.
4. Flush all committed dirty pages and synchronize the database.
5. Write and synchronize a checkpoint-complete record.
6. Atomically publish the new recovery start point in the control file.
7. Recycle WAL segments older than the published safe point.
8. Resume Raft application.

Run a sharp checkpoint:

- After startup recovery.
- On orderly shutdown.
- Manually for testing.
- When WAL size or dirty-page thresholds are exceeded.

This can briefly stop writes and is acceptable for the first version. Readers
may continue if the buffer and page writer implementation can guarantee a
stable sharp-checkpoint image; otherwise the simplest first checkpoint may
briefly stop all engine operations.

A later fuzzy checkpoint records the transaction table and dirty-page table
without stopping application. It reduces pauses but is not needed to prove the
first correct WAL implementation.

Local WAL checkpoints and Raft snapshots are related but different:

- A local checkpoint bounds physical crash recovery.
- A Raft snapshot lets the group discard old logical Raft entries and bring a
  far-behind follower up to date.

The system eventually needs both.

## Server and B+ Tree Ownership

The process has one logical B+ tree, one pager/buffer pool, one WAL manager,
one transaction manager, and one key lock manager.

Connection threads must not each open an independent `BTree` with an
independent pager or page cache. That would recreate the embedded
multi-process architecture inside one process and produce conflicting cached
copies of the same database pages.

The target shape is:

```text
ServerProcess
├── shared StorageEngine
│   ├── shared BTree
│   ├── shared BufferPool
│   ├── shared KeyLockManager
│   ├── shared TransactionManager
│   └── shared LogManager
├── connection threads
│   └── per-request/per-transaction contexts
└── one Raft apply loop
    └── submits local write transactions to StorageEngine
```

The current `BTree` class is not thread-safe merely because all threads point
to the same object. It owns a `Pager*`, database-open state, and a non-atomic
active cursor count. It must be redesigned so shared engine state is protected
and transaction-specific state is passed explicitly:

```cpp
Result BTree::get(TransactionContext &txn, const Key &key);
Status BTree::put(
    TransactionContext &txn,
    const Key &key,
    const Value &value
);
```

A lightweight `BTreeHandle` per connection is acceptable if it only references
the one shared engine. It must not own a separate cache, pager, root state, or
WAL manager.

A literal thread-per-connection server is simple for an educational first
version, but it needs a hard connection limit because idle or malicious
clients otherwise create unbounded threads and memory use. A bounded worker
pool can replace it later without changing the storage-engine interfaces.

Connection threads may execute reads. Replicated writes are proposed to Raft
and executed only by the apply loop after commitment.

## Final Recommended Invariants

The implementation should assert these rules:

1. One server process owns a database directory.
2. At most one cached `Page` object is the authoritative in-memory copy of a
   page number.
3. Transaction locks are owned by transaction IDs.
4. Point reads hold `S(key)` until transaction end.
5. Writes hold `X(key)` until durable commit or complete rollback.
6. A page is accessed only while pinned and appropriately latched.
7. No page latch is retained until transaction end.
8. No disk, network, quorum, or WAL synchronization wait occurs while holding
   a page latch.
9. WAL is durable through `pageLSN` before a dirty page write.
10. Replicas exchange logical commands, never local physical WAL records.
11. Raft entries are applied once and in increasing committed-index order.
12. `last_applied` changes atomically with the command's local data changes.
13. Only the Raft apply loop performs replicated writes.
14. Old local WAL is recycled only after a durable checkpoint establishes a
    safe recovery point.

## Recommended Implementation Order

1. Make `KeyStore` the complete local key-value boundary.
2. Introduce shared `StorageEngine`, `TransactionContext`, thread-safe
   `PCache`, cached-page guards, pins, and page latches.
3. Implement the sharded key lock manager and Repeatable Read tests.
4. Add latch-crabbed point operations and a conservative structure latch for
   scans and structural changes.
5. Replace rollback journaling with full-page physical WAL and ARIES recovery.
6. Add sharp checkpoints and WAL recycling.
7. Add the bounded single-node client/server API.
8. Add one Raft group and logical `WriteBatch` encoding.
9. Add ordered local apply, durable request deduplication, and ReadIndex reads.
10. Add Raft snapshots and only then revisit finer scan concurrency, fuzzy
    checkpoints, or additional storage writers.
