# B-Trees and How Databases Store Data on Disk

Over the last couple of months, I have been spending a lot of time reading
about databases and building one myself. Honestly, there is no grand reason
behind it other than technical curiosity.

The project is currently a crash-safe, transactional, ordered key-value store.
I am planning to move it toward a client-server architecture next, but I am
pretty happy with how far the embedded version has come.

I wanted to document the process of building it, but my notes slowly turned
into a disorganized mess of Markdown files in the project repository. So I
figured I might as well turn them into a series :)

I am not going to spend much time explaining why databases exist. I will assume
the reader is already familiar with DBMSs at a high level. This series is more
about what is happening inside the storage engine, mixed with implementation
notes and things I learned while building my own.

## 1. The Problem We Are Trying to Solve

Let's start with a basic question: how should a storage engine organize its
data?

For now, it does not really matter whether we are building an SQL database or a
key-value store. At the storage layer, we need some way to associate keys with
records and find or modify them without scanning the entire database.

There are many ways to do this, but two useful starting points are hash indexes
and B+ trees.

- **Hash index:** We hash the key and use the result to find its bucket. Exact
  lookups are very fast on average, usually `O(1)`. Collisions still have to be
  handled, but they are not the main problem here. The bigger limitation is
  that hashing destroys the ordering between keys. If we ask for every key
  between `100` and `200`, the hashes do not tell us where that range starts or
  ends. We may have to scan the whole index.
- **B+ tree:** Entries stay ordered by key. A lookup is `O(log n)` instead of
  `O(1)`, but the tree is wide enough that `log n` ends up being very small.
  Keeping the keys ordered also makes range queries and ordered scans much
  easier.

Talking about key-value pairs here does not mean the database itself has to
expose a key-value API. Relational engines can use the same idea internally.
SQLite's ordinary rowid tables, for example, use a signed 64-bit `rowid` as the
B-tree key and store the encoded table row as its payload. The `rowid` is
normally an internal identifier, although an `INTEGER PRIMARY KEY` can be an
alias for it.

I am going to focus on B+ trees because that is the storage structure I
implemented and the one I am most familiar with. The easiest place to start is
with their much narrower relative: the binary search tree.

## 2. A Quick Refresher on Binary Search Trees

Nothing too deep here, just enough so that we can see what B+ trees do
differently.

A binary search tree is made up of nodes, and each node has a key and at most
two children. The rules are:

- Everything to the left of a node is smaller than it.
- Everything to the right is greater than it.

So let's say we insert `8`, `3`, `10`, `1`, `6`, and `14`. The tree might look
something like this:

```text
        8
       / \
      3   10
     / \    \
    1   6    14
```

If we are looking for `6`, we start at `8`. `6` is smaller, so we go left to
`3`. It is greater than `3`, so we go right, and there it is.

Inserting a new key is pretty much the same process. We keep going left or
right until we find an empty spot and put it there. Deletion is a little more
annoying because we have to handle nodes with zero, one, or two children, but
the basic ordering rule is still the same.

Assuming the tree is balanced, searching, inserting, and deleting are all
`O(log n)`. A balanced tree with around one million keys would only be around
20 levels tall, which sounds pretty good.

The key word there, though, is *balanced*. If we insert already sorted keys into
a basic binary search tree, we can end up with a pathological structure that looks something like this:

```text
1
 \
  3
   \
    6
     \
      8
```

This is basically a linked list pretending to be a tree. Searching it is now
`O(n)`, which is obviously not great. There are trees like AVL and red-black
trees that rebalance themselves and avoid this problem.

So if we already have balanced binary trees with `O(log n)` reads and writes,
why not just use one of those for a database?

## 3. Why Binary Trees Are a Poor Fit for Disk

There is nothing wrong with binary trees if the whole thing lives in
memory. Following a pointer to the left or right child is cheap. But databases
usually have more data than they can fit in memory, which means that some of
the tree has to spillover onto disk.

This changes the problem quite a bit. Normal memory pointers are meaningless
once the process exits, so we cannot store them on disk and expect them to
still work later. Instead, internal nodes store child page numbers.

