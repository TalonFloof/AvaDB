#include <stdlib.h>
#include <avadb/avapager.h>

#include "avadb/avadb.h"
#include "avadb/avatree.h"

/* --- LRU List Helpers --- */
static void lru_remove(AvaPager* pager, AvaPagerSlot* slot) {
    if (slot->prev) slot->prev->next = slot->next;
    else pager->head = slot->next;
    if (slot->next) slot->next->prev = slot->prev;
    else pager->tail = slot->prev;
}

static void lru_add_head(AvaPager* pager, AvaPagerSlot* slot) {
    slot->next = pager->head;
    slot->prev = NULL;
    if (pager->head)
        pager->head->prev = slot;
    pager->head = slot;
    if (!pager->tail)
        pager->tail = slot;
}

/* --- Hash Map Helpers --- */
static uint32_t hash_pgid(ava_pgid_t pgid) {
    return (uint32_t)(pgid ^ (pgid >> 32));
}

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file) {
    /* If no values were specified, fill them with the default ones */
    if (pager->page_size < 512)
        pager->page_size = 4096;
    if (pager->cache_capacity < 16)
        pager->cache_capacity = 512;
    pager->interface = interface;
    pager->file = file;
    pager->slots = calloc(pager->cache_capacity, sizeof(AvaPagerSlot));
    for (uint32_t i=0; i < pager->cache_capacity; i++) {
        pager->slots[i].data = malloc(pager->page_size);
    }

    pager->hash_map_size = pager->cache_capacity * 2;
    pager->hash_map = calloc(pager->hash_map_size, sizeof(AvaPagerSlot*));
    pager->head = NULL;
    pager->tail = NULL;
    pager->cache_size = 0;
}

void ava_pager_deinit(AvaPager* pager) {
    /* Free the pager's page cache */
    ava_pager_sync(pager);
    if (pager->slots) {
        for (uint32_t i=0; i < pager->cache_capacity; i++) {
            free(pager->slots[i].data);
        }
        free(pager->slots);
    }
    if (pager->hash_map)
        free(pager->hash_map);
}

static AvaPagerSlot* find_slot(AvaPager* pager, ava_pgid_t pgid) {
    uint32_t index = hash_pgid(pgid) % pager->hash_map_size;
    /* TODO: Improve worse-case, currently O(n) due to linear probing */
    if (!pager->hash_map[index])
        return NULL;
    for (uint32_t i=0; i < pager->hash_map_size; i++) {
        uint32_t cur = (index + i) % pager->hash_map_size;
        if (pager->hash_map[cur] && pager->hash_map[cur]->pgid == pgid) {
            return pager->hash_map[cur];
        }
    }
    return NULL;
}

static AvaPagerSlot* evict_page(AvaPager* pager) {
    /* Find a page from the tail to the head that we can evict */
    for (AvaPagerSlot* slot = pager->tail; slot != NULL; slot = slot->prev) {
        if (slot->pin_count == 0) {
            if (slot->dirty) {
                /* Write dirty page to disk */
                (*pager->interface)->write(*pager->interface, *pager->file, slot->data, pager->page_size, slot->pgid * pager->page_size);
            }
            /* Remove entry from hashmap and page LRU */
            /* TODO: Improve worse-case, currently O(n) due to linear probing */
            uint32_t index = hash_pgid(slot->pgid) % pager->hash_map_size;
            for (uint32_t i=0; i < pager->hash_map_size; i++) {
                uint32_t cur = (index + i) % pager->hash_map_size;
                if (pager->hash_map[cur] && pager->hash_map[cur]->pgid == slot->pgid) {
                    pager->hash_map[cur] = NULL;
                    lru_remove(pager, slot);
                    return slot;
                }
            }
        }
    }
    return NULL;
}

void* ava_pager_get(AvaPager* pager, ava_pgid_t pgid) {
    AvaPagerSlot* slot = find_slot(pager, pgid);
    if (slot) {
        /* Page is currently in the cache, add a pin to it and move it to the head */
        slot->pin_count++;
        lru_remove(pager, slot);
        lru_add_head(pager, slot);
        return slot->data;
    }

    /* If under the page cache limit, load it
     * Otherwise evict a page and then load it
     */
    if (pager->cache_size < pager->cache_capacity) {
        slot = &pager->slots[pager->cache_size++];
    } else {
        slot = evict_page(pager);
        if (!slot)
            return NULL;
    }

    /* Read the page from the disk */
    (*pager->interface)->read(*pager->interface, *pager->file, slot->data, pager->page_size, pgid * pager->page_size);

    slot->pgid = pgid;
    slot->pin_count = 1;
    slot->dirty = false;

    /* TODO: Improve worse-case, currently O(n) due to linear probing */
    uint32_t index = hash_pgid(slot->pgid) % pager->hash_map_size;
    for (uint32_t i=0; i < pager->hash_map_size; i++) {
        uint32_t cur = (index + i) % pager->hash_map_size;
        if (!pager->hash_map[cur]) {
            pager->hash_map[cur] = slot;
            lru_add_head(pager, slot);
            return slot->data;
        }
    }
    return NULL;
}

void ava_pager_unpin(AvaPager* pager, ava_pgid_t pgid) {
    AvaPagerSlot* slot = find_slot(pager, pgid);
    if (slot && slot->pin_count > 0) {
        slot->pin_count--;
    }
}

void ava_pager_mark_dirty(AvaPager* pager, ava_pgid_t pgid) {
    AvaPagerSlot* slot = find_slot(pager, pgid);
    if (slot) slot->dirty = true;
}

bool ava_pager_allocate(AvaPager* pager, ava_pgid_t* new_pgid) {
    AvaDBFileHeader* header = ava_pager_get(pager,0);
    if (header->free_page_start != 0) {
        ava_pgid_t index = header->free_page_start;
        AvaTreePageHeader* page = ava_pager_get(pager, index);
        if (page->type != AVA_PAGE_TYPE_FREE) {
            ava_pager_unpin(pager, index);
            ava_pager_unpin(pager, 0);
            return false;
        }
        header->free_page_start = page->header.free.next_free;
        ava_pager_mark_dirty(pager,index);
        page->header.free.next_free = 0;
        ava_pager_mark_dirty(pager,0);
        *new_pgid = index;
        ava_pager_unpin(pager, index);
        ava_pager_unpin(pager, 0);
        return true;
    }
    ava_pager_unpin(pager, 0);

    size_t cur_size = (*pager->interface)->getsize(*pager->interface, *pager->file);
    (*pager->interface)->truncate(*pager->interface, *pager->file, cur_size+pager->page_size);
    *new_pgid = cur_size / pager->page_size;
    return true;
}
void ava_pager_free(AvaPager* pager, ava_pgid_t pgid) {
    AvaDBFileHeader* header = ava_pager_get(pager,0);
    AvaTreePageHeader* page = ava_pager_get(pager,pgid);
    ava_pager_mark_dirty(pager,0);
    ava_pager_mark_dirty(pager,pgid);

    page->type = AVA_PAGE_TYPE_FREE;
    page->header.free.next_free = header->free_page_start;
    header->free_page_start = pgid;

    ava_pager_unpin(pager, pgid);
    ava_pager_unpin(pager, 0);
}
void ava_pager_sync(AvaPager* pager) {
    /* TODO: Implement pager syncing */
}