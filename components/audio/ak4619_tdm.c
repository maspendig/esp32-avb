#include "ak4619_tdm.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_tdm.h"
#include "soc/rtc.h"

// AK4619 I2C 7-bit slave address (MSB 7 bits: "0010000" = 0x10)
#define AK4619_ADDR      0x10

// AK4619 Registers
// Registers
#define REG_POWER         0x00
#define REG_AUDIO_IF0     0x01
#define REG_AUDIO_IF1     0x02
#define REG_SYSCLK        0x03
#define REG_ADC_VOL0      0x06
#define REG_ADC_VOL1      0x07
#define REG_ADC_VOL2      0x08
#define REG_ADC_VOL3      0x09
#define REG_ADC_IN_SEL    0x0B
#define REG_DAC_VOL0      0x0E
#define REG_DAC_VOL1      0x0F
#define REG_DAC_VOL2      0x10
#define REG_DAC_VOL3      0x11

// Pins from user
#define I2C_PORT_NUM     I2C_NUM_1
#define I2C_SDA          GPIO_NUM_7
#define I2C_SCL          GPIO_NUM_8
#define I2C_CLK_SPEED    400000

// I2S Pins
#define I2S_PORT_NUM  I2S_NUM_0
#define I2S_MCLK     GPIO_NUM_22
#define I2S_BCLK     GPIO_NUM_47
#define I2S_WS       GPIO_NUM_48
#define I2S_DOUT     GPIO_NUM_46
#define I2S_DIN      GPIO_NUM_23

#define SAMPLE_RATE 48000

static i2s_chan_handle_t tx_chan;
static i2s_chan_handle_t rx_chan;
static i2c_master_dev_handle_t ak4619_dev_handle;


// Optional reset pin (set to GPIO_NUM_NC if unused)
#define AK4619_RESET_PIN GPIO_NUM_53

static const char* TAG = "AK4619";

static void ak4619_tune_apll(void)
{
  // Standard 48kHz setup uses APLL = 48000 * 768 = 36864000 Hz
  // Best results with AK4619-2 with calculated 47,999.182 Hz

  // AK4619-2
  uint32_t target_freq = 36863372;

  // AK4619-1
  // uint32_t target_freq = 36863670;
  uint32_t o_div = 0;
  uint32_t sdm0 = 0;
  uint32_t sdm1 = 0;
  uint32_t sdm2 = 0;

  // Calculate coefficients for the exact target frequency
  uint32_t real_freq = rtc_clk_apll_coeff_calc(target_freq, &o_div, &sdm0, &sdm1, &sdm2);

  ESP_LOGW(TAG, "  Target APLL: %lu Hz", target_freq);
  ESP_LOGW(TAG, "  Actual APLL: %lu Hz", real_freq);
  ESP_LOGW(TAG, "  Coeffs: o_div=%lu, sdm0=%lu, sdm1=%lu, sdm2=%lu", o_div, sdm0, sdm1, sdm2);

  // Apply the calculated coefficients
  rtc_clk_apll_coeff_set(o_div, sdm0, sdm1, sdm2);
  rtc_clk_apll_enable(true);
}

