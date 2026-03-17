#pragma once
#include <stdint.h>
#include "packed.h"
#include "avapager.h"
/*
 * AvaDB uses B+ Trees to store key-value pairs, each key may be an arbitrary binary of any length, while a value
 * can either be an arbitrary binary, or a subtree allowing for hierarchical relational organization.
 */

/*
 * Originally I was going to implement this as an enum, but because the "geniuses" behind ISO thought
 * it was a good idea to make enum sizing implementation defined until ISO C23, we don't have
 * that luxury here. What's also annoying is the fact this feature has been standardized in C++ since 2011.
 * Why this wasn't part of C for longer is something I still don't understand, thanks ISO...
 */
#define AVA_PAGE_TYPE_FREE 0
#define AVA_PAGE_TYPE_INTERNAL 1
#define AVA_PAGE_TYPE_LEAF 2
#define AVA_PAGE_TYPE_OVERFLOW 3

#define AVA_VALUE_TYPE_NULL 0
#define AVA_VALUE_TYPE_INT 1
#define AVA_VALUE_TYPE_FLOAT 2
#define AVA_VALUE_TYPE_STRING 3
#define AVA_VALUE_TYPE_BLOB 4
#define AVA_VALUE_TYPE_TREE 5
#define AVA_VALUE_FLAG_OVERFLOW 0x80

typedef uint16_t ava_off_t;

/* Slotted Page B+ Tree format */
typedef packed_struct AvaTreePageHeader {
    uint8_t type;
    uint8_t unused1[7]; /* Ensures that the next value is properly aligned, may have future use */

    union {
        struct {
            ava_pgid_t next_free;
        } free;
        struct {
            uint16_t num_entries;
            uint16_t fragments_size;
            ava_off_t free_end;
            uint16_t unused2; /* Ensures that the next value is properly aligned, may have future use */
            ava_pgid_t right_sibling;
        } node;
    } type_header;
} AvaTreePageHeader;

/* Pointers to cells, located immediately after the header in the Slotted Page */
typedef struct AvaTreeCellPtr {
    ava_off_t offset;
    uint16_t size;
} AvaTreeCellPtr;

/* Internal nodes hold a Child Page ID and the Separator Key (No values) */
typedef packed_struct AvaTreeInternalCell {
    ava_pgid_t left_child; /* Points to subtree where keys < this key */
    uint8_t key_size;
    uint8_t key[];
} AvaTreeInternalCell;

/* Leaf nodes hold the Metadata + Key + Value (or Overflow Page ID) */
typedef packed_struct AvaTreeLeafCell {
    uint32_t value_size;
    uint8_t value_type; /* Used to hint to the user what type this is storing */
    uint8_t key_size;
    uint8_t payload[]; /* [Key Bytes] followed by [Value Bytes or OverflowPgID] */
} AvaTreeLeafCell;

void ava_tree_insert(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size, char* value, uint32_t value_size, uint8_t value_type);
void ava_tree_replace(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size, char* value, uint32_t value_size, uint8_t value_type);
void ava_tree_delete(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size);
AvaTreeLeafCell* ava_tree_search(AvaPager* pager, ava_pgid_t root, char* key, uint8_t key_size);