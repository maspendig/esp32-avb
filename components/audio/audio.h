//
// Created by max on 1/21/26.
//

#ifndef ETHERNET_PTP_AUDIO_H
#define ETHERNET_PTP_AUDIO_H
#include <stdlib.h>
#include <sys/types.h>
#include "sdkconfig.h"

#ifdef CONFIG_CODEC_ES8311
#include "es8311.h"
#define SAMPLE_BIT_RATE 16
#define INPUT_CHANNELS 1
#define OUTPUT_CHANNELS 2
#define CODEC_INIT() es8311_init()
#define CODEC_I2S_WRITE(buf, size, bytes_written) es8311_i2s_write(buf, size, bytes_written)
#elif CONFIG_CODEC_TLV320AIC3254
#include "codec.h"
#define INPUT_CHANNELS 0
#define OUTPUT_CHANNELS 2
#define CODEC_INIT() InitCodec()
#define CODEC_I2S_READ(buf, size, bytes_read) i2s_read(buf, size, bytes_read)
#define CODEC_I2S_WRITE(buf, size, bytes_written) i2s_write(buf, size, bytes_written)
#define CODEC_SET_OUTPUT_LEVELS(left, right) SetOutputLevels(left, right)
#elif CONFIG_CODEC_AK4619
#include "ak4619_tdm.h"
#define INPUT_CHANNELS 4
#define OUTPUT_CHANNELS 4
#define CODEC_INIT() ak4619_tdm_init()
#define CODEC_I2S_READ(buf, size, bytes_read) ak4619_i2s_read(buf, size, bytes_read)
#define CODEC_I2S_WRITE(buf, size, bytes_written) ak4619_i2s_write(buf, size, bytes_written)
#define CODEC_SET_OUTPUT_LEVELS(left, right) // AK4619 output levels set in init
#else
#error "No codec selected in menuconfig"
#endif

static inline int16_t convert_sample_24_to_16_dither(int32_t s24)
{
  // s24: signed 24-bit value in int32_t, range [-8388608, 8388607]

  // Generate TPDF dither in [-0.5, +0.5) LSB of TARGET (16-bit).
  // Here: two uniform integers in [0, 255], difference in [-255, +255].
  int32_t r1 = rand() & 0xFF;
  int32_t r2 = rand() & 0xFF;
  int32_t tpdf = r1 - r2; // [-255, 255]

  // Scale noise to roughly ±1 LSB at 16-bit.
  // 1 LSB at 16-bit corresponds to 256 LSB at 24-bit.
  // We add dither in the 24-bit domain before shifting.
  int32_t noise_24 = tpdf; // already about ±256 peak

  int32_t v = s24 + noise_24; // add dither

  // Round to nearest when shifting down by 8 bits.
  // Add 0.5 LSB of the discarded bits (here 2^7) before shifting.
  v += (v >= 0 ? 128 : -128);

  // Shift to 16-bit domain
  int32_t s16 = v >> 8;

  // Saturate to int16_t
  if (s16 > 32767) s16 = 32767;
  if (s16 < -32768) s16 = -32768;

  return (int16_t)s16;
}

void init_audio_codec();
void empty_audio_buffer();
#endif //ETHERNET_PTP_AUDIO_H
