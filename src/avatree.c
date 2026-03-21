#include <avadb/avatree.h>
#include <string.h>
#include <stdlib.h>

/* Overflow page management */

static ava_pgid_t create_overflow_chain(AvaPager* pager, char* data, uint32_t size) {
    ava_pgid_t first_page = 0;
    ava_pgid_t current_page = 0;
    uint32_t written = 0;

    while (written < size) {
        ava_pgid_t new_page;
        ava_pager_allocate(pager, &new_page);
        AvaTreePageHeader* page = ava_pager_get(pager, new_page);
        page->type = AVA_PAGE_TYPE_OVERFLOW;

        if (first_page == 0)
            first_page = new_page;
        else {
            AvaTreePageHeader* prev = ava_pager_get(pager, current_page);
            prev->header.overflow.next = new_page;
            ava_pager_mark_dirty(pager, current_page);
            ava_pager_unpin(pager, current_page);
        }

        current_page = new_page;
        page->header.overflow.next = 0;

        uint32_t to_write = (size-written > pager->page_size - sizeof(AvaTreePageHeader)) ? (pager->page_size - sizeof(AvaTreePageHeader)) : (size-written);

        memcpy((uint8_t*)page + sizeof(AvaTreePageHeader), data + written, to_write);
        ava_pager_mark_dirty(pager, current_page);
        written += to_write;
        ava_pager_unpin(pager, new_page);
    }
    return first_page;
}

static void free_overflow_chain(AvaPager* pager, ava_pgid_t chain_head_page) {
    for (ava_pgid_t cur = chain_head_page; cur != 0;) {
        AvaTreePageHeader* page = ava_pager_get(pager, cur);
        ava_pgid_t next = page->header.overflow.next;
        ava_pager_free(pager, cur);
        ava_pager_unpin(pager, cur);
        cur = next;
    }
}

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
                          (page->header.node.num_entries * sizeof(AvaTreeCellPtr));
    uint32_t free_end = page->header.node.free_end;
    if (free_end < free_start) return 0; /* Shouldn't happen in normal circumstances, but this check is done for safety */
    return free_end - free_start;
}

static void compact_page(AvaPager* pager, AvaTreePageHeader* page) {
    uint16_t num_entries = page->header.node.num_entries;
    uint32_t header_size = sizeof(AvaTreePageHeader) + (num_entries * sizeof(AvaTreeCellPtr));

    uint8_t* temp_buffer = malloc(pager->page_size);
    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    memcpy(temp_buffer, page, pager->page_size);
    AvaTreeCellPtr* temp_ptrs = (AvaTreeCellPtr*)(temp_buffer + sizeof(AvaTreePageHeader));
    
    /* Reset the real page's free pointer */
    page->header.node.free_end = pager->page_size;
    
    for (uint16_t i = 0; i < num_entries; i++) {
        uint32_t size = temp_ptrs[i].size;
        void* data = temp_buffer + temp_ptrs[i].offset;
        
        page->header.node.free_end -= size;
        memcpy((uint8_t*)page + page->header.node.free_end, data, size);
        
        ptrs[i].offset = page->header.node.free_end;
    }
    free(temp_buffer);
}

/* Internal Functions */

