//
// Created by max on 1/20/26.
//

#ifndef ETHERNET_PTP_ES8311_H
#define ETHERNET_PTP_ES8311_H

#include <esp_err.h>
#include <driver/i2s_types.h>

/* I2C configuration for ESP32-P4-Function-EV-Board */
#define I2C_NUM                 0
#define I2C_SCL_IO              GPIO_NUM_8
#define I2C_SDA_IO              GPIO_NUM_7

/* I2S configuration for ESP32-P4-Function-EV-Board */
#define I2S_NUM                 0
#define I2S_MCK_IO              GPIO_NUM_13
#define I2S_BCK_IO              GPIO_NUM_12
#define I2S_WS_IO               GPIO_NUM_10
#define I2S_DO_IO               GPIO_NUM_9
#define I2S_DI_IO               GPIO_NUM_11

/* PA control GPIO */
#define PA_CTRL_IO              GPIO_NUM_53

/* Audio configuration based on AVB config and ES8311 example */
#define AUDIO_SAMPLE_RATE       CONFIG_SAMPLING_RATE
#define AUDIO_MCLK_MULTIPLE     384  // For 24-bit compatibility
#define AUDIO_VOICE_VOLUME      60   // 0-100

/**
 * @brief Initialize ES8311 codec and I2S driver
 */
void es8311_init();

/**
 * @brief Write audio data to I2S
 *
 * @param data Buffer containing audio data
 * @param size Size of buffer in bytes
 * @param bytes_written Pointer to store number of bytes written
 */
void es8311_i2s_write(const void* data, size_t size, size_t* bytes_written);

#endif //ETHERNET_PTP_ES8311_H