The pager uses that number to find the page in the database file. If the page
is not already cached, it has to be read into memory first. A spinning disk may
physically seek to it, while an SSD has no moving head, but either way we are
doing an I/O operation before the tree can continue.

There are two differences that matter here: how long an access takes and how
much data gets moved around.

Here are some very rough latency numbers:

| Storage | Rough random-read latency | Dependent reads per second |
| --- | ---: | ---: |
| Main memory | around 100 ns | around 10 million |
| Low-latency NVMe SSD | around 20 us | around 50,000 |
| SATA SSD | around 100 us | around 10,000 |
| Spinning HDD | around 12 ms | around 80 |

These are not maximum throughput numbers or universal benchmarks. Storage
devices can process more requests when many of them are queued at once. A tree
lookup is different because each page tells us which page to read next, so the
reads are dependent and their latencies mostly add up.

The unit of transfer is different too. From the program's point of view, we can
ask for two bytes from memory and get those two bytes back. Underneath that,
the CPU will usually fetch a whole cache line if the data is not already
cached.

Files work through even larger units. We can call something like
`pread(fd, buffer, 2, offset)` and ask for two bytes, but the OS and storage
device still work with filesystem blocks, memory pages, and device sectors.
The OS will usually keep the extra data in its page cache in case we need it
again.

My engine makes this unit explicit by reading and writing 4 KiB database
pages. Now imagine putting one binary tree node in each page. We would read 4
KiB just to get one key and two child page numbers. Most of the page would be
wasted.

We could pack several binary tree nodes into one page, but then we would be
managing a collection of separately connected nodes inside it. At that point,
it makes more sense to use a tree designed around the page itself and let one
node hold many keys and children.

The other issue is the height. Our tree with one million keys was around 20
levels tall. If every node we need happens to be on a different page, a single
lookup could need around 20 page reads. Some of those pages will probably be
cached in memory, especially the ones near the root, but we still want to avoid
going through that many pages in the first place.

This is one of those cases where Big O notation doesn't really tell the whole
story. A binary tree and a B+ tree both have `O(log n)` lookups, but reading
another page is way more expensive than doing a few extra comparisons inside a
page. The `c` in the time complexity on disk is big enough that we want to minimize
the log term as much as possible.

So instead of putting one key in a node, why not fill the node with as many
keys and child page numbers as we can? A node might now have 100 children
instead of two. That makes the tree much wider and, more importantly, much
shorter. The same million keys that needed around 20 levels in a binary tree
might only need a few levels in a B+ tree.

That is basically the reason we need B-trees. Their nodes are shaped around how
the database actually reads data. We do more comparisons inside each node, but
we read far fewer pages from disk. Damn good deal.

## 4. The B+ Tree Structure

Before going any further, let's look at what a B+ tree actually looks like.
There are two kinds of nodes:

- **Leaf nodes:** These hold the actual key-value pairs and don't have any
  children. You can think of them as packing a bunch of binary search tree
  entries into one node.
- **Internal nodes:** These don't hold values. They only hold separator keys
  and child page numbers that tell us where to continue searching.

Here is a small example:

```text
                         Internal node
                       [   20  |  40   ]
                         /     |      \
                        /      |       \
             keys < 20   20 <= keys < 40   keys >= 40
                    /           |             \
                   v            v              v
              [ 5:a  12:b ] [ 20:c  31:d ] [ 40:e  57:f ]
                  leaf            leaf             leaf
```

The separator `20` tells us that everything in the child to its left is less
than `20`, while the next child starts at `20`. The same applies to `40`.
Notice that the separator keys are still in the leaves. The internal node just
keeps copies of them so it can route searches.

More generally, if an internal node has separator keys `k1, k2, ..., kq` and
children `c0, c1, ..., cq`, then:

- Keys in `c0` are less than `k1`.
- Keys in `ci` are greater than or equal to `ki` and less than `k(i+1)`.
- Keys in the last child, `cq`, are greater than or equal to `kq`.

Another important number is the tree's **order**. The terminology is a bit
annoying because different sources define it differently. Here, I am using the
same definition as my implementation: an order `n` tree can have at most `n`
children in an internal node.

That gives us the following invariants:

