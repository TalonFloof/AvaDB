#pragma once
#include <stdint.h>
#include "avapager.h"
#include "avainterface.h"

typedef struct AvaDB {
    AvaOSInterface* interface;
    AvaFile* file;
    AvaPager pager;
} AvaDB;

typedef struct AvaDBFileHeader {
    uint8_t magic[8];
    uint16_t version;
    uint16_t page_size;
    uint32_t unused1; /* This entry exists for alignment */
    uint64_t num_pages;
    uint64_t free_page_start;
    uint64_t root_tree_index;
} AvaDBFileHeader;