// setup i2s for ak4619 in tdm mode
static esp_err_t ak4619_setup_i2s(void)
{
  esp_err_t ret;

  // Create I2S channel in TDM mode
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true; // Clear DMA buffer on underflow
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 32;

  ret = i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S new channel failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Configure TDM clock
  i2s_tdm_clk_config_t clk_cfg = {
    .sample_rate_hz = SAMPLE_RATE,
    .clk_src = I2S_CLK_SRC_APLL,
    .mclk_multiple = I2S_MCLK_MULTIPLE_384 // 384fs for TDM mode (4 slots × 32-bit × 2 sides = 256 BCLK cycles)
  };

  // Configure TDM slot - 4 slots for AK4619
  // For AK4619 in TDM mode:
  // Slot 0: DAC1/ADC1 Left channel
  // Slot 1: DAC1/ADC1 Right channel
  // Slot 2: DAC2/ADC2 Left channel
  // Slot 3: DAC2/ADC2 Right channel
  i2s_tdm_slot_config_t slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO,
                                                                       I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 |
                                                                       I2S_TDM_SLOT3);

  // Configure GPIO pins
  i2s_tdm_gpio_config_t gpio_cfg = {
    .mclk = I2S_MCLK,
    .bclk = I2S_BCLK,
    .ws = I2S_WS,
    .dout = I2S_DOUT,
    .din = I2S_DIN,
    .invert_flags = {
      .mclk_inv = false,
      .bclk_inv = false,
      .ws_inv = false
    }
  };

  // Combine into TDM config
  i2s_tdm_config_t tdm_cfg = {
    .clk_cfg = clk_cfg,
    .slot_cfg = slot_cfg,
    .gpio_cfg = gpio_cfg
  };

  // Initialize TX channel in TDM mode
  ret = i2s_channel_init_tdm_mode(tx_chan, &tdm_cfg);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S TX channel init TDM mode failed: %s", esp_err_to_name(ret));
    i2s_del_channel(tx_chan);
    i2s_del_channel(rx_chan);
    return ret;
  }

  // Initialize RX channel in TDM mode
  ret = i2s_channel_init_tdm_mode(rx_chan, &tdm_cfg);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S RX channel init TDM mode failed: %s", esp_err_to_name(ret));
    i2s_del_channel(tx_chan);
    i2s_del_channel(rx_chan);
    return ret;
  }

  // Tune APLL to correct the slight sample rate offset
  ak4619_tune_apll();

  // Enable the channels
  ret = i2s_channel_enable(tx_chan);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S TX channel enable failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = i2s_channel_enable(rx_chan);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S RX channel enable failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "I2S configured for AK4619 in TDM mode (4 slots, 32-bit, 48kHz, 384fs MCLK)");

  return ESP_OK;
}


// Write a register to AK4619
static esp_err_t ak_write(uint8_t reg, uint8_t val)
{
  esp_err_t ret;
  uint8_t data[2] = {reg, val};

  ret = i2c_master_transmit(ak4619_dev_handle, data, sizeof(data), -1);

  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C write failed: reg=0x%02X, val=0x%02X, error=%s",
             reg, val, esp_err_to_name(ret));
  }
  else
  {
    ESP_LOGD(TAG, "I2C write OK: reg=0x%02X, val=0x%02X", reg, val);
  }

  return ret;
}

// Read a register from AK4619 (Random Address Read)
static esp_err_t ak_read(uint8_t reg, uint8_t* val)
{
  esp_err_t ret;

  ret = i2c_master_transmit_receive(ak4619_dev_handle, &reg, 1, val, 1, -1);

  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C read failed: reg=0x%02X, error=%s",
             reg, esp_err_to_name(ret));
  }
  else
  {
    ESP_LOGD(TAG, "I2C read OK: reg=0x%02X, val=0x%02X", reg, *val);
  }

  return ret;
}