- Every internal node with `q` separator keys has exactly `q + 1` children.
- An internal node can have at most `n - 1` separator keys and `n` children.
- A leaf can have at most `n - 1` key-value pairs.
- Every non-root node must have at least `ceil(n / 2) - 1` keys. For an
  internal node, that also means at least `ceil(n / 2)` children.
- The root is the exception. A root leaf can have fewer than the normal minimum.
  If its last key is deleted, my code removes that page and the tree has no
  root at all. A root internal node only needs one separator key and two
  children.
- All leaves are at the same depth. This is what keeps the tree balanced.

My implementation currently uses an order of `200`. So a non-root node holds
between `99` and `199` keys, while a non-root internal node has between `100`
and `200` children. Insertions and deletions can temporarily break these
limits, but the operation has to repair the tree before it is done.

## 5. Searching a B+ Tree

Searching the tree is pretty straightforward. We start at the root and keep
moving down until we reach a leaf. This could be written recursively, but I
implemented it with a loop in `descend_from_root_to_leaf` in `BTree.cpp`.

The code roughly does this:

```text
current = root

while current is not a leaf:
    i = index of the first separator greater than or equal to the target

    if there is no such separator:
        next = rightmost child
    else if separator[i] equals the target:
        next = child to the right of separator[i]
    else:
        next = child to the left of separator[i]

    if the caller needs the path:
        save the current page and chosen child

    current = next

search for the target inside the leaf
```

Equality goes right because a separator is the smallest key in its right
subtree. The saved path is not really needed for a normal lookup, but insertion
and deletion need it in case they have to repair parent nodes later.

### How Much Does the Wider Tree Actually Help?

Let's use an order of `100` here because it makes the math easier, even though
my implementation currently uses `200`. Each internal node can point to 100
children, and each leaf can hold up to 99 key-value pairs.

With five levels, a completely full tree can hold:

```text
99 * 100^4 = 9.9 billion keys
```

So one billion keys fit in a tree whose root-to-leaf path is about five pages.
If the pages are closer to their minimum occupancy, it may take six, but let's
use five for a simple comparison.

We also do a binary search inside every page. Searching around 100 keys takes
at most about `ceil(log2(100)) = 7` comparisons. Across five pages, that gives
us roughly 35 key comparisons.

Now compare that with a balanced binary tree:

```text
ceil(log2(1 billion)) = 30 levels
```

That is around 30 comparisons, which is actually a little fewer than the B+
tree. The problem is that, if each binary tree node lives on a separate page,
it can also mean 30 dependent page reads instead of five.

Here is a rough comparison assuming none of the pages are already cached:

| Structure | Page reads | Key comparisons | HDD | SATA SSD | Low-latency NVMe SSD |
| --- | ---: | ---: | ---: | ---: | ---: |
| B+ tree | 5 | about 35 | about 60 ms | about 0.53 ms | about 0.10 ms |
| Binary tree | 30 | about 30 | about 360 ms | about 3.18 ms | about 0.60 ms |

These are just example numbers, not benchmarks. I am assuming roughly 12 ms
for a random HDD read, 106 microseconds for a SATA SSD read, and 20
microseconds for a low-latency NVMe SSD read.

In a real database, the root and some internal pages will probably already be
cached, so both trees may need fewer disk reads than this. The exact timings
will also depend on the device and workload. But the main point doesn't
change: a B+ tree is happy to do a few extra comparisons if that saves a large
number of page reads.

## 6. Insertion

Now that we know how to find a leaf, insertion starts off pretty simply:

1. Traverse the tree and keep the path we took.
2. Do a binary search inside the leaf.
3. If the key already exists, replace its value.
4. Otherwise, insert the new key-value pair in sorted order.

If the leaf still fits within the tree's order, we are done. The interesting
part starts when it overflows.

Let's use a tiny tree with a maximum of three entries per leaf. If we insert
`15` into the following full leaf, it temporarily has four entries:

```text
Before: [ 5:a | 12:b | 20:d ]

Insert: [ 5:a | 12:b | 15:c | 20:d ]
```

We split it into two leaves and copy the first key in the new right leaf into
the parent:

```text
                         [ 15 ]
                         /    \
                        /      \
           [ 5:a | 12:b ]    [ 15:c | 20:d ]
```

