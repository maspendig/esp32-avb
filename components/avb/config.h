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

// Entity capabilities
#define CONFIG_ENTITY_CAPABILITIES 0x0000C508

// ----- Audio Configuration -----
// Number of audio channels (stereo = 2)
#define CONFIG_AUDIO_CHANNELS 8

// Listener configuration (stream sink - receives audio)
// This device has 1 stereo stream input
#define CONFIG_NUM_STREAM_INPUTS 1
#define CONFIG_LISTENER_STREAM_SINKS 1
#define CONFIG_LISTENER_CAPABILITIES 0x4001

// Talker configuration (stream source - sends audio)
// This device has 1 stereo stream output (if acting as talker)

// #define CONFIG_NUM_STREAM_OUTPUTS 0
// #define CONFIG_TALKER_STREAM_SOURCES 1
// #define CONFIG_TALKER_CAPABILITIES 0x4001

// TODO should be always the same!?
#define CONFIG_NUM_STREAM_OUTPUTS 0
#define CONFIG_TALKER_STREAM_SOURCES 0
#define CONFIG_TALKER_CAPABILITIES 0x0000

// Stream port configuration
// Each stream port has clusters that map audio channels
// For stereo: 1 cluster with 2 channels OR 2 clusters with 1 channel each
// Using 1 cluster with 2 channels (simpler)
#define CONFIG_NUM_AUDIO_CLUSTERS 1
#define CONFIG_CHANNELS_PER_CLUSTER CONFIG_AUDIO_CHANNELS

// External ports represent physical I/O (e.g., speaker outputs, mic inputs)
// For a listener device outputting stereo audio: 2 external output ports
#define CONFIG_NUM_EXTERNAL_INPUT_PORTS 0
#define CONFIG_NUM_EXTERNAL_OUTPUT_PORTS CONFIG_AUDIO_CHANNELS

// Audio maps define channel routing between streams and clusters
// For stereo: 2 mappings (one per channel)
#define CONFIG_NUM_AUDIO_MAPS 1
#define CONFIG_NUM_AUDIO_MAPPINGS CONFIG_AUDIO_CHANNELS

#define CONFIG_CONTROLLER_CAPABILITIES 0x0000

// TODO replace with MAAP handled MAC
#define MAAP_MAC_ADDRESS (u8[6]){0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}

// Supported stream formats (AAF format)
// Format: 0x00a0CCSSFNNNNNNN where CC=channels, SS=sample size, F=format, N=sample rate
// 0x00a0020860000800 = 2ch, 24-bit, 48kHz
static const u64 stream_formats[] = {
  0x00a0020840000800,
};


#define CONFIG_NUM_STREAM_FORMATS (sizeof(stream_formats) / sizeof(stream_formats[0]))

// Current stream format (should match one of the supported formats)

#define CONFIG_CURRENT_STREAM_FORMAT 0x00a0020840000800

#endif //ETHERNET_PTP_CONFIG_H

