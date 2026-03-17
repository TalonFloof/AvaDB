#include <avadb/avatree.h>
#include <string.h>
#include <stdlib.h>

/* Helper Functions for Slotted Pages */
static AvaTreeCellPtr* get_cell_ptrs(AvaTreePageHeader* page) {
    return (AvaTreeCellPtr*)((uint8_t*)page + sizeof(AvaTreePageHeader));
}
static void* get_cell_content(AvaTreePageHeader* page, uint16_t index) {
    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    return (uint8_t*)page + ptrs[index].offset;
}
static uint32_t get_node_free_space(AvaTreePageHeader* page) {
    uint32_t free_start = sizeof(AvaTreePageHeader) + 
                          (page->type_header.node.num_entries * sizeof(AvaTreeCellPtr));
    uint32_t free_end = page->type_header.node.free_end;
    if (free_end < free_start) return 0; /* Shouldn't happen in normal circumstances, but this check is done for safety */
    return free_end - free_start;
}

/* Internal Functions */
static int node_find_key(AvaTreePageHeader* page, char* key, uint8_t key_size, uint16_t* out_index) {
    uint16_t low = 0;
    uint16_t high = page->type_header.node.num_entries;
    int exact_match = 0;
    while (low < high) {
        uint16_t mid = (low + high) / 2;
        /* get pointer to the key based on Node Type */
        uint8_t* cell_key = NULL;
        uint8_t cell_key_size = 0;
        void* cell_content = get_cell_content(page, mid);

        if (page->type == AVA_PAGE_TYPE_INTERNAL) {
            AvaTreeInternalCell* cell = (AvaTreeInternalCell*)cell_content;
            cell_key = cell->key;
            cell_key_size = cell->key_size;
        } else {
            AvaTreeLeafCell* cell = (AvaTreeLeafCell*)cell_content;
            cell_key = cell->payload;
            cell_key_size = cell->key_size;
        }
        /* Compare keys */
        int cmp = 0;
        int min_len = (key_size < cell_key_size) ? key_size : cell_key_size;
        cmp = memcmp(key, cell_key, min_len);
        /* If our prefix matches, the actual key is thus greater */
        if (cmp == 0) {
            if (key_size < cell_key_size) cmp = -1;
            else if (key_size > cell_key_size) cmp = 1;
        }
        /* Otherwise, perform a standard search tree check */
        if (cmp < 0) {
            high = mid;
        } else if (cmp > 0) {
            low = mid + 1;
        } else {
            *out_index = mid;
            return 1;
        }
    }
    *out_index = low;
    return 0;
}

/* High-level Tree Functions */
AvaTreeLeafCell* ava_tree_search(AvaPager* pager, ava_pgid_t root_pgid, char* key, uint8_t key_size) {
    if (root_pgid == 0) return NULL; /* Sanity check */
    ava_pgid_t current_pgid = root_pgid;
    AvaTreePageHeader* page = ava_pager_read(pager, current_pgid);
    while (page->type == AVA_PAGE_TYPE_INTERNAL) {
        uint16_t index;
        node_find_key(page, key, key_size, &index);
        /* Perform a modified binary tree search, each node holds a pointer that
         * points to a node that contains keys less than the specified key.
         * If none match our condition, we use the right sibling pointer on the node.
         */
        ava_pgid_t next_pgid;
        if (index >= page->type_header.node.num_entries) {
            next_pgid = page->type_header.node.right_sibling;
        } else {
            AvaTreeInternalCell* cell = (AvaTreeInternalCell*)get_cell_content(page, index);
            next_pgid = cell->left_child;
        }
        /* Retrieve the next page */
        current_pgid = next_pgid;
        page = (AvaTreePageHeader*)ava_pager_get_page(pager, current_pgid);
    }
    /* We are at a leaf node, do a linear search for the key until we hit the end of the linked list */
    uint16_t index;
    int found = node_find_key(page, key, key_size, &index);
    if (found)
        return get_cell_content(page, index);
    return NULL;
}
