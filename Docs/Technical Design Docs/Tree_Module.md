I think walking through the different cases for node insertions and deletions will help define the interface we need

## Assumptions

If the keys in a node are: k_1, k_2, k_3, and if it has children c_0, c_1, c_2, c_3, then the relation between the keys are as the following:

- Keys in c_0 < k_1
- k_1 ≤ Keys in c_1 < k_2
- …etc

So when traversing an internal page, we descend into the child corresponding to the first separator key that is strictly greater than the search key. If no separator key is greater, we descend into the rightmost child.

For the rest of this document, assume that a page may hold at most m - 1 keys. So a page overflow means that after insertion it temporarily holds m keys.

Occupancy rules:

- A root leaf page may have anywhere between 0 and m - 1 keys. The 0-key case represents an empty tree.
- A root internal page may have anywhere between 1 and m - 1 keys.
- A root internal page may therefore have anywhere between 2 and m child page numbers.
- Any non-root leaf page must have at least ceil(m / 2) - 1 keys and at most m - 1 keys.
- Any non-root internal page must have at least ceil(m / 2) - 1 keys and at most m - 1 keys.
- Since an internal page always has exactly one more child page number than separator keys, any non-root internal page must have between ceil(m / 2) and m child page numbers.

The insertion should keep track of the pages traversed to get to the target leaf

## Insertion

**Key exists in the map**

This case is trivial:

- If the head of the B-tree is not a leaf, find the first separator key that is strictly greater than the target key and descend into the corresponding child. If none is greater, descend into the rightmost child. if that child is not a leaf, do the same process again.
- If it’s a leaf, find the index of the key and update the corresponding index in the values array

**Key doesn’t exist in the map**

- Follow the traversal process above until we get to the target leaf.
- Find the index of the first element that is greater than or equal to the key. If it is not equal, insert the key and its value at that position.

What we need to handle now is overflow: if insertion leaves the page with m keys. Two cases:

**The parent has enough space**

Let’s assume the parent has the following structure

- Keys: k1, k2, k3
- Child page numbers: c0, c1, c2, c3
- Let’s assume that the leaf page that is going to be split is page number c1.
- Let k_p be the smallest key in the new right leaf after the split.

Steps:

- Allocate a new page c_p. it’s going to be a leaf page.
- Split c1 into two leaf pages:
	- The old leaf c1 keeps the key-value pairs whose keys are less than k_p.
	- The new right leaf c_p gets the key-value pairs whose keys are greater than or equal to k_p.
- Promote k_p to the parent. Since this is a leaf split, k_p still remains in the new right leaf page c_p. It is copied up to the parent, not removed from the leaf.
- The new parent structure will be:
	- Keys: k1, k_p, k2, k3
	- Child page numbers: c_0, c_1, c_p, c_2, c_3
- And we are done

**The Parent doesn’t have enough space**

We need to rearrange both the keys and the child pointers

- After splitting the leaf, inserting k_p into the parent would temporarily give the parent this structure:
	- Keys: k1, k_p, k2, k3, k4
	- Child page numbers: c_0, c_1, c_p, c_2, c_3, c_4
- The parent is now full and needs to be split.
- Promote the median key k2 to the parent of this internal node.
- Allocate a new internal page for the right half. it will have:
	- Keys: k3, k4
	- Child page numbers: c_2, c_3, c_4
- The old internal page keeps the left half:
	- Keys: k1, k_p
	- Child page numbers: c_0, c_1, c_p
- Since this is an internal-page split, the promoted key k2 does not stay in either child page after the split. it only lives in the parent above them.
- So from the point of view of the next parent up, the old internal child is replaced by:
	- left child: old internal page with keys k1, k_p
	- separator key: k2
	- right child: new internal page with keys k3, k4

**The root ends up with m keys**

- If split propagation reaches the root and the root itself now has m keys, then the root has overflowed and must also be split.
- Create a new root node.
- Promote the median key from the old root into this new root.
- Split the old root into two children:
	- a left child containing the keys and child pointers to the left of the promoted median
	- a right child containing the keys and child pointers to the right of the promoted median
