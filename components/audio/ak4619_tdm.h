#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ak4619_tdm_init(void);
esp_err_t ak4619_read_register(uint8_t reg, uint8_t *val);
esp_err_t ak4619_write_register(uint8_t reg, uint8_t val);

// I2S read/write wrappers compatible with TLV codec interface
void ak4619_i2s_read(void *buf, uint32_t size, uint32_t *bytes_read);
void ak4619_i2s_write(void *buf, uint32_t size, uint32_t *bytes_written);

// DAC Volume Control
// Volume range: 0x00 (+12.0dB) to 0xFF (Mute)
// 0x18 = 0dB (default), 0.5dB steps
// Attenuation increases with higher values
esp_err_t ak4619_set_dac_volume(uint8_t dac_num, uint8_t left_vol, uint8_t right_vol);
esp_err_t ak4619_set_dac1_volume(uint8_t left_vol, uint8_t right_vol);
esp_err_t ak4619_set_dac2_volume(uint8_t left_vol, uint8_t right_vol);
esp_err_t ak4619_set_all_dac_volume(uint8_t volume);

// Helper function to convert dB to register value
// db_value: -115.0 to +12.0 (in 0.5dB steps)
// Returns: register value (0x00 to 0xFF)
uint8_t ak4619_db_to_volume(float db_value);