esp_err_t ak4619_tdm_init(void)
{
  esp_err_t ret;

  ESP_LOGI(TAG, "========== AK4619 TDM Initialization ==========");
  ESP_LOGI(TAG, "I2C Configuration:");
  ESP_LOGI(TAG, "  Port: I2C_%d", I2C_PORT_NUM);
  ESP_LOGI(TAG, "  SDA:  GPIO%d", I2C_SDA);
  ESP_LOGI(TAG, "  SCL:  GPIO%d", I2C_SCL);
  ESP_LOGI(TAG, "  RESET: GPIO%d", AK4619_RESET_PIN);
  ESP_LOGI(TAG, "  I2C Address: 0x%02X", AK4619_ADDR);
  ESP_LOGI(TAG, "  Speed: %d Hz", I2C_CLK_SPEED);
  ESP_LOGI(TAG, "  AK4619 Address: 0x%02X (7-bit)", AK4619_ADDR);

  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << AK4619_RESET_PIN);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  // Reset AK4619 if reset pin is defined
  gpio_set_level(AK4619_RESET_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(AK4619_RESET_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(100));

  // I2C init
  i2c_master_bus_config_t i2c_mst_config = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_PORT_NUM,
    .scl_io_num = I2C_SCL,
    .sda_io_num = I2C_SDA,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus_handle;
  ret = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C new master bus failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "I2C driver initialized successfully");

  vTaskDelay(pdMS_TO_TICKS(50)); // Give codec time to reset and stabilize

  // Scan I2C bus
  ESP_LOGI(TAG, "Scanning I2C bus...");
  int devices_found = 0;
  for (int addr = 0x08; addr < 0x78; addr++)
  {
    if (i2c_master_probe(bus_handle, addr, 50) == ESP_OK)
    {
      ESP_LOGI(TAG, "  Device found at address: 0x%02X", addr);
      devices_found++;
    }
  }

  if (devices_found == 0)
  {
    ESP_LOGW(TAG, "No I2C devices found on bus!");
    ESP_LOGW(TAG, "Check:");
    ESP_LOGW(TAG, "  1. Power supply to codec (TVDD, AVDD)");
    ESP_LOGW(TAG, "  2. I2C pull-up resistors on SDA/SCL");
    ESP_LOGW(TAG, "  3. I2C pin connections");
    ESP_LOGW(TAG, "  4. Codec power-down pin (PDN) if present");
  }

  // Probe for AK4619 specifically
  if (i2c_master_probe(bus_handle, AK4619_ADDR, 100) != ESP_OK)
  {
    ESP_LOGE(TAG, "AK4619 probe failed - device not responding");
    return ESP_ERR_NOT_FOUND;
  }

  i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = AK4619_ADDR,
    .scl_speed_hz = I2C_CLK_SPEED,
  };

  ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &ak4619_dev_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C adding device failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Start register configuration
  ESP_LOGI(TAG, "Configuring AK4619 registers...");

  // IM// --- Power up ADC & DAC ---
  // PDN pin assumed high -> power down
  //ESP_ERROR_CHECK(ak_write(REG_POWER, 0x00)); // Disable analog & digital blocks
  // read back and echo
  uint8_t power_reg;
  ESP_ERROR_CHECK(ak_read(REG_POWER, &power_reg));
  ESP_LOGI(TAG, "Power Register after power down: 0x%02X", power_reg);

  // --- Audio Interface Format for TDM256, I2S compatible, 32-bit slots ---
  // TDM=1, DCF=010 (I2S), DSL=11 (32-bit), BCKP=0, SDOPH=0
  ESP_ERROR_CHECK(ak_write(REG_AUDIO_IF0, 0xAC));
  // read back and echo
  uint8_t audio_if0_reg;
  ESP_ERROR_CHECK(ak_read(REG_AUDIO_IF0, &audio_if0_reg));
  ESP_LOGI(TAG, "Audio IF0 Register after config: 0x%02X", audio_if0_reg);

  // --- Audio Interface Format 2 ---
  // SLOT=1 (slot length basis), DIDL=11 (32-bit input), DODL=00 (24-bit output)
  ESP_ERROR_CHECK(ak_write(REG_AUDIO_IF1, 0b11100));
  // read back and echo
  uint8_t audio_if1_reg;
  ESP_ERROR_CHECK(ak_read(REG_AUDIO_IF1, &audio_if1_reg));
  ESP_LOGI(TAG, "Audio IF1 Register after config: 0x%02X", audio_if1_reg);

  // --- System Clock for 48kHz with MCLK=384*Fs (bit settings FS[2:0]=010) ---
  ESP_ERROR_CHECK(ak_write(REG_SYSCLK, 0b010));
  // read back and echo
  uint8_t sysclk_reg;
  ESP_ERROR_CHECK(ak_read(REG_SYSCLK, &sysclk_reg));
  ESP_LOGI(TAG, "SysClk Register after config: 0x%02X", sysclk_reg);

  // adc input select to single ended
  ESP_ERROR_CHECK(ak_write(REG_ADC_IN_SEL, 0b01010101));
  // read back and echo
  uint8_t adc_in_sel_reg;
  ESP_ERROR_CHECK(ak_read(REG_ADC_IN_SEL, &adc_in_sel_reg));
  ESP_LOGI(TAG, "ADC Input Select Register after config: 0x%02X", adc_in_sel_reg);
  /*
      // --- Optional: set ADC/LR volume to neutral 0dB ---
      ESP_ERROR_CHECK(ak_write(REG_ADC_VOL0, 0x30));
      ESP_ERROR_CHECK(ak_write(REG_ADC_VOL1, 0x30));
      ESP_ERROR_CHECK(ak_write(REG_ADC_VOL2, 0x30));
      ESP_ERROR_CHECK(ak_write(REG_ADC_VOL3, 0x30));

      // --- Optional: neutral DAC volume ---
      ESP_ERROR_CHECK(ak_write(REG_DAC_VOL0, 0x18));
      ESP_ERROR_CHECK(ak_write(REG_DAC_VOL1, 0x18));
      ESP_ERROR_CHECK(ak_write(REG_DAC_VOL2, 0x18));
      ESP_ERROR_CHECK(ak_write(REG_DAC_VOL3, 0x18));
      */


  // PDN pin assumed high -> power down
  ESP_ERROR_CHECK(ak_write(REG_POWER, 0b110111)); // Enable analog & digital blocks, release reset state
  // read back and echo
  ESP_ERROR_CHECK(ak_read(REG_POWER, &power_reg));
  ESP_LOGI(TAG, "Power Register after config: 0x%02X", power_reg);


  // setup i2s for ak4619 in tdm mode
  ret = ak4619_setup_i2s();
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to setup I2S for AK4619: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "========== AK4619 TDM Initialization Complete ==========");
  return ESP_OK;
}

