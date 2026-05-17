#pragma once
#include <unordered_map>
#include <vector>

// Forward-declaration of the page struct
struct Page;

/**
 * Purpose: The usecase I'm targeting for this is in the PCache. It will hold a list of pages that
 *          are still sitting in cache but are unref'ed (ready to be swapped out). Pages will be added to
 *          the end of the list. We also want to support the following operations:
 *              i. O(1) lookup
 *              ii. O(1) adding
 *              iii. O(1) removing
 *              iv. Length
 */
class DLList {
    private:
        struct Node {
            Node() {};
            Node(Page *page): page(page) {};
            Page *page = nullptr;
            Node *next = nullptr;
            Node *prev = nullptr;
        };
        std::unordered_map<int, Node *> nodesMap; // For O(1) lookups
        int length = 0;
        Node *head = nullptr;
        Node *tail = nullptr;
    public:
        DLList();
        ~DLList();
        void add(int page_num, Page *page);
        Page *get(int page_num);
        Page *remove(int page_num);
        bool exists(int page_num);
        int len();
};