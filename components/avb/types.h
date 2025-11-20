//
// Created by max on 11/20/25.
//

#ifndef ETHERNET_PTP_TYPES_H
#define ETHERNET_PTP_TYPES_H

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

struct header_s
{
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint8_t eth_type[2];
};

#ifndef ntohll
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ntohll(x) ((uint64_t)( \
(((uint64_t)(x) & 0x00000000000000ffULL) << 56) | \
(((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | \
(((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | \
(((uint64_t)(x) & 0x00000000ff000000ULL) << 8)  | \
(((uint64_t)(x) & 0x000000ff00000000ULL) >> 8)  | \
(((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | \
(((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | \
(((uint64_t)(x) & 0xff00000000000000ULL) >> 56) ))
#define htonll(x) ntohll(x)
#else
#define ntohll(x) ((uint64_t)(x))
#define htonll(x) ((uint64_t)(x))
#endif
#endif

#endif //ETHERNET_PTP_TYPES_H
