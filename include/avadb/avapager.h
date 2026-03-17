#pragma once
#include <stdint.h>
#include "avainterface.h"

typedef struct AvaPager {
    uint16_t page_size;
    uint32_t max_page_quota;
    AvaOSInterface** interface;
    AvaFile** file;
    void* pages;
} AvaPager;

void ava_pager_init(AvaPager* pager, AvaOSInterface** interface, AvaFile** file);
void ava_pager_deinit(AvaPager* pager);
void* ava_pager_read(AvaPager* pager, uint64_t index);
void ava_pager_flush(AvaPager* pager, void* ptr);