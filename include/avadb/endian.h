#pragma once
#include <stdint.h>

/* Portable header that handles byteswapping to keep behaviour consistent
 * between little and big endian systems
 * All on-disk data is stored in little endian to ensure that
 * those systems do not suffer any timing penalties due to the endian difference.
 */

#if defined(_MSC_VER)
#include <stdlib.h>
#define bswap16 _byteswap_ushort
#define bswap32 _byteswap_ulong
#define bswap64 _byteswap_uint64
#elif defined(__GNUC__) || defined(__clang__)
#define bswap16 __builtin_bswap16
#define bswap32 __builtin_bswap32
#define bswap64 __builtin_bswap64
#else
static inline uint16_t bswap16(uint16_t x) {
    return (x >> 8) | (x << 8);
}
static inline uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) |
        ((x << 8) & 0xff0000) | ((x << 24) & 0xff000000);
}
static inline uint64_t bswap64(uint64_t x) {
    return ((x >> 56) & 0xff) | ((x >> 40) & 0xff00) |
        ((x >> 24) & 0xff0000) | ((x >> 8) & 0xff000000) |
        ((x << 8) & 0xff00000000) | ((x << 24) & 0xff0000000000) |
        ((x << 40) & 0xff000000000000) | ((x << 56) & 0xff00000000000000);
}
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #define AVA_LITTLE_ENDIAN 1
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64)
    /* All MSVC platforms are little endian */
    #define AVA_LITTLE_ENDIAN 1
#else
    #define AVA_LITTLE_ENDIAN 0
#endif

#if AVA_LITTLE_ENDIAN
    #define htole16(x) (x)
    #define htole32(x) (x)
    #define htole64(x) (x)
    #define le16toh(x) (x)
    #define le32toh(x) (x)
    #define le64toh(x) (x)
#else
    #define htole16(x) bswap16(x)
    #define htole32(x) bswap32(x)
    #define htole64(x) bswap64(x)
    #define le16toh(x) bswap16(x)
    #define le32toh(x) bswap32(x)
    #define le64toh(x) bswap64(x)
#endif