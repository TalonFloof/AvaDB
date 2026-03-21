#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "avainterface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t ava_pgid_t;

typedef struct AvaPagerSlot {
    ava_pgid_t pgid;
    void* data;
    uint16_t pin_count;
    bool dirty;

    struct AvaPagerSlot* prev;
    struct AvaPagerSlot* next;
} AvaPagerSlot;

typedef struct AvaPager {
    uint16_t page_size;
    uint32_t cache_capacity;
    uint32_t cache_size;
    AvaOSInterface** interface;
    AvaFile** file;

    AvaPagerSlot* slots;
    AvaPagerSlot** hash_map; /* For O(1) lookups of page slots */
    uint32_t hash_map_size;
    AvaPagerSlot* head; /* Most Recently Used page */
    AvaPagerSlot* tail; /* Least Recently Used page */
} AvaPager;

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file);
void ava_pager_deinit(AvaPager* pager);
void* ava_pager_get(AvaPager* pager, ava_pgid_t pgid);
void ava_pager_mark_dirty(AvaPager* pager, ava_pgid_t pgid);
void ava_pager_unpin(AvaPager* pager, ava_pgid_t pgid);
bool ava_pager_allocate(AvaPager* pager, ava_pgid_t* new_pgid);
void ava_pager_free(AvaPager* pager, ava_pgid_t pgid);
void ava_pager_sync(AvaPager* pager);

#ifdef __cplusplus
}
#endif