The separator `15` stays in the right leaf because the actual record belongs
there. The copy in the parent is only used for routing.

If the parent has room, that is the end of it. If the parent is also full, it
has to split too. Internal-node splits are slightly different from leaf
splits. We promote the middle separator into the parent above, but this time
the promoted key is removed from the two nodes being split. Internal keys are
routing boundaries, not records that need to remain in a leaf.

This can keep moving up the path we saved during traversal:

```text
leaf overflows
    -> split leaf
    -> insert separator into parent
    -> parent overflows
    -> split parent
    -> insert separator into grandparent
    -> ...
```

There is one final case. If the root splits, there is no parent to receive the
separator. We allocate a new internal root, place the separator in it, and make
the two split pages its children. This is the only way insertion increases the
height of the tree.

In my implementation, the maximum is `199` keys per node. An insertion may
temporarily bring that count to `200`, and then `propagate_splitting` repairs
the tree on its way back up.

## 7. Deletion

Deletion is where the tree gets a bit more annoying.

We start the same way as before: find the target leaf, remove the key-value
pair, and check how many entries are left. If the leaf is still above the
minimum, there is no structural work to do.

There is still one detail to watch out for. If we delete the first key in a
leaf, the separator that represents that leaf may now be wrong. The new first
key has to be copied into the appropriate ancestor.

The harder case is when the leaf falls below its minimum occupancy. We have
two ways to repair it: borrow or merge.

### Borrowing From a Sibling

If a sibling has more than the minimum number of entries, it can spare one.
For example, assume each non-root leaf needs at least two entries:

```text
Parent separator:                 [ 20 ]

Before deletion repair:
    left leaf                     [ 10 ]
    right leaf                    [ 20 | 30 | 40 ]

Borrow 20 from the right:
    left leaf                     [ 10 | 20 ]
    right leaf                    [ 30 | 40 ]
    new parent separator:         [ 30 ]
```

The parent separator changes because the smallest key in the right leaf is now
`30`. Borrowing from the left works in the other direction: take its last
entry, put it at the start of the current leaf, and update the separator for
the current leaf.

### Merging With a Sibling

If neither sibling can spare an entry, we merge:

```text
Parent:                    [ 20 ]
                           /    \
                          /      \
Leaves:              [ 10 ]    [ 20 | 30 ]

After merge:          [ 10 | 20 | 30 ]
```

One leaf survives, the other page is freed, and the parent loses the separator
that used to sit between them. That may now leave the parent below its own
minimum, so the same repair can continue upward.

Internal nodes follow the same general borrow-or-merge idea, but the movement
is different. A separator from the parent moves down into the underfull node,
and a separator from the sibling moves up to replace it. During a merge, the
parent separator between the two nodes moves down and joins the merged node.

Eventually we may reach the root. If an internal root loses its last separator,
it only has one child left, so that child becomes the new root. The tree gets
one level shorter. If the root is a leaf and its final key is deleted, my
implementation frees that page and marks the tree as empty.

So insertion grows the tree by splitting the root, while deletion shrinks it
by collapsing the root. Everything else is mostly the tree trying to keep its
occupancy rules intact.

## 8. B-Trees and B+ Trees

I have used the terms B-tree and B+ tree a lot here, so it is worth being a bit
more precise.

In a traditional B-tree, values may be stored in both internal nodes and
leaves. In a B+ tree, values only live in the leaves. Internal nodes contain
separator keys and child page numbers.

This gives the internal nodes more room for routing information, which
increases fanout. It also means every lookup follows the same general path to a
leaf instead of sometimes finding the value in an internal node.

Many B+ tree implementations also link neighboring leaves together. That makes
range scans very convenient: find the first leaf, then keep walking through
the sibling links. My current leaf format does not have those links. The cursor
keeps its traversal path, walks back up until it finds an unvisited subtree to
the right, and then descends to that subtree's leftmost leaf. It works, but
leaf links are something I may add later.

From this point onward, whenever I say B-tree while discussing my project, I
really mean the B+ tree variant.

## 9. From Logical Nodes to Fixed-Size Pages

