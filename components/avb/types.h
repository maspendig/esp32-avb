//
// Created by max on 11/20/25.
//

#ifndef ETHERNET_PTP_TYPES_H
#define ETHERNET_PTP_TYPES_H

#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

struct Node
{
  struct Node* next;
  struct Node* prev;
};

struct header_s
{
  u8 dst_mac[6];
  u8 src_mac[6];
  u8 eth_type[2];
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


inline void list_append(struct Node* head, struct Node* entry)
{
  struct Node* last_entry = head->prev;

  // | head | <-> | ... | <-> | last_entry | <-> | entry |
  entry->prev = last_entry;
  last_entry->next = entry;

  head->prev = entry;
  entry->next = head;
}

#endif //ETHERNET_PTP_TYPES_H