// Public API for reading AK4619 registers
esp_err_t ak4619_read_register(uint8_t reg, uint8_t* val)
{
  return ak_read(reg, val);
}

// Public API for writing AK4619 registers
esp_err_t ak4619_write_register(uint8_t reg, uint8_t val)
{
  return ak_write(reg, val);
}

// I2S read wrapper for compatibility with main audio task
void ak4619_i2s_read(void* buf, uint32_t size, uint32_t* bytes_read)
{
  size_t bytes_read_local = 0;
  esp_err_t ret = i2s_channel_read(rx_chan, buf, size, &bytes_read_local, portMAX_DELAY);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
    *bytes_read = 0;
    return;
  }
  *bytes_read = bytes_read_local;
}

// I2S write wrapper for compatibility with main audio task
void ak4619_i2s_write(void* buf, uint32_t size, uint32_t* bytes_written)
{
  size_t bytes_written_local = 0;
  esp_err_t ret = i2s_channel_write(tx_chan, buf, size, &bytes_written_local, portMAX_DELAY);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    *bytes_written = 0;
    return;
  }
  *bytes_written = bytes_written_local;
}

// ============================================================================
// DAC Volume Control Functions
// ============================================================================

/**
 * @brief Convert dB value to AK4619 DAC volume register value
 *
 * @param db_value Volume in dB (-115.0 to +12.0, in 0.5dB steps)
 * @return uint8_t Register value (0x00 to 0xFF)
 *         0x00 = +12.0dB (max gain)
 *         0x18 = 0.0dB (unity gain, default)
 *         0xFE = -115.0dB (minimum)
 *         0xFF = Mute
 */
