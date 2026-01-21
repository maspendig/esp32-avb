/**
 * This file provides interfaces to interact with audio codecs and handle audio data.
 * The used codec is defined via CONFIG_AUDIO_CODEC in the project configuration.
 * Currently supported codecs are:
 * - ES8311 (default - onboard ESP32-P4-Function-EV-Board codec)
 * - TLV320AIC32X4 (audio cape required)
 * - AK4619 (TDM mode - audio cape required)
 */

#include "audio.h"

#define TAG "audio"