static int node_find_key(AvaTreePageHeader* page, char* key, uint8_t key_size, uint16_t* out_index) {
    uint16_t low = 0;
    uint16_t high = page->header.node.num_entries;
    while (low < high) {
        uint16_t mid = (low + high) / 2;
        /* get pointer to the key based on Node Type */
        uint8_t* cell_key = NULL;
        uint8_t cell_key_size = 0;
        void* cell_content = get_cell_content(page, mid);
        if (page->type == AVA_PAGE_TYPE_INTERNAL) {
            AvaTreeInternalCell* cell = cell_content;
            cell_key = cell->key;
            cell_key_size = cell->key_size;
        } else {
            AvaTreeLeafCell* cell = cell_content;
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

static void leaf_node_insert(AvaPager* pager, AvaTreePageHeader* page, uint16_t index, char* key, uint8_t key_size, char* value, uint32_t value_size, uint8_t value_type) {
    uint16_t cell_content_size = sizeof(AvaTreeLeafCell) + key_size + value_size;
    
    if (get_node_free_space(page) < cell_content_size + sizeof(AvaTreeCellPtr)) {
        compact_page(pager, page);
    }
    page->header.node.free_end -= cell_content_size;
    ava_off_t new_cell_offset = page->header.node.free_end;

    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    AvaTreeLeafCell* new_cell = (AvaTreeLeafCell*)((uint8_t*)page + new_cell_offset);

    /* Shift existing pointers to the right to ensure entries are aligned */
    memmove(&ptrs[index + 1],
            &ptrs[index],
            (page->header.node.num_entries - index) * sizeof(AvaTreeCellPtr));

    ptrs[index].offset = new_cell_offset;
    ptrs[index].size = cell_content_size;
    new_cell->key_size = key_size;
    new_cell->value_size = value_size;
    new_cell->value_type = value_type;
    memcpy(new_cell->payload, key, key_size);
    memcpy(new_cell->payload + key_size, value, value_size);
    page->header.node.num_entries++;
}

static void internal_node_insert(AvaPager* pager, AvaTreePageHeader* page, uint16_t index, ava_pgid_t left_child, char* key, uint8_t key_size) {
    /* Internal nodes store {LeftChild, Key}. The 'right_sibling' or next cell's left_child acts as the right path. */
    uint16_t cell_content_size = sizeof(AvaTreeInternalCell) + key_size;

    if (get_node_free_space(page) < cell_content_size + sizeof(AvaTreeCellPtr)) {
        compact_page(pager, page);
    }
    /* Sanity Check: If after compaction we still don't have space, we can't insert. */
    /* In a robust implementation this should return an error or force a split. */
    if (get_node_free_space(page) < cell_content_size + sizeof(AvaTreeCellPtr)) {
        return; /* Prevent corruption */
    }
    page->header.node.free_end -= cell_content_size;
    ava_off_t new_cell_offset = page->header.node.free_end;

    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    AvaTreeInternalCell* new_cell = (AvaTreeInternalCell*)((uint8_t*)page + new_cell_offset);

    memmove(&ptrs[index + 1],
            &ptrs[index],
            (page->header.node.num_entries - index) * sizeof(AvaTreeCellPtr));

    ptrs[index].offset = new_cell_offset;
    ptrs[index].size = cell_content_size;

    new_cell->left_child = left_child;
    new_cell->key_size = key_size;
    memcpy(new_cell->key, key, key_size);

    page->header.node.num_entries++;
}

static void internal_node_update_key(AvaPager* pager, AvaTreePageHeader* page, uint16_t index, char* new_key, uint8_t new_key_size) {
    uint16_t cell_content_size = sizeof(AvaTreeInternalCell) + new_key_size;

    /* Optimization: If the page is tight, we must reclaim the OLD key's space to fit the NEW key. */
    if (get_node_free_space(page) < cell_content_size) {
        /* 1. Save the critical data from the old cell (left_child) */
        AvaTreeInternalCell* old_cell = get_cell_content(page, index);
        ava_pgid_t saved_left_child = old_cell->left_child;

        /* 2. Mark the old cell as empty so compact_page() discards its data */
        AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
        ptrs[index].size = 0;

        /* 3. Compact. This deletes the old key data, making room. */
        compact_page(pager, page);
        
        if (get_node_free_space(page) < cell_content_size) {
             return; /* Critical failure: Page full even after compaction */
        }

        /* 4. Allocate and write the new cell */
        page->header.node.free_end -= cell_content_size;
        AvaTreeInternalCell* new_cell = (AvaTreeInternalCell*)((uint8_t*)page + page->header.node.free_end);
        
        new_cell->left_child = saved_left_child;
        new_cell->key_size = new_key_size;
        memcpy(new_cell->key, new_key, new_key_size);

        /* 5. Update the directory pointer */
        ptrs[index].offset = page->header.node.free_end;
        ptrs[index].size = cell_content_size;
        return;
    }

    /* Fast path: We have enough space to just append the new key without compacting immediately */
    AvaTreeInternalCell* old_cell = get_cell_content(page, index);
    ava_pgid_t left_child = old_cell->left_child;

    page->header.node.free_end -= cell_content_size;
    ava_off_t new_cell_offset = page->header.node.free_end;

    AvaTreeInternalCell* new_cell = (AvaTreeInternalCell*)((uint8_t*)page + new_cell_offset);
    new_cell->left_child = left_child;
    new_cell->key_size = new_key_size;
    memcpy(new_cell->key, new_key, new_key_size);

    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    ptrs[index].offset = new_cell_offset;
    ptrs[index].size = cell_content_size;
}

/* Used to pass split data up the recursion stack */
typedef struct {
    int did_split;
    ava_pgid_t new_page_id;
    uint8_t promoted_key_size;
    uint8_t promoted_key[255]; /* Max key size is small enough to keep on stack */
} AvaSplitResult;

static void leaf_node_split(AvaPager* pager, AvaTreePageHeader* old_page, char* new_key, uint8_t new_key_size, char* new_value, uint32_t new_value_size, uint8_t new_value_type, AvaSplitResult* result) {
    /* Allocate new page for split */
    result->did_split = 1;
    ava_pager_allocate(pager,&result->new_page_id);
    AvaTreePageHeader* new_page = ava_pager_get(pager, result->new_page_id);
    ava_pager_mark_dirty(pager, result->new_page_id);

    new_page->type = AVA_PAGE_TYPE_LEAF;
    new_page->header.node.num_entries = 0;
    new_page->header.node.free_end = pager->page_size;
    new_page->header.node.right_sibling = old_page->header.node.right_sibling;
    old_page->header.node.right_sibling = result->new_page_id;

    /* Get how we'll split the entries */
    uint16_t total_entries = old_page->header.node.num_entries;
    uint16_t split_index = total_entries / 2;

    /* Move the first half to a new page */
    for (uint16_t i = split_index; i < total_entries; i++) {
        AvaTreeLeafCell* cell = get_cell_content(old_page, i);
        leaf_node_insert(pager, new_page, new_page->header.node.num_entries,
                         (char*)cell->payload, cell->key_size,
                         (char*)(cell->payload + cell->key_size), cell->value_size, cell->value_type);
    }
    /* Truncate from the old page */
    old_page->header.node.num_entries = split_index;
    /* Compare new_key with the first key of the new page (the separator) */
    AvaTreeLeafCell* first_new = get_cell_content(new_page, 0);
    /* Comparison logic */
    int cmp = 0;
    int min_len = (new_key_size < first_new->key_size) ? new_key_size : first_new->key_size;
    cmp = memcmp(new_key, first_new->payload, min_len);
    if (cmp == 0) {
         if (new_key_size < first_new->key_size) cmp = -1;
         else if (new_key_size > first_new->key_size) cmp = 1;
    }

    if (cmp < 0) {
        /* Insert into Old Page */
        uint16_t idx;
        node_find_key(old_page, new_key, new_key_size, &idx);
        leaf_node_insert(pager, old_page, idx, new_key, new_key_size, new_value, new_value_size, new_value_type);
    } else {
        /* Insert into New Page */
        uint16_t idx;
        node_find_key(new_page, new_key, new_key_size, &idx);
        leaf_node_insert(pager, new_page, idx, new_key, new_key_size, new_value, new_value_size, new_value_type);
    }

    /* Set out result (copy of the first key of the right sibling) */
    first_new = get_cell_content(new_page, 0);
    result->promoted_key_size = first_new->key_size;
    ava_pager_unpin(pager, result->new_page_id);
    memcpy(result->promoted_key, first_new->payload, first_new->key_size);
}

static void internal_node_split(AvaPager* pager, AvaTreePageHeader* old_page, ava_pgid_t extra_child, char* extra_key, uint8_t extra_key_size, AvaSplitResult* result) {
    result->did_split = 1;

    /*
     * This is the reconstruction approach. We create a temporary logical list of all
     * keys and pointers (N+1 keys, N+2 pointers) to make redistribution trivial.
     */
    uint16_t total_items = old_page->header.node.num_entries + 1;
    
    /* A temporary structure to hold the logical key/pointer pairs */
    struct TmpInternalEntry {
        ava_pgid_t child_id;
        uint8_t key_size;
        char key[255];
    };

    /* Use heap allocation to prevent stack overflow on large pages */
    struct TmpInternalEntry* all_entries = malloc(sizeof(struct TmpInternalEntry) * total_items);
    ava_pgid_t* all_children = malloc(sizeof(ava_pgid_t) * (total_items + 1));
    /* Find insertion point for the new key/child from the child split */
    uint16_t insert_idx;
    node_find_key(old_page, extra_key, extra_key_size, &insert_idx);
    /* Populate the temporary logical lists */
    int old_idx = 0;
    for (int i = 0; i < total_items; i++) {
        if (i == insert_idx) {
            /* Insert the new key from the child split */
            all_entries[i].key_size = extra_key_size;
            memcpy(all_entries[i].key, extra_key, extra_key_size);
        } else {
            /* Copy a key from the old page */
            AvaTreeInternalCell* cell = get_cell_content(old_page, old_idx);
            all_entries[i].key_size = cell->key_size;
            memcpy(all_entries[i].key, cell->key, cell->key_size);
            old_idx++;
        }
    }

    old_idx = 0;
    for (int i = 0; i < total_items + 1; i++) {
        if (i == insert_idx + 1) {
            /* The pointer after the new key is the new child page */
            all_children[i] = extra_child;
        } else {
            /* Copy a pointer from the old page */
            if (old_idx < old_page->header.node.num_entries) {
                all_children[i] = ((AvaTreeInternalCell*)get_cell_content(old_page, old_idx))->left_child;
            } else {
                all_children[i] = old_page->header.node.right_sibling;
            }
            old_idx++;
        }
    }

    /* Get the middle key that will be promoted to an internal node */
    uint16_t median_idx = total_items / 2;
    result->promoted_key_size = all_entries[median_idx].key_size;
    memcpy(result->promoted_key, all_entries[median_idx].key, all_entries[median_idx].key_size);

    /* Get a new page and initialize it */
    ava_pager_allocate(pager,&result->new_page_id);
    AvaTreePageHeader* new_page = ava_pager_get(pager, result->new_page_id);
    ava_pager_mark_dirty(pager, result->new_page_id);

    /* Clear old page and initialize new page */
    old_page->header.node.num_entries = 0;
    old_page->header.node.free_end = pager->page_size;
    new_page->type = AVA_PAGE_TYPE_INTERNAL;
    new_page->header.node.num_entries = 0;
    new_page->header.node.free_end = pager->page_size;

    /* Rebuild left (old) page */
    for (int i = 0; i < median_idx; i++) {
        internal_node_insert(pager, old_page, i, all_children[i], all_entries[i].key, all_entries[i].key_size);
    }
    old_page->header.node.right_sibling = all_children[median_idx];

    /* Rebuild right (new) page */
    for (int i = median_idx + 1; i < total_items; i++) {
        internal_node_insert(pager, new_page, new_page->header.node.num_entries, all_children[i], all_entries[i].key, all_entries[i].key_size);
    }
    new_page->header.node.right_sibling = all_children[total_items];

    ava_pager_unpin(pager, result->new_page_id);
    free(all_entries);
    free(all_children);
}

static void leaf_node_delete(AvaTreePageHeader* page, uint16_t index);

static void ava_tree_insert_recursive(AvaPager* pager, ava_pgid_t page_id, char* key, uint8_t key_size, char* value, uint32_t value_size, uint8_t value_type, AvaSplitResult* result) {
    AvaTreePageHeader* page = ava_pager_get(pager, page_id);
    result->did_split = 0;
    if (page->type == AVA_PAGE_TYPE_LEAF) {
        uint16_t insert_index;
        int found = node_find_key(page, key, key_size, &insert_index);

        /* Handle Duplicates: If key exists, delete it first (Replace/Upsert) */
        /* We do this BEFORE checking space, because deleting the old value might make room for the new one! */
        if (found) {
            /* If this is an overflow entry, free the overflow chain before attempting to delete the entry */
            AvaTreeLeafCell* cell = get_cell_content(page, insert_index);
            if (cell->value_type & AVA_VALUE_FLAG_OVERFLOW) {
                ava_pgid_t overflow = *(ava_pgid_t*)(cell->payload + cell->key_size);
                free_overflow_chain(pager, overflow);
            }

            leaf_node_delete(page, insert_index);
            /* Compact to ensure the deleted space is actually available for the new entry */
            compact_page(pager, page);
        }

        uint16_t required_space = sizeof(AvaTreeLeafCell) + key_size + value_size + sizeof(AvaTreeCellPtr);
        if (get_node_free_space(page) >= required_space) {
            ava_pager_mark_dirty(pager, page_id);
            leaf_node_insert(pager, page, insert_index, key, key_size, value, value_size, value_type);
        } else {
            ava_pager_mark_dirty(pager, page_id);
            leaf_node_split(pager, page, key, key_size, value, value_size, value_type, result);
        }
        ava_pager_unpin(pager, page_id);
        return;
    }
    uint16_t index;
    int found = node_find_key(page, key, key_size, &index);
    if (found) index++;
    ava_pgid_t next_pgid;
    if (index >= page->header.node.num_entries) {
        next_pgid = page->header.node.right_sibling;
    } else {
        next_pgid = ((AvaTreeInternalCell*)get_cell_content(page, index))->left_child;
    }
    ava_tree_insert_recursive(pager, next_pgid, key, key_size, value, value_size, value_type, result);
    if (result->did_split) {
        /* Child split! We need to insert the promoted key into THIS node. */
        uint16_t required = sizeof(AvaTreeInternalCell) + result->promoted_key_size + sizeof(AvaTreeCellPtr);
        if (get_node_free_space(page) >= required) {
            /* This node has space, absorb the split */
            node_find_key(page, (char*)result->promoted_key, result->promoted_key_size, &index);
            ava_pager_mark_dirty(pager, page_id);

            /*
             * The child we descended into (`next_pgid`) is the left sibling of the split.
             * The `result->new_page_id` is the right sibling.
             * We insert a new cell {P_left, K_promoted}.
             * P_left is the Old Page (`next_pgid`).
             * The pointer to the right sibling (new_page_id) must then become the `left_child` of the *next* cell in the array.
             */
            internal_node_insert(pager, page, index, next_pgid, (char*)result->promoted_key, result->promoted_key_size);

            /* The cell that was originally at `index` is now at `index + 1`.
               Its `left_child` pointer needs to be updated to point to the new right sibling page. 
               If we inserted at the end, the right sibling pointer is the page's right_sibling. */
            if (index == page->header.node.num_entries - 1) {
                page->header.node.right_sibling = result->new_page_id;
            } else {
                AvaTreeInternalCell* shifted_cell = get_cell_content(page, index + 1);
                shifted_cell->left_child = result->new_page_id;
            }

            result->did_split = 0; /* Split handled, stop bubbling up */
        } else {
            /* This internal node is also full, must split. */
            ava_pager_mark_dirty(pager, page_id);
            internal_node_split(pager, page, result->new_page_id, (char*)result->promoted_key, result->promoted_key_size, result);
        }
    }
    ava_pager_unpin(pager, page_id);
}

#define AVA_DELETE_OK 0
#define AVA_DELETE_UNDERFLOW 1

/*
 * Checks if a node is underflowed (less than 50% full).
 * Returns 1 if underflowed, 0 otherwise.
 */
static int is_node_underflow(AvaPager* pager, AvaTreePageHeader* page) {
    /* Calculate the static overhead */
    uint32_t used_space = sizeof(AvaTreePageHeader) +
                          (page->header.node.num_entries * sizeof(AvaTreeCellPtr));

    /* Iterate through active pointers to get the actual size of live content */
    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    for (uint16_t i = 0; i < page->header.node.num_entries; i++) {
        used_space += ptrs[i].size;
    }

    /* Threshold is typically 50% of the page size */
    return used_space < (pager->page_size / 2);
}

static void leaf_node_delete(AvaTreePageHeader* page, uint16_t index) {
    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);

    /* We don't need to reclaim space immediately (lazy deletion).
       Just shift the pointers left. */
    int num_to_move = page->header.node.num_entries - index - 1;
    if (num_to_move > 0) {
        memmove(&ptrs[index], &ptrs[index + 1], num_to_move * sizeof(AvaTreeCellPtr));
    }
    page->header.node.num_entries--;
}

static ava_pgid_t get_child_pgid(AvaTreePageHeader* parent, uint16_t child_index) {
    if (child_index >= parent->header.node.num_entries) {
        return parent->header.node.right_sibling;
    }
    return ((AvaTreeInternalCell*)get_cell_content(parent, child_index))->left_child;
}

static void internal_node_delete_key(AvaTreePageHeader* page, uint16_t key_index) {
    /* This is a simplified delete that only removes the key/pointer from the cell array.
       It doesn't handle freeing the child page, which is done by the caller. */
    AvaTreeCellPtr* ptrs = get_cell_ptrs(page);
    int num_to_move = page->header.node.num_entries - key_index - 1;
    if (num_to_move > 0) {
        memmove(&ptrs[key_index], &ptrs[key_index + 1], num_to_move * sizeof(AvaTreeCellPtr));
    }
    page->header.node.num_entries--;
}

/* --- Leaf Rebalancing --- */

static void borrow_from_right_leaf(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t right_sibling_pgid, AvaTreePageHeader* right_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, right_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    AvaTreeLeafCell* cell_to_move = get_cell_content(right_sibling_page, 0);
    leaf_node_insert(pager, child_page, child_page->header.node.num_entries, (char*)cell_to_move->payload, cell_to_move->key_size, (char*)(cell_to_move->payload + cell_to_move->key_size), cell_to_move->value_size, cell_to_move->value_type);
    leaf_node_delete(right_sibling_page, 0);

    AvaTreeLeafCell* new_separator_source = get_cell_content(right_sibling_page, 0);
    internal_node_update_key(pager, parent_page, child_index, (char*)new_separator_source->payload, new_separator_source->key_size);
}

static void borrow_from_left_leaf(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t left_sibling_pgid, AvaTreePageHeader* left_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, left_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    uint16_t last_idx = left_sibling_page->header.node.num_entries - 1;
    AvaTreeLeafCell* cell_to_move = get_cell_content(left_sibling_page, last_idx);
    leaf_node_insert(pager, child_page, 0, (char*)cell_to_move->payload, cell_to_move->key_size, (char*)(cell_to_move->payload + cell_to_move->key_size), cell_to_move->value_size, cell_to_move->value_type);
    leaf_node_delete(left_sibling_page, last_idx);

    AvaTreeLeafCell* new_sep = get_cell_content(child_page, 0);
    internal_node_update_key(pager, parent_page, child_index - 1, (char*)new_sep->payload, new_sep->key_size);
}

static void merge_with_right_leaf(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t right_sibling_pgid, AvaTreePageHeader* right_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, right_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    for (uint16_t i = 0; i < right_sibling_page->header.node.num_entries; i++) {
        AvaTreeLeafCell* cell_to_move = get_cell_content(right_sibling_page, i);
        leaf_node_insert(pager, child_page, child_page->header.node.num_entries, (char*)cell_to_move->payload, cell_to_move->key_size, (char*)(cell_to_move->payload + cell_to_move->key_size), cell_to_move->value_size, cell_to_move->value_type);
    }

    child_page->header.node.right_sibling = right_sibling_page->header.node.right_sibling;
    ava_pager_free(pager, right_sibling_pgid);

    if (child_index + 1 < parent_page->header.node.num_entries) {
         AvaTreeInternalCell* next_cell = get_cell_content(parent_page, child_index + 1);
         next_cell->left_child = child_pgid;
    } else {
         parent_page->header.node.right_sibling = child_pgid;
    }
    internal_node_delete_key(parent_page, child_index);
}

static void merge_with_left_leaf(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t left_sibling_pgid, AvaTreePageHeader* left_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, left_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    for (uint16_t i = 0; i < child_page->header.node.num_entries; i++) {
        AvaTreeLeafCell* cell = get_cell_content(child_page, i);
        leaf_node_insert(pager, left_sibling_page, left_sibling_page->header.node.num_entries, (char*)cell->payload, cell->key_size, (char*)(cell->payload + cell->key_size), cell->value_size, cell->value_type);
    }

    left_sibling_page->header.node.right_sibling = child_page->header.node.right_sibling;
    ava_pager_free(pager, child_pgid);

    if (child_index < parent_page->header.node.num_entries) {
        AvaTreeInternalCell* cell_at_child = get_cell_content(parent_page, child_index);
        cell_at_child->left_child = left_sibling_pgid;
    } else {
        parent_page->header.node.right_sibling = left_sibling_pgid;
    }
    internal_node_delete_key(parent_page, child_index - 1);
}

/* --- Internal Node Rebalancing --- */

static void borrow_from_right_internal(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t right_sibling_pgid, AvaTreePageHeader* right_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, right_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    AvaTreeInternalCell* parent_sep = get_cell_content(parent_page, child_index);
    internal_node_insert(pager, child_page, child_page->header.node.num_entries, child_page->header.node.right_sibling, (char*)parent_sep->key, parent_sep->key_size);
    
    AvaTreeInternalCell* right_0 = get_cell_content(right_sibling_page, 0);
    internal_node_update_key(pager, parent_page, child_index, (char*)right_0->key, right_0->key_size);

    child_page->header.node.right_sibling = right_0->left_child;
    internal_node_delete_key(right_sibling_page, 0);
}

static void borrow_from_left_internal(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t left_sibling_pgid, AvaTreePageHeader* left_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, left_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    uint16_t last_idx = left_sibling_page->header.node.num_entries - 1;
    AvaTreeInternalCell* left_last = get_cell_content(left_sibling_page, last_idx);
    AvaTreeInternalCell* parent_sep = get_cell_content(parent_page, child_index - 1);

    ava_pgid_t old_left_right = left_sibling_page->header.node.right_sibling;
    internal_node_insert(pager, child_page, 0, old_left_right, (char*)parent_sep->key, parent_sep->key_size);

    internal_node_update_key(pager, parent_page, child_index - 1, (char*)left_last->key, left_last->key_size);
    left_sibling_page->header.node.right_sibling = left_last->left_child;
    internal_node_delete_key(left_sibling_page, last_idx);
}

static void merge_with_right_internal(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t right_sibling_pgid, AvaTreePageHeader* right_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, right_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    AvaTreeInternalCell* parent_sep = get_cell_content(parent_page, child_index);
    internal_node_insert(pager, child_page, child_page->header.node.num_entries, child_page->header.node.right_sibling, (char*)parent_sep->key, parent_sep->key_size);

    for(uint16_t i=0; i<right_sibling_page->header.node.num_entries; i++) {
         AvaTreeInternalCell* cell = get_cell_content(right_sibling_page, i);
         internal_node_insert(pager, child_page, child_page->header.node.num_entries, cell->left_child, (char*)cell->key, cell->key_size);
    }
    child_page->header.node.right_sibling = right_sibling_page->header.node.right_sibling;
    ava_pager_free(pager, right_sibling_pgid);

    if (child_index + 1 < parent_page->header.node.num_entries) {
         AvaTreeInternalCell* next_cell = get_cell_content(parent_page, child_index + 1);
         next_cell->left_child = child_pgid;
    } else {
         parent_page->header.node.right_sibling = child_pgid;
    }
    internal_node_delete_key(parent_page, child_index);
}

static void merge_with_left_internal(AvaPager* pager, ava_pgid_t parent_id, AvaTreePageHeader* parent_page, uint16_t child_index, ava_pgid_t child_pgid, AvaTreePageHeader* child_page, ava_pgid_t left_sibling_pgid, AvaTreePageHeader* left_sibling_page) {
    ava_pager_mark_dirty(pager, child_pgid);
    ava_pager_mark_dirty(pager, left_sibling_pgid);
    ava_pager_mark_dirty(pager, parent_id);

    AvaTreeInternalCell* parent_sep = get_cell_content(parent_page, child_index - 1);
    internal_node_insert(pager, left_sibling_page, left_sibling_page->header.node.num_entries, left_sibling_page->header.node.right_sibling, (char*)parent_sep->key, parent_sep->key_size);

    for(uint16_t i=0; i<child_page->header.node.num_entries; i++) {
         AvaTreeInternalCell* cell = get_cell_content(child_page, i);
         internal_node_insert(pager, left_sibling_page, left_sibling_page->header.node.num_entries, cell->left_child, (char*)cell->key, cell->key_size);
    }
    left_sibling_page->header.node.right_sibling = child_page->header.node.right_sibling;
    ava_pager_free(pager, child_pgid);
    
    if (child_index < parent_page->header.node.num_entries) {
        AvaTreeInternalCell* cell_at_child = get_cell_content(parent_page, child_index);
        cell_at_child->left_child = left_sibling_pgid;
    } else {
        parent_page->header.node.right_sibling = left_sibling_pgid;
    }
    internal_node_delete_key(parent_page, child_index - 1);
}

static int handle_node_underflow(AvaPager* pager, AvaTreePageHeader* parent_page, ava_pgid_t parent_id, uint16_t child_index) {
    ava_pgid_t child_pgid = get_child_pgid(parent_page, child_index);
    AvaTreePageHeader* child_page = ava_pager_get(pager, child_pgid);
    
    ava_pgid_t right_sibling_pgid = 0;
    AvaTreePageHeader* right_sibling_page = NULL;
    if (child_index < parent_page->header.node.num_entries) {
        right_sibling_pgid = get_child_pgid(parent_page, child_index + 1);
        right_sibling_page = ava_pager_get(pager, right_sibling_pgid);
    }

    ava_pgid_t left_sibling_pgid = 0;
    AvaTreePageHeader* left_sibling_page = NULL;
    if (child_index > 0) {
        left_sibling_pgid = get_child_pgid(parent_page, child_index - 1);
        left_sibling_page = ava_pager_get(pager, left_sibling_pgid);
    }

    /* Attempt to borrow from right sibling */
    if (right_sibling_page && !is_node_underflow(pager, right_sibling_page) && right_sibling_page->header.node.num_entries > 1) {
        if (child_page->type == AVA_PAGE_TYPE_LEAF) borrow_from_right_leaf(pager, parent_id, parent_page, child_index, child_pgid, child_page, right_sibling_pgid, right_sibling_page);
        else borrow_from_right_internal(pager, parent_id, parent_page, child_index, child_pgid, child_page, right_sibling_pgid, right_sibling_page);
        ava_pager_unpin(pager, child_pgid);
        if (right_sibling_page) ava_pager_unpin(pager, right_sibling_pgid);
        if (left_sibling_page) ava_pager_unpin(pager, left_sibling_pgid);
        return AVA_DELETE_OK;
    }

    /* Attempt to borrow from left sibling */
    if (left_sibling_page && !is_node_underflow(pager, left_sibling_page) && left_sibling_page->header.node.num_entries > 1) {
        if (child_page->type == AVA_PAGE_TYPE_LEAF) borrow_from_left_leaf(pager, parent_id, parent_page, child_index, child_pgid, child_page, left_sibling_pgid, left_sibling_page);
        else borrow_from_left_internal(pager, parent_id, parent_page, child_index, child_pgid, child_page, left_sibling_pgid, left_sibling_page);
        ava_pager_unpin(pager, child_pgid);
        if (right_sibling_page) ava_pager_unpin(pager, right_sibling_pgid);
        if (left_sibling_page) ava_pager_unpin(pager, left_sibling_pgid);
        return AVA_DELETE_OK;
    }

    /* Attempt to merge with right sibling */
    if (right_sibling_page) {
        if (child_page->type == AVA_PAGE_TYPE_LEAF) merge_with_right_leaf(pager, parent_id, parent_page, child_index, child_pgid, child_page, right_sibling_pgid, right_sibling_page);
        else merge_with_right_internal(pager, parent_id, parent_page, child_index, child_pgid, child_page, right_sibling_pgid, right_sibling_page);
    } else if (left_sibling_page) {
        /* Must merge with left sibling */
        if (child_page->type == AVA_PAGE_TYPE_LEAF) merge_with_left_leaf(pager, parent_id, parent_page, child_index, child_pgid, child_page, left_sibling_pgid, left_sibling_page);
        else merge_with_left_internal(pager, parent_id, parent_page, child_index, child_pgid, child_page, left_sibling_pgid, left_sibling_page);
    }

    if (is_node_underflow(pager, parent_page)) {
        ava_pager_unpin(pager, child_pgid);
        if (right_sibling_page) ava_pager_unpin(pager, right_sibling_pgid);
        if (left_sibling_page) ava_pager_unpin(pager, left_sibling_pgid);
        return AVA_DELETE_UNDERFLOW;
    }
    ava_pager_unpin(pager, child_pgid);
    if (right_sibling_page) ava_pager_unpin(pager, right_sibling_pgid);
    if (left_sibling_page) ava_pager_unpin(pager, left_sibling_pgid);
    return AVA_DELETE_OK;
}

static int ava_tree_delete_recursive(AvaPager* pager, ava_pgid_t page_id, char* key, uint8_t key_size) {
    AvaTreePageHeader* page = ava_pager_get(pager, page_id);

    if (page->type == AVA_PAGE_TYPE_LEAF) {
        uint16_t index;
        if (node_find_key(page, key, key_size, &index)) {
            ava_pager_mark_dirty(pager, page_id);

            /* If this is an overflow entry, free the overflow chain before attempting to delete the entry */
            AvaTreeLeafCell* cell = get_cell_content(page, index);
            if (cell->value_type & AVA_VALUE_FLAG_OVERFLOW) {
                ava_pgid_t overflow = *(ava_pgid_t*)(cell->payload + cell->key_size);
                free_overflow_chain(pager, overflow);
            }

            leaf_node_delete(page, index);

            if (is_node_underflow(pager, page)) {
                ava_pager_unpin(pager, page_id);
                return AVA_DELETE_UNDERFLOW;
            }
            ava_pager_unpin(pager, page_id);
            return AVA_DELETE_OK;
        }
        ava_pager_unpin(pager, page_id);
        return AVA_DELETE_OK; /* Not found */
    }

    /* Internal Node */
    uint16_t index;
    int found = node_find_key(page, key, key_size, &index);
    if (found) index++;

    ava_pgid_t next_pgid;
    if (index >= page->header.node.num_entries) {
        next_pgid = page->header.node.right_sibling;
    } else {
        next_pgid = ((AvaTreeInternalCell*)get_cell_content(page, index))->left_child;
    }

    int result = ava_tree_delete_recursive(pager, next_pgid, key, key_size);

    if (result == AVA_DELETE_UNDERFLOW) {
        /* The child we descended from has underflowed. We need to fix it. */
        if (page->type == AVA_PAGE_TYPE_INTERNAL) {
            int res = handle_node_underflow(pager, page, page_id, index);
            ava_pager_unpin(pager, page_id);
            return res;
        } else {
            /* This case (a leaf's child underflowing) is impossible. */
            ava_pager_unpin(pager, page_id);
            return AVA_DELETE_OK;
        }
    }

    ava_pager_unpin(pager, page_id);
    return AVA_DELETE_OK;
}

/* High-level Tree Functions */
AvaTreeLeafCell* ava_tree_search(AvaPager* pager, ava_pgid_t root_pgid, char* key, uint8_t key_size, ava_pgid_t* pgid_index) {
    /* Sanity checks */
    if (root_pgid == 0) return NULL;
    if (pgid_index == NULL) return NULL;

    ava_pgid_t current_pgid = root_pgid;
    AvaTreePageHeader* page = ava_pager_get(pager, current_pgid);
    while (page->type == AVA_PAGE_TYPE_INTERNAL) {
        uint16_t index;
        int found = node_find_key(page, key, key_size, &index);
        /* Since the pointer on each entry contains the keys less than it, we consult the
         * next entry on the node to ensure we navigate towards the correct node
         */
        if (found)
            index++;
        /* Perform a modified binary tree search, each node holds a pointer that
         * points to a node that contains keys less than the specified key.
         * If none match our condition, we use the right sibling pointer on the node.
         */
        ava_pgid_t next_pgid;
        if (index >= page->header.node.num_entries) {
            next_pgid = page->header.node.right_sibling;
        } else {
            AvaTreeInternalCell* cell = get_cell_content(page, index);
            next_pgid = cell->left_child;
        }
        /* Retrieve the next page */
        ava_pager_unpin(pager, current_pgid);
        current_pgid = next_pgid;
        page = (AvaTreePageHeader*)ava_pager_get(pager, current_pgid);
    }
    /* We are at a leaf node, find the cell associated with our key */
    uint16_t index;
    int found = node_find_key(page, key, key_size, &index);
    if (found) {
        /* The caller is responsible for unpinning the page, hence why we can pass the index to it */
        *pgid_index = current_pgid;
        return get_cell_content(page, index);
    }
    ava_pager_unpin(pager, current_pgid);
    return NULL;
}

ava_pgid_t ava_tree_insert(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size, char* value, uint32_t value_size, uint8_t value_type) {
    /* If the size of the value hits past threshold (25%), we push the value to an overflow chain */
    if (value_size > (pager->page_size / 4)) {
        ava_pgid_t overflow = create_overflow_chain(pager, value, value_size);

        return ava_tree_insert(pager, root, key, key_size,
            (char*)&overflow,
            sizeof(ava_pgid_t),
            value_type | AVA_VALUE_FLAG_OVERFLOW);
    }
    /* If the tree is empty, we must create the first node */
    if (root == 0) {
        ava_pgid_t root_pgid;
        ava_pager_allocate(pager, &root_pgid);
        AvaTreePageHeader* root_page = ava_pager_get(pager, root_pgid);
        ava_pager_mark_dirty(pager, root_pgid); /* Mark page as dirty for modification */

        /* Initialize the new leaf page */
        root_page->type = AVA_PAGE_TYPE_LEAF;
        root_page->header.node.num_entries = 0;
        root_page->header.node.free_end = pager->page_size;
        root_page->header.node.right_sibling = 0; /* No siblings yet */

        /* Insert the first element into this new page. The index is always 0. */
        leaf_node_insert(pager, root_page, 0, key, key_size, value, value_size, value_type);

        ava_pager_unpin(pager, root_pgid);
        return root_pgid;
    } else {
        AvaSplitResult result;
        ava_tree_insert_recursive(pager, root, key, key_size, value, value_size, value_type, &result);

        if (result.did_split) {
            /* The root itself split. We must create a new root. */
            ava_pgid_t new_root_id;
            ava_pager_allocate(pager,&new_root_id);
            AvaTreePageHeader* new_root = ava_pager_get(pager, new_root_id);
            ava_pager_mark_dirty(pager, new_root_id);

            new_root->type = AVA_PAGE_TYPE_INTERNAL;
            new_root->header.node.num_entries = 0;
            new_root->header.node.free_end = pager->page_size;

            /* The new root has two children: the old root and the new page from the split */
            new_root->header.node.right_sibling = result.new_page_id;
            internal_node_insert(pager, new_root, 0, root, (char*)result.promoted_key, result.promoted_key_size);
            ava_pager_unpin(pager, root);
            ava_pager_unpin(pager, new_root_id);
            return new_root_id;
        }
        ava_pager_unpin(pager, root);
        return root;
    }
}

ava_pgid_t ava_tree_delete(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size) {
    if (root == 0) return 0;

    ava_tree_delete_recursive(pager, root, key, key_size);

    /* Handle Root Collapse: If internal root has 0 entries (only 1 child pointer), promote the child */
    AvaTreePageHeader* root_page = ava_pager_get(pager, root);
    if (root_page->type == AVA_PAGE_TYPE_INTERNAL && root_page->header.node.num_entries == 0) {
        ava_pgid_t new_root = root_page->header.node.right_sibling;
        ava_pager_unpin(pager, root);
        return new_root;
    }
    ava_pager_unpin(pager, root);
    return root;
}
