#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "avainterface.h"

typedef uint64_t ava_pgid_t;

typedef struct AvaPager {
    uint16_t page_size;
    uint32_t max_page_quota;
    AvaOSInterface** interface;
    AvaFile** file;
    ava_pgid_t* page_nums;
    void* pages;
} AvaPager;

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file);
void ava_pager_deinit(AvaPager* pager);
void* ava_pager_read(AvaPager* pager, ava_pgid_t index);
void ava_pager_mark_dirty(AvaPager* pager, ava_pgid_t index);
bool ava_pager_allocate(AvaPager* pager, ava_pgid_t* new_index);
void ava_pager_sync(AvaPager* pager);