uint8_t ak4619_db_to_volume(float db_value)
{
  // Clamp to valid range
  if (db_value > 12.0f)
  {
    db_value = 12.0f;
  }
  else if (db_value < -115.0f)
  {
    return 0xFF; // Mute
  }

  // Convert dB to register value
  // Formula: reg_value = (12.0 - db_value) / 0.5
  // 0x00 = +12.0dB, 0x18 = 0dB, 0xFE = -115.0dB
  int reg_value = (int)((12.0f - db_value) / 0.5f + 0.5f); // +0.5 for rounding

  // Clamp to valid register range (0x00 to 0xFE, 0xFF is mute)
  if (reg_value < 0)
  {
    reg_value = 0;
  }
  else if (reg_value > 0xFE)
  {
    reg_value = 0xFE;
  }

  return (uint8_t)reg_value;
}

/**
 * @brief Set DAC volume for a specific DAC pair
 *
 * @param dac_num DAC number (1 or 2)
 * @param left_vol Left channel volume (0x00 to 0xFF)
 * @param right_vol Right channel volume (0x00 to 0xFF)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ak4619_set_dac_volume(uint8_t dac_num, uint8_t left_vol, uint8_t right_vol)
{
  esp_err_t ret;
  uint8_t left_reg, right_reg;

  if (dac_num == 1)
  {
    left_reg = REG_DAC_VOL0; // DAC1 Left (0x0E)
    right_reg = REG_DAC_VOL1; // DAC1 Right (0x0F)
  }
  else if (dac_num == 2)
  {
    left_reg = REG_DAC_VOL2; // DAC2 Left (0x10)
    right_reg = REG_DAC_VOL3; // DAC2 Right (0x11)
  }
  else
  {
    ESP_LOGE(TAG, "Invalid DAC number: %d (must be 1 or 2)", dac_num);
    return ESP_ERR_INVALID_ARG;
  }

  // Write left channel volume
  ret = ak_write(left_reg, left_vol);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to set DAC%d left volume", dac_num);
    return ret;
  }

  // Write right channel volume
  ret = ak_write(right_reg, right_vol);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to set DAC%d right volume", dac_num);
    return ret;
  }

  // Convert register values back to dB for logging
  float left_db = 12.0f - (left_vol * 0.5f);
  float right_db = 12.0f - (right_vol * 0.5f);

  if (left_vol == 0xFF)
  {
    ESP_LOGI(TAG, "DAC%d Left: Muted", dac_num);
  }
  else
  {
    ESP_LOGI(TAG, "DAC%d Left: %.1f dB (0x%02X)", dac_num, left_db, left_vol);
  }

  if (right_vol == 0xFF)
  {
    ESP_LOGI(TAG, "DAC%d Right: Muted", dac_num);
  }
  else
  {
    ESP_LOGI(TAG, "DAC%d Right: %.1f dB (0x%02X)", dac_num, right_db, right_vol);
  }

  return ESP_OK;
}

/**
 * @brief Set DAC1 volume (convenience function)
 *
 * @param left_vol Left channel volume (0x00 to 0xFF)
 * @param right_vol Right channel volume (0x00 to 0xFF)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ak4619_set_dac1_volume(uint8_t left_vol, uint8_t right_vol)
{
  return ak4619_set_dac_volume(1, left_vol, right_vol);
}

/**
 * @brief Set DAC2 volume (convenience function)
 *
 * @param left_vol Left channel volume (0x00 to 0xFF)
 * @param right_vol Right channel volume (0x00 to 0xFF)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ak4619_set_dac2_volume(uint8_t left_vol, uint8_t right_vol)
{
  return ak4619_set_dac_volume(2, left_vol, right_vol);
}

/**
 * @brief Set all DAC channels to the same volume
 *
 * @param volume Volume value (0x00 to 0xFF) applied to all 4 channels
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ak4619_set_all_dac_volume(uint8_t volume)
{
  esp_err_t ret;

  ret = ak4619_set_dac_volume(1, volume, volume);
  if (ret != ESP_OK)
  {
    return ret;
  }

  ret = ak4619_set_dac_volume(2, volume, volume);
  if (ret != ESP_OK)
  {
    return ret;
  }

  ESP_LOGI(TAG, "All DAC channels set to same volume");
  return ESP_OK;
}



