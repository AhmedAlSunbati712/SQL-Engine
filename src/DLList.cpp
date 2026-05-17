#include <unordered_map>
#include <DLList.h>


DLList::DLList(): head(new Node()), tail(new Node()) {
    head->next = tail;
    tail->prev = head;
}

/**
 * @brief Destructor function. Frees up the nodes and the pages stored in the nodes
 */
DLList::~DLList() {
    Node *curr = head;
    while (curr) {
        Node *next = curr->next;
        delete curr;
        curr = next;
    }
}

/**
 * @brief Adds a new page to the list. If the page already exists in the list, we move it to the end of the list
 */
void DLList::add(int page_num, Page *page) {
    Node *new_node = nullptr;
    if (nodesMap.find(page_num) != nodesMap.end()) {
        new_node = nodesMap[page_num];

        // detach it from its position in the list
        Node *new_node_prev = new_node->prev;
        new_node_prev->next = new_node->next;
        new_node->next->prev = new_node_prev;
    } else {
        new_node = new Node(page);
        nodesMap[page_num] = new_node;
        length++;
    }

    // Attaching to the end of the list
    Node *last_node = tail->prev;
    last_node->next = new_node;
    new_node->prev = last_node;

    new_node->next = tail;
    tail->prev = new_node;
}

/**
 * @brief Given a page_num for a page, we lookup that page and return it or return a nullptr if not found.
 */
Page *DLList::get(int page_num) {
    if (page_num < 0) return nullptr;
    return (nodesMap.find(page_num) == nodesMap.end()) ? nullptr : nodesMap[page_num]->page;
}

Page *DLList::remove(int page_num) {
    auto it = nodesMap.find(page_num);
    if (it == nodesMap.end()) return nullptr;
    Node *node = it->second;
    Page *page = node->page; // Save the page to return before doing cleanup

    // Cleanup poitners in the DLList
    Node *prev_node = node->prev;
    prev_node->next = node->next;
    node->next->prev = prev_node;

    // Decrement length, free node, and remove from the nodesmap.
    length--;
    nodesMap.erase(page_num);
    
    return page;
}

bool DLList::exists(int page_num) {
    return (nodesMap.find(page_num) == nodesMap.end()) ? false : true;
}

int DLList::len() {
    return length;
}