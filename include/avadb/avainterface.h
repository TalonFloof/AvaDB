#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* AvaFile;

typedef struct AvaOSInterface {
    uint16_t version; /* Currently set to 1 */
    int (*open)(struct AvaOSInterface*, const char*, uint16_t, AvaFile**);
    int (*close)(struct AvaOSInterface*, AvaFile*);
    size_t (*read)(struct AvaOSInterface*, AvaFile*, void*, size_t, uint64_t);
    size_t (*write)(struct AvaOSInterface*, AvaFile*, void*, size_t, uint64_t);
    size_t (*truncate)(struct AvaOSInterface*, AvaFile*, size_t);
    size_t (*getsize)(struct AvaOSInterface*, AvaFile*);
    int (*sync)(struct AvaOSInterface*, AvaFile*);
    int (*lock)(struct AvaOSInterface*, AvaFile*);
    int (*unlock)(struct AvaOSInterface*, AvaFile*);
} AvaOSInterface;

#ifdef __cplusplus
}
#endif