So far, the diagrams have made nodes look like abstract boxes. On disk, each
one is a 4 KiB page.

Every page has a page number. To find its byte offset in the database file, the
pager does:

```text
file offset = page number * 4096
```

This is why child references in an internal node are page numbers instead of
memory pointers. A pointer only makes sense inside one running process. A page
number still means the same thing after the process exits and the database is
opened again.

There is also a difference between a page on disk and the `Page` object used by
the program. The in-memory object has extra bookkeeping such as its reference
count, dirty flag, and whether it still needs to be flushed. Those fields are
for the pager and page cache. Only the 4096-byte data buffer is part of the
database format.

I also don't write C++ structs directly to disk. Compiler padding, machine
endianness, and later changes to a struct could all silently change the file
format. Instead, every fixed-width number is encoded explicitly in big-endian
order, and every field has a known offset.

## 10. The Overall Database File Layout

The database file itself is just a sequence of 4 KiB pages:

```text
Database file

page 0          page 1          page 2          page 3          ...
+-----------+   +-----------+   +-----------+   +-----------+
| database  |   | internal  |   | leaf      |   | free page |   ...
| header    |   | B+ page   |   | B+ page   |   |           |
+-----------+   +-----------+   +-----------+   +-----------+
```

Page `0` is special. It contains the database header:

- A magic string used to recognize the file.
- A file-change counter.
- The total number of pages in the database.
- The first page in the freelist and the number of free pages.
- The page number of the B+ tree root.

Every page after that can be an internal page, a leaf, or a free page waiting
to be reused. A page does not keep one role forever. A leaf freed by a merge
may later be reused as an internal page after a split.

The root is also not required to be page `1` or any other fixed page. The
header tells us where it is. This matters because a root split creates a brand
new root, and a root collapse replaces it with one of its children.

The rollback journal is a separate file. It is not another region inside the
main database file. I will come back to why it exists when we follow an
insertion all the way through commit.

## 11. The Layout of a B+ Tree Page

Both leaf and internal pages use a slotted layout. The page header and cell
directory grow from the start of the page, while the actual cells are packed
from the end:

```text
low byte offsets                                      high byte offsets

+-------------+----------------+------------------+-------------------+
| page header | cell directory |    free space    | packed cell data  |
+-------------+----------------+------------------+-------------------+
              grows this way ->    <- grows this way
```

The free space sits in the middle. The page header stores the boundaries of
that free region.

### Leaf Pages

A leaf-page header is 7 bytes:

```text
+-----------+-----------+-------------------+-----------------+
| page type | key count | free-space start  | free-space end  |
| 1 byte    | 2 bytes   | 2 bytes           | 2 bytes         |
+-----------+-----------+-------------------+-----------------+
```

Each directory entry is a 2-byte offset pointing to a leaf cell. A leaf cell
contains:

```text
key type | key size | encoded key | value type | value size | value
```

### Internal Pages

The internal-page header is 11 bytes. It has the same first four fields, plus
the 4-byte page number of the leftmost child:

```text
page type | key count | free-space start | free-space end | leftmost child
```

Every internal cell then stores:

```text
key type | key size | separator key | right child page number
```

The leftmost child has no separator before it, which is why it lives in the
header. Every other child is stored beside the separator immediately to its
left.

## 12. Storing Variable-Length Records

Keys and values are not all the same size. An integer key may take a handful
of bytes, while a string or byte-array value may be much larger. If we stored
cells in fixed-size slots, we would either waste a lot of space or impose a
very small maximum record size.

The cell directory avoids that. The entries in the directory stay in sorted
key order, but the cells they point to can live wherever they fit in the cell
region:

```text
Directory                    Cell region

[ offset to key 5  ] ------> [ key 5  | value ... ]
[ offset to key 12 ] ---+    [ key 31 | value ........ ]
[ offset to key 31 ] -+ |    [ key 12 | value .... ]
                      | +------------------^
                      +--------------------^
```

This separates logical order from physical placement. Moving a cell inside
the page only requires changing its directory offset. References from other
pages are unaffected because they point to the page, not to a byte offset
inside it.

My current implementation keeps the decoded keys and values in vectors. When
`write_back` is called, it clears the raw page and packs all of the cells again
from the end. It is simple and was easier to get correct than doing in-place
cell movement.

