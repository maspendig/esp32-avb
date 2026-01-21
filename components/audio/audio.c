/**
 * This file provides interfaces to interact with audio codecs and handle audio data.
 * The used codec is defined via CONFIG_AUDIO_CODEC in the project configuration.
 * Currently supported codecs are:
 * - ES8311 (default - onboard ESP32-P4-Function-EV-Board codec)
 * - TLV320AIC32X4 (audio cape required)
 * - AK4619 (TDM mode - audio cape required)
 */

#include "audio.h"
#define CONFIG_CODEC_ES8311 1

#define TAG "audio"
#ifdef CONFIG_CODEC_ES8311
#include "es8311.h"
#define CODEC_INIT() es8311_init()
#define CODEC_I2S_WRITE(buf, size, bytes_written) i2s_write(buf, size, bytes_written)
#elif CONFIG_CODEC_TLV320AIC3254
#include "codec.h"
#define CODEC_INIT() InitCodec()
#define CODEC_I2S_READ(buf, size, bytes_read) i2s_read(buf, size, bytes_read)
#define CODEC_I2S_WRITE(buf, size, bytes_written) i2s_write(buf, size, bytes_written)
#define CODEC_SET_OUTPUT_LEVELS(left, right) SetOutputLevels(left, right)
#elif CONFIG_CODEC_AK4619
#include "ak4619_tdm.h"
#define CODEC_INIT() ak4619_tdm_init()
#define CODEC_I2S_READ(buf, size, bytes_read) ak4619_i2s_read(buf, size, bytes_read)
#define CODEC_I2S_WRITE(buf, size, bytes_written) ak4619_i2s_write(buf, size, bytes_written)
#define CODEC_SET_OUTPUT_LEVELS(left, right) // AK4619 output levels set in init
#else
#error "No codec selected in menuconfig"
#endif