- The promoted median does not stay in either child if the old root was an internal page. If the old root was a leaf page, then the same leaf-split rule applies: the promoted key is copied into the new root and still remains in the new right leaf.
- The new root will now contain one key and two child page numbers, and the height of the tree increases by one.
## Deletion

**Leaf page that contains the key has more than the minimal number of keys**

- In this case, just delete the key and its corresponding value. Nothing else needs to happen to the tree structure.
- One thing to handle: if the deleted key was the first key in the leaf page, then the separator key in the parent that points to this leaf may need to change. Update that parent separator so that it matches the new first key of this leaf page.
- If that separator change also changes the first key represented by the parent subtree, then the same separator update may need to continue upward through ancestors until the copied subtree minimum is consistent again.

**Leaf page contains the minimal number of keys**

- Delete the key and its corresponding value.
- Check the sibling nodes:
	    - If one of them has more than the minimal number of keys, then borrow a key from the sibling node
	        - If it’s the left sibling node, borrow its last key-value pair and put it at the start of the current leaf page. Update the separator key in the parent (the one whose right child is the current page) so that it matches the new first key of the current leaf page.
	        - If it’s the right sibling node, borrow its first key-value pair and put it at the end of the current leaf page. Then update the separator key in the parent (the one whose right child is the sibling we borrowed from) so that it matches the new first key of that right sibling.
	    - If none of them have more than the minimal number of keys, merge with whichever one is available.
	        - If the left sibling is chosen, move all of the current leaf page's key-value pairs into the left sibling page, then delete the current page. In the parent, delete the separator key whose right child pointer pointed to the current page.
	        - If the right sibling is chosen, move all of the right sibling's key-value pairs into the current leaf page, then delete the right sibling page. In the parent, delete the separator key whose right child pointer pointed to that right sibling page.
- If borrowing or merging changes the smallest key represented by a subtree, propagate that separator update upward the same way as above.
- After a merge, the parent may now go under the minimal number of keys. In that case, the same underflow repair process must continue upward.

**An internal page goes under the minimal number of keys**

This is similar in spirit to the leaf-page case, but the key movement rules are different because internal-page keys are routing separators rather than actual stored records.

- After deleting a separator key / child pointer pair from an internal page, check whether the page still has at least the minimal number of keys.
- If it does, then we are done.
- If it does not, check the sibling internal pages.
    - If one of them has more than the minimal number of keys, then borrow through the parent.
        - If borrowing from the left sibling:
            - Let k_p be the separator key in the parent between the left sibling and the underflowing internal page.
            - Move k_p down into the start of the current internal page.
            - Move the rightmost child pointer from the left sibling over so that it becomes the leftmost child of the current internal page.
            - Move the left sibling's last key up into the parent so that it becomes the new separator k_p.
        - If borrowing from the right sibling:
            - Let k_p be the separator key in the parent between the current internal page and the right sibling.
            - Move k_p down into the end of the current internal page.
            - Move the leftmost child pointer from the right sibling over so that it becomes the new rightmost child of the current internal page.
            - Move the right sibling's first key up into the parent so that it becomes the new separator k_p.
    - If neither sibling has more than the minimal number of keys, then merge with one of them.
        - If merging with the left sibling:
            - Let k_p be the separator key in the parent between the left sibling and the current internal page.
            - Move k_p down into the left sibling.
            - Append all keys and child page numbers from the current internal page to that left sibling.
            - Delete the current internal page.
            - Delete k_p from the parent, since it has now moved down into the merged page.
        - If merging with the right sibling:
            - Let k_p be the separator key in the parent between the current internal page and the right sibling.
            - Move k_p down into the current internal page.
            - Append all keys and child page numbers from the right sibling to the current internal page.
            - Delete the right sibling page.
            - Delete k_p from the parent, since it has now moved down into the merged page.
- After merging internal pages, the parent may itself go under the minimal number of keys. If that happens, continue the same repair process upward.
- One last root case: if the root ends up with zero keys and has exactly one child, make that child the new root. This reduces the height of the tree by one.

## Footnotes
- We need to keep the page-num of the root page of the b+ tree in the header of the db and make sure to modify it accordingly whenever we change the node.