There is an important rough edge here. The tree currently decides that a page
is full based on the number of keys, using the fixed order of `200`. With
variable-length records, entry count is not enough. A page with a few large
values can run out of bytes long before it reaches 199 entries. For this case, we will need overflow
pages. It will add a lot of complexity to the tree operations, so I decided not to implement it in
the first version.

Check out the notes section if you are curious on how to handle large records inside 4KB pages

## 13. Page Allocation and Reuse

Splits need new pages, and merges leave old pages behind. Continually growing
the file without ever reusing anything would be easy, but not great.

The pager handles this with a freelist stored inside the database file. The
header points to the first free page, and every free page stores links to the
next and previous free pages:

```text
database header
      |
      v
 [ free page 18 ] <-> [ free page 7 ] <-> [ free page 42 ]
```

When the tree frees a page after a merge, the pager puts it at the front of
this list and updates the freelist count in the header.

When a split asks for a page, the pager checks the freelist first. If one is
available, it removes the head, clears its old bytes, and returns that page
number. If the freelist is empty, it uses the current database page count as
the new page number and grows the file by one page.

Freeing a page does not immediately make the database file smaller. It makes
that space reusable. Shrinking the physical file would require a separate
compaction or vacuum-like operation.

## 14. Following an Insertion All the Way to Disk

Let's put the pieces together. Assume we insert a key into page `12`, that leaf
is full, and its parent is page `4`. We will also assume the freelist is empty,
so the pager has to append a new page.

The operation roughly looks like this:

```text
1. Read the root and descend to leaf page 12.
2. Tell the pager that page 12 is about to change.
3. Insert the key into page 12 in memory and notice that it overflowed.
4. Allocate page 27 for the new right leaf, updating the database header.
5. Split the entries between pages 12 and 27.
6. Tell the pager that parent page 4 is about to change.
7. Insert the new separator and page number 27 into page 4.
```

Calling `begin_write` before changing an existing page lets the pager keep its
old image for rollback. New pages and the database header are also added to the
dirty set as they are changed.

At this point, the tree is updated in memory, but that does not mean the
operation is safely committed. A crash halfway through writing pages `12`,
`27`, and `4` could leave the file with half of the old tree and half of the
new one.

My pager uses a rollback journal to avoid that. In this example, the existing
versions of pages `0`, `4`, and `12` need to be recoverable:

```text
1. Save the old images of existing pages that will be overwritten.
2. Write and sync those images to the journal.
3. Write the new dirty pages to the database file.
4. Sync the database file.
5. Truncate the journal to finish the commit.
```

A newly appended page such as page `27` has no old image to save. Instead, the
journal remembers the database's original page count. Recovery can restore the
old pages and truncate any pages added by the interrupted transaction.

This is where the boundaries between subsystems become useful. The B+ tree
decides *which* pages need to change. The pager and journal decide *how* those
changes safely reach disk.

## 15. What the B+ Tree Does Not Solve

The tree gives us an ordered index and a way to map keys to pages. That is a
big part of a storage engine, but it is not the whole thing.

It does not answer:

- Which pages should stay in memory and which one should be evicted?
- How do we recover if the process crashes halfway through a write?
- How do multiple readers or writers coordinate?
- How do transactions get committed or rolled back atomically?
- How do we detect corruption in a database page?
- How do SQL rows and schemas map onto the key-value records?

In my project, the pager, page cache, rollback journal, and lock manager handle
some of these problems already. The current implementation, however, renders the system
an embedded engine. There are still a lot of space for improvement, and I'm already planning on that.
Currently, I'm working on a big migration to transform the engine into a client-server architecture with raft-backed replication. It will probably take a lot of time and effort to get it done, but as always, it will be exciting :D

## 16. Next
In the next article, we will talk about crash-tolerance and recovery. How do we protect our database from sudden machine crashes? How do we recover the database to a consistent state? We will also talk about concurrency control for transactions. The isolation level that the engine currently provides is serlizable transactions, which means that clients will never experience any of the following:
- Dirty Reads
- Unrepeatable reads
- Phantom reads

See ya next time!
