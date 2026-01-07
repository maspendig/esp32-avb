//
// Created by max on 1/7/26.
//

#ifndef ETHERNET_PTP_COMMON_H
#define ETHERNET_PTP_COMMON_H
#include <esp_random.h>
#include "types.h"

inline u32 random_in_range(const u32 min, const u32 max)
{
  u32 r = esp_random();
  u32 span = (max - min + 1);
  return (u32)(r % span) + min;
}

#endif //ETHERNET_PTP_COMMON_H
