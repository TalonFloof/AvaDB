#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* AvaFile;

typedef struct AvaOSInterface {
    uint16_t version; /* Currently set to 1 */
    int (*open)(struct AvaOSInterface*, const char*, uint16_t, AvaFile**);
    int (*close)(struct AvaOSInterface*, AvaFile*);
    int (*read)(struct AvaOSInterface*, AvaFile*, void*, int, uint64_t);
    int (*write)(struct AvaOSInterface*, AvaFile*, void*, int, uint64_t);
    int (*truncate)(struct AvaOSInterface*, AvaFile*, uint64_t);
    int (*getsize)(struct AvaOSInterface*, AvaFile*);
    int (*sync)(struct AvaOSInterface*, AvaFile*);
    int (*lock)(struct AvaOSInterface*, AvaFile*);
    int (*unlock)(struct AvaOSInterface*, AvaFile*);
} AvaOSInterface;

#ifdef __cplusplus
}
#endif