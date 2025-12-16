//
// Created by max on 11/21/25.

#ifndef ETHERNET_PTP_CONFIG_H
#define ETHERNET_PTP_CONFIG_H

#import "types.h"

#define CONFIG_ENTITY_NAME "ESP32-P4"
#define CONFIG_FW_VERSION "0.0.1"
#define CONFIG_GROUP_NAME NULL

// String descriptors
#define CONFIG_VENDOR_NAME "HAW Kiel"
#define CONFIG_MODEL_NAME "ESP32-P4"
#define CONFIG_CLOCK_SOURCE_NAME "Internal"
#define CONFIG_CLOCK_DOMAIN_NAME "Internal Clock Domain"
#define CONFIG_LOCALE_IDENTIFIER "en-US"

#define CONFIG_SAMPLING_RATE 48000
#define CONFIG_ENTITY_CAPABILITIES 0x0000C508
#define CONFIG_TALKER_STREAM_SOURCES 8
#define CONFIG_TALKER_CAPABILITIES 0x4001
#define CONFIG_LISTENER_STREAM_SINKS 8
#define CONFIG_LISTENER_CAPABILITIES 0x4001

#define CONFIG_CONTROLLER_CAPABILITIES 0x0000

// TODO replace with MAAP handled MAC
#define MAAP_MAC_ADDRESS (u8[6]){0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}

static const u64 stream_formats[] = {
  0x00a0020860000800,
  0x00a0040860000800,
  0x00a0060860000800,
};
// current stream format differs from above formats by having 0840 instead of 0860 - why?
// 0x00a0020840000800,

#endif //ETHERNET_PTP_CONFIG_H

