/***************
CTAG TBD >>to be determined<< is an open source eurorack synthesizer module.

A project conceived within the Creative Technologies Arbeitsgruppe of
Kiel University of Applied Sciences: https://www.creative-technologies.de

(c) 2025 by Robert Manzke. All rights reserved.

The CTAG TBD software is licensed under the GNU General Public License
(GPL 3.0), available here: https://www.gnu.org/licenses/gpl-3.0.txt

The CTAG TBD hardware design is released under the Creative Commons
Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0).
Details here: https://creativecommons.org/licenses/by-nc-sa/4.0/

CTAG TBD is provided "as is" without any express or implied warranties.

License and copyright details for specific submodules are included in their
respective component folders / files if different from this license.
***************/

#include "codec.h"

#include <driver/i2s_std.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"


static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

static const char* TAG = "CODEC";

#define I2C_PORT_NUM I2C_NUM_1 // 0 is used for OLED
#define I2C_SDA GPIO_NUM_7
#define I2C_SCL GPIO_NUM_8
#define I2C_CLK_SPEED 400000

// dysfunctional IOs
// GPIO45, GPIO54, GPIO6,
// working GPIOs
// GPIO23, GPIO22
#define I2S_PORT_NUM I2S_NUM_0
#define I2S_MCLK GPIO_NUM_53 // former GPIO45
#define I2S_BCLK GPIO_NUM_46
#define I2S_WS GPIO_NUM_47
#define I2S_DOUT GPIO_NUM_48
#define I2S_DIN GPIO_NUM_23 // former GPIO54

uint8_t page = 255;

#define AIC3254_ADDR 0x18 // 0b0011000 (7-bit address)
#define ACK_CHECK_EN 1
/* tlv320aic32x4 register space (in decimal to match datasheet) */
#define AIC32X4_REG(page, reg)    ((page * 128) + reg)
#define    AIC32X4_PSEL        AIC32X4_REG(0, 0)
#define    AIC32X4_RESET        AIC32X4_REG(0, 1)
#define    AIC32X4_CLKMUX        AIC32X4_REG(0, 4)
#define    AIC32X4_PLLPR        AIC32X4_REG(0, 5)
#define    AIC32X4_PLLJ        AIC32X4_REG(0, 6)
#define    AIC32X4_PLLDMSB        AIC32X4_REG(0, 7)
#define    AIC32X4_PLLDLSB        AIC32X4_REG(0, 8)
#define    AIC32X4_NDAC        AIC32X4_REG(0, 11)
#define    AIC32X4_MDAC        AIC32X4_REG(0, 12)
#define AIC32X4_DOSRMSB        AIC32X4_REG(0, 13)
#define AIC32X4_DOSRLSB        AIC32X4_REG(0, 14)
#define    AIC32X4_NADC        AIC32X4_REG(0, 18)
#define    AIC32X4_MADC        AIC32X4_REG(0, 19)
#define AIC32X4_AOSR        AIC32X4_REG(0, 20)
#define AIC32X4_CLKMUX2        AIC32X4_REG(0, 25)
#define AIC32X4_CLKOUTM        AIC32X4_REG(0, 26)
#define AIC32X4_IFACE1        AIC32X4_REG(0, 27)
#define AIC32X4_IFACE2        AIC32X4_REG(0, 28)
#define AIC32X4_IFACE3        AIC32X4_REG(0, 29)
#define AIC32X4_BCLKN        AIC32X4_REG(0, 30)
#define AIC32X4_IFACE4        AIC32X4_REG(0, 31)
#define AIC32X4_IFACE5        AIC32X4_REG(0, 32)
#define AIC32X4_IFACE6        AIC32X4_REG(0, 33)
#define AIC32X4_GPIOCTL        AIC32X4_REG(0, 52)
#define AIC32X4_DOUTCTL        AIC32X4_REG(0, 53)
#define AIC32X4_DINCTL        AIC32X4_REG(0, 54)
#define AIC32X4_MISOCTL        AIC32X4_REG(0, 55)
#define AIC32X4_SCLKCTL        AIC32X4_REG(0, 56)
#define AIC32X4_DACSPB        AIC32X4_REG(0, 60)
#define AIC32X4_ADCSPB        AIC32X4_REG(0, 61)
#define AIC32X4_DACSETUP    AIC32X4_REG(0, 63)
#define AIC32X4_DACMUTE        AIC32X4_REG(0, 64)
#define AIC32X4_LDACVOL        AIC32X4_REG(0, 65)
#define AIC32X4_RDACVOL        AIC32X4_REG(0, 66)
#define AIC3254_BEEPCTL_L        AIC32X4_REG(0, 71)
#define AIC3254_BEEPCTL_R        AIC32X4_REG(0, 72)
#define AIC32X4_ADCSETUP    AIC32X4_REG(0, 81)
#define    AIC32X4_ADCFGA        AIC32X4_REG(0, 82)
#define AIC32X4_LADCVOL        AIC32X4_REG(0, 83)
#define AIC32X4_RADCVOL        AIC32X4_REG(0, 84)
#define AIC32X4_LAGC1        AIC32X4_REG(0, 86)
#define AIC32X4_LAGC2        AIC32X4_REG(0, 87)
#define AIC32X4_LAGC3        AIC32X4_REG(0, 88)
#define AIC32X4_LAGC4        AIC32X4_REG(0, 89)
#define AIC32X4_LAGC5        AIC32X4_REG(0, 90)
#define AIC32X4_LAGC6        AIC32X4_REG(0, 91)
#define AIC32X4_LAGC7        AIC32X4_REG(0, 92)
#define AIC32X4_RAGC1        AIC32X4_REG(0, 94)
#define AIC32X4_RAGC2        AIC32X4_REG(0, 95)
#define AIC32X4_RAGC3        AIC32X4_REG(0, 96)
#define AIC32X4_RAGC4        AIC32X4_REG(0, 97)
#define AIC32X4_RAGC5        AIC32X4_REG(0, 98)
#define AIC32X4_RAGC6        AIC32X4_REG(0, 99)
#define AIC32X4_RAGC7        AIC32X4_REG(0, 100)
#define AIC32X4_PWRCFG        AIC32X4_REG(1, 1)
#define AIC32X4_LDOCTL        AIC32X4_REG(1, 2)
#define AIC32X4_LPLAYBACK    AIC32X4_REG(1, 3)
#define AIC32X4_RPLAYBACK    AIC32X4_REG(1, 4)
#define AIC32X4_OUTPWRCTL    AIC32X4_REG(1, 9)
#define AIC32X4_CMMODE        AIC32X4_REG(1, 10)
#define AIC32X4_HPLROUTE    AIC32X4_REG(1, 12)
#define AIC32X4_HPRROUTE    AIC32X4_REG(1, 13)
#define AIC32X4_LOLROUTE    AIC32X4_REG(1, 14)
#define AIC32X4_LORROUTE    AIC32X4_REG(1, 15)
#define    AIC32X4_HPLGAIN        AIC32X4_REG(1, 16)
#define    AIC32X4_HPRGAIN        AIC32X4_REG(1, 17)
#define    AIC32X4_LOLGAIN        AIC32X4_REG(1, 18)
#define    AIC32X4_LORGAIN        AIC32X4_REG(1, 19)
#define AIC32X4_HEADSTART    AIC32X4_REG(1, 20)
#define AIC32X4_MICBIAS        AIC32X4_REG(1, 51)
#define AIC32X4_LMICPGAPIN    AIC32X4_REG(1, 52)
#define AIC32X4_LMICPGANIN    AIC32X4_REG(1, 54)
#define AIC32X4_RMICPGAPIN    AIC32X4_REG(1, 55)
#define AIC32X4_RMICPGANIN    AIC32X4_REG(1, 57)
#define AIC32X4_FLOATINGINPUT    AIC32X4_REG(1, 58)
#define AIC32X4_LMICPGAVOL    AIC32X4_REG(1, 59)
#define AIC32X4_RMICPGAVOL    AIC32X4_REG(1, 60)
#define AIC32X4_REFPOWERUP    AIC32X4_REG(1, 123)
#define AIC32X4_DACPRB        AIC32X4_REG(0, 60)
#define AIC32X4_ADCPRB        AIC32X4_REG(0, 61)
#define AIC32X4_HPF_COEFF_N0_MSB  AIC32X4_REG(8, 1)
#define AIC32X4_HPF_COEFF_N0_LSB  AIC32X4_REG(8, 2)
#define AIC32X4_HPF_COEFF_N1_MSB  AIC32X4_REG(8, 3)
#define AIC32X4_HPF_COEFF_N1_LSB  AIC32X4_REG(8, 4)
#define AIC32X4_HPF_COEFF_D1_MSB  AIC32X4_REG(8, 5)
#define AIC32X4_HPF_COEFF_D1_LSB  AIC32X4_REG(8, 6)

static void cfg_i2c()
{
  ESP_LOGI(TAG, "cfg codec i2c");
  esp_err_t err = ESP_OK;
  i2c_config_t conf = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = I2C_SDA,
    .scl_io_num = I2C_SCL,
    .sda_pullup_en = false,
    .scl_pullup_en = false,
    .master = {
      .clk_speed = I2C_CLK_SPEED,
    },
    .clk_flags = 0,
  };

  err |= i2c_param_config(I2C_PORT_NUM, &conf);
  err |= i2c_driver_install(I2C_PORT_NUM, conf.mode, 0, 0, ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_SHARED);
}

static void write_reg(uint8_t reg_add, uint8_t data)
{
  esp_err_t err = ESP_OK;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  err |= i2c_master_start(cmd);
  ESP_ERROR_CHECK(err); // send start bit
  err |= i2c_master_write_byte(cmd, (AIC3254_ADDR << 1) | I2C_MASTER_WRITE,
                               ACK_CHECK_EN); // aic3254 7-bit address + write bit
  ESP_ERROR_CHECK(err);
  err |= i2c_master_write_byte(cmd, reg_add, ACK_CHECK_EN); // target register
  ESP_ERROR_CHECK(err);
  err |= i2c_master_write_byte(cmd, data, ACK_CHECK_EN); // target value
  ESP_ERROR_CHECK(err);
  err |= i2c_master_stop(cmd); // send stop bit
  ESP_ERROR_CHECK(err);
  err |= i2c_master_cmd_begin((i2c_port_t)I2C_PORT_NUM, cmd, 1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);
  ESP_ERROR_CHECK(err);
}

static uint8_t read_reg(uint8_t reg_add)
{
  uint8_t data = 0xFF; // dummy
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd); // send start bit
  i2c_master_write_byte(cmd, (AIC3254_ADDR << 1) | I2C_MASTER_WRITE,
                        ACK_CHECK_EN); // aic3254 7-bit address + write bit
  i2c_master_write_byte(cmd, reg_add, ACK_CHECK_EN); // register to be read
  i2c_master_start(
    cmd); // resend start bit (see application reference guide p.80 for more info on the i2c transaction)
  i2c_master_write_byte(cmd, (AIC3254_ADDR << 1) | I2C_MASTER_READ,
                        ACK_CHECK_EN); // aic3254 7-bit address + read bit
  i2c_master_read_byte(cmd, &data, I2C_MASTER_NACK); // read into data buffer
  i2c_master_stop(cmd); // send stop bit
  i2c_master_cmd_begin((i2c_port_t)I2C_PORT_NUM, cmd, 1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);
  return data;
}

static esp_err_t test_reg(uint8_t register_address, uint8_t expected_value)
{
  uint8_t read_value = read_reg(register_address);

  if (read_value == expected_value)
  {
    //printf("[PASS] R_0x%X -> EXPECTED: 0x%X. ACTUAL: 0x%X.\n", register_address, expected_value, read_value);
    return ESP_OK;
  }
  else
  {
    //printf("[ERROR] R_0x%X -> EXPECTED: 0x%X. ACTUAL: 0x%X.\n", register_address, expected_value, read_value);
    return ESP_ERR_INVALID_STATE;
  }
}

static void write_AIC32X4_reg(uint8_t reg_add, uint8_t data)
{
  if ((reg_add >> 7) != page)
  {
    page = reg_add >> 7;
    write_reg(AIC32X4_PSEL, page);
    //        ESP_LOGE("AIC3254", "WRITE: Switched to page %d", page);
  }
  uint8_t reg_add1 = reg_add & 0x7F;
  write_reg(reg_add1, data);
  //    uint8_t val = read_16bit_reg(reg_add);
  //    ESP_LOGE("AIC3254", "addr: 0x%02X val: 0x%02X,0x%02X", reg_add, data, val);
}


static void identify()
{
  write_AIC32X4_reg(AIC32X4_PSEL, 0);
  esp_err_t err = test_reg(0, 0);
  if (err == ESP_OK)
  {
    ESP_LOGI(TAG, "AIC3254 found");
    write_AIC32X4_reg(AIC32X4_LOLGAIN, 0b1000000); // mute lout drivers
    write_AIC32X4_reg(AIC32X4_LORGAIN, 0b1000000); // mute lout drivers
  }
  else
  {
    ESP_LOGE(TAG, "AIC3254 not found");
  }
}

// from pg. 26 of https://www.ti.com/lit/an/slaa408a/slaa408a.pdf?ts=1766827966822&ref_url=https%253A%252F%252Fwww.ti.com%252Fproduct%252FTLV320AIC3254
// check this https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/6/Coefficients.png
// and this https://e2e.ti.com/support/audio-group/audio/f/audio-forum/669437/tlv320aic3204-first-order-iir-filter-coefficients-for-adc as a reference
static void ADCHighPassEnable()
{
  // Power down ADCs before changing coefficients
  write_AIC32X4_reg(AIC32X4_ADCSETUP, 0b00000000);

  // Configure ADC to use PRB_R1 which includes IIR filter
  write_AIC32X4_reg(AIC32X4_ADCPRB, 0x01);

  // DC blocking filter (first-order HPF) at fc ≈ 3.7Hz, fs = 44100Hz
  // Transfer function: H(z) = (1 - z^-1) / (1 - α·z^-1)
  // α = exp(-2π·fc/fs) ≈ 0.999472
  // N0 = +1.0 * 2^23 = 0x7FFFFF (8388607)
  // N1 = -1.0 * 2^23 = 0x800001 (-8388607 in two's complement, 24-bit)
  // D1 = α * 2^23 ≈ 0.999472 * 8388608 ≈ 0x7FB0FE (8384190)

  // Switch to page 8 for left ADC channel coefficients
  write_reg(AIC32X4_PSEL, 8);
  page = 8;

  // Left channel: C4 (N0) at Page 8, Reg 24,25,26
  write_reg(24, 0x7F); // N0 MSB
  write_reg(25, 0xFF); // N0 MID
  write_reg(26, 0xFF); // N0 LSB (0x7FFFFF = +8388607)

  // Left channel: C5 (N1) at Page 8, Reg 28,29,30
  write_reg(28, 0x80); // N1 MSB
  write_reg(29, 0x00); // N1 MID
  write_reg(30, 0x01); // N1 LSB (0x800001 = -8388607)

  // Left channel: C6 (D1) at Page 8, Reg 32,33,34
  write_reg(32, 0x7F); // D1 MSB
  write_reg(33, 0xB0); // D1 MID
  write_reg(34, 0xFE); // D1 LSB (0x7FB0FE ≈ 8384190)

  // Switch to page 9 for right ADC channel coefficients
  write_reg(AIC32X4_PSEL, 9);
  page = 9;

  // Right channel: C36 (N0) at Page 9, Reg 32,33,34
  write_reg(32, 0x7F); // N0 MSB
  write_reg(33, 0xFF); // N0 MID
  write_reg(34, 0xFF); // N0 LSB

  // Right channel: C37 (N1) at Page 9, Reg 36,37,38
  write_reg(36, 0x80); // N1 MSB
  write_reg(37, 0x00); // N1 MID
  write_reg(38, 0x01); // N1 LSB

  // Right channel: C38 (D1) at Page 9, Reg 40,41,42
  write_reg(40, 0x7F); // D1 MSB
  write_reg(41, 0xB0); // D1 MID
  write_reg(42, 0xFE); // D1 LSB

  // Switch back to page 0
  write_reg(AIC32X4_PSEL, 0);
  page = 0;

  // Power up ADCs
  write_AIC32X4_reg(AIC32X4_ADCSETUP, 0b11000000);

  ESP_LOGI(TAG, "High-pass IIR filter enabled on ADC path (3.7Hz @ 44.1kHz)");
}


static void ADCHighPassDisable()
{
  // power down l+r adcs
  write_AIC32X4_reg(AIC32X4_ADCSETUP, 0b00000000); // (P0_R81) power down left and right ADCs

  // Configure ADC to use PRB_R1 (same as HPF enabled)
  write_AIC32X4_reg(AIC32X4_ADCPRB, 0x01);

  // Switch to page 8 for left ADC channel coefficients
  write_reg(AIC32X4_PSEL, 8);
  page = 8;

  // Left channel: C4 (N0) at Page 8, Reg 24,25,26 - Default from Table 5-4: 0x7FFFFF00
  write_reg(24, 0x7F); // N0 MSB
  write_reg(25, 0xFF); // N0 MID
  write_reg(26, 0xFF); // N0 LSB - Changed to 0xFF (closer to unity gain in Q23)

  // Left channel: C5 (N1) at Page 8, Reg 28,29,30 - Zero
  write_reg(28, 0x00); // N1 MSB
  write_reg(29, 0x00); // N1 MID
  write_reg(30, 0x00); // N1 LSB

  // Left channel: C6 (D1) at Page 8, Reg 32,33,34 - Zero
  write_reg(32, 0x00); // D1 MSB
  write_reg(33, 0x00); // D1 MID
  write_reg(34, 0x00); // D1 LSB

  // Switch to page 9 for right ADC channel coefficients
  write_reg(AIC32X4_PSEL, 9);
  page = 9;

  // Right channel: C36 (N0) at Page 9, Reg 32,33,34 - Default: 0x7FFFFF00
  write_reg(32, 0x7F); // N0 MSB
  write_reg(33, 0xFF); // N0 MID
  write_reg(34, 0xFF); // N0 LSB

  // Right channel: C37 (N1) at Page 9, Reg 36,37,38 - Zero
  write_reg(36, 0x00); // N1 MSB
  write_reg(37, 0x00); // N1 MID
  write_reg(38, 0x00); // N1 LSB

  // Right channel: C38 (D1) at Page 9, Reg 40,41,42 - Zero
  write_reg(40, 0x00); // D1 MSB
  write_reg(41, 0x00); // D1 MID
  write_reg(42, 0x00); // D1 LSB

  // Switch back to page 0
  write_reg(AIC32X4_PSEL, 0);
  page = 0;

  write_AIC32X4_reg(AIC32X4_ADCSETUP, 0b11000000); // (P0_R81) power up left and right ADCs

  ESP_LOGI(TAG, "High-pass filter disabled on ADC path (all-pass/bypass mode)");
}

static void cfg_codec(const bool use_pll)
{
  ESP_LOGI(TAG, "AIC3254 configuration");

  // Step 1: Define starting point - Set register page to 0
  write_reg(AIC32X4_PSEL, 0);
  page = 0;

  // Step 2: Initiate SW Reset
  write_AIC32X4_reg(AIC32X4_RESET, 0x01);
  vTaskDelay(10 / portTICK_PERIOD_MS);

  if (use_pll)
  {
    // Step 3: Program Clock Settings
    // PLL_CLKIN = MCLK, CODEC_CLKIN = PLL_CLK
    write_AIC32X4_reg(AIC32X4_CLKMUX, 0b0011);

    // Step 4: Program PLL clock dividers (P=1, R=1, J=4, D=0)
    write_AIC32X4_reg(AIC32X4_PLLPR, 0x11); // PLL disabled, P=1, R=1
    write_AIC32X4_reg(AIC32X4_PLLJ, 0x04); // J = 4
    write_AIC32X4_reg(AIC32X4_PLLDMSB, 0x00); // D[13:8] = 0
    write_AIC32X4_reg(AIC32X4_PLLDLSB, 0x00); // D[7:0] = 0

    // Step 5: Power up PLL
    write_AIC32X4_reg(AIC32X4_PLLPR, 0x91); // Power up PLL, P=1, R=1
    vTaskDelay(10 / portTICK_PERIOD_MS); // Wait for PLL lock
  }

  // Step 3: Program Clock Settings default
  // CODEC_CLKIN = MCLK

  // Step 4: Program PLL clock dividers default

  // Step 6: Program and power up NDAC
  write_AIC32X4_reg(AIC32X4_NDAC, 0x81); // Power up NDAC = 1

  // Step 7: Program and power up MDAC
  write_AIC32X4_reg(AIC32X4_MDAC, 0x82); // Power up MDAC = 2

  // Step 8: Program OSR value
  write_AIC32X4_reg(AIC32X4_DOSRMSB, 0x00);
  write_AIC32X4_reg(AIC32X4_DOSRLSB, 0x80); // DOSR = 128

  // Step 9: Program I2S word length (16-bit)
  write_AIC32X4_reg(AIC32X4_IFACE1, 0b00000000);
  write_AIC32X4_reg(AIC32X4_IFACE2, 0x00);

  // Step 10: Program processing block (using defaults PRB_P1 and PRB_R1)
  // Default values are already suitable

  // Step 11: Program Analog Blocks - Set register page to 1
  write_reg(AIC32X4_PSEL, 1);
  page = 1;

  // Step 12: Disable coarse AVDD generation
  write_AIC32X4_reg(AIC32X4_PWRCFG, 0b00001000);

  // Step 13: Enable Master Analog Power Control
  write_AIC32X4_reg(AIC32X4_LDOCTL, 0x01);

  // Step 14: Program Common Mode voltage
  write_AIC32X4_reg(AIC32X4_CMMODE, 0x08); // Output CM = 1.65V (LDOIN/2)

  // Step 15: Program PowerTune (using defaults)
  // Step 16: Program Reference fast charging (using defaults)
  // Step 17: Program Headphone depop settings (using defaults)

  // Step 18: Program routing of DAC output to output amplifiers
  write_AIC32X4_reg(AIC32X4_LOLROUTE, 0x08); // Left DAC to LOL
  write_AIC32X4_reg(AIC32X4_LORROUTE, 0x08); // Right DAC to LOR
  write_AIC32X4_reg(AIC32X4_HPLROUTE, 0x08); // Left DAC to HPL
  write_AIC32X4_reg(AIC32X4_HPRROUTE, 0x08); // Right DAC to HPR

  // Step 19: Unmute and set gain of output drivers
  write_AIC32X4_reg(AIC32X4_HPLGAIN, 0x00); // Unmute HPL, 0dB gain
  write_AIC32X4_reg(AIC32X4_HPRGAIN, 0x00); // Unmute HPR, 0dB gain
  write_AIC32X4_reg(AIC32X4_LOLGAIN, 0x06); // Unmute LOL, 6dB gain
  write_AIC32X4_reg(AIC32X4_LORGAIN, 0x06); // Unmute LOR, 6dB gain

  // Step 20: Power up output drivers
  write_AIC32X4_reg(AIC32X4_OUTPWRCTL, 0b00111100); // Power up HPL, HPR, LOL, LOR

  // Step 21: Apply waiting time (for depop and soft-stepping)
  vTaskDelay(10 / portTICK_PERIOD_MS);

  // Step 22: Power Up DAC - Set register page to 0
  write_reg(AIC32X4_PSEL, 0);
  page = 0;

  // Step 23: Power up DAC channels
  write_AIC32X4_reg(AIC32X4_DACSETUP, 0b11010100);

  // Step 24: Unmute digital volume control
  write_AIC32X4_reg(AIC32X4_DACMUTE, 0x00);
  write_AIC32X4_reg(AIC32X4_LDACVOL, 0x00); // Left DAC 0dB
  write_AIC32X4_reg(AIC32X4_RDACVOL, 0x00); // Right DAC 0dB

  // ADC Configuration (similar sequence for recording path)
  write_AIC32X4_reg(AIC32X4_NADC, 0x81); // Power up NADC = 1
  write_AIC32X4_reg(AIC32X4_MADC, 0x82); // Power up MADC = 2
  write_AIC32X4_reg(AIC32X4_AOSR, 0x80); // AOSR = 128

  // ADC routing
  write_reg(AIC32X4_PSEL, 1);
  page = 1;
  write_AIC32X4_reg(AIC32X4_LMICPGAPIN, 0b01000000);
  write_AIC32X4_reg(AIC32X4_RMICPGAPIN, 0b01000000);
  write_AIC32X4_reg(AIC32X4_LMICPGANIN, 0b01000000);
  write_AIC32X4_reg(AIC32X4_RMICPGANIN, 0b01000000);
  write_AIC32X4_reg(AIC32X4_LMICPGAVOL, 0x80);
  write_AIC32X4_reg(AIC32X4_RMICPGAVOL, 0x80);

  write_reg(AIC32X4_PSEL, 0);
  page = 0;
  ADCHighPassEnable();
  write_AIC32X4_reg(AIC32X4_ADCSETUP, 0b11000000);
  write_AIC32X4_reg(AIC32X4_ADCFGA, 0x00);
}


void SetOutputLevels(const uint32_t left, const uint32_t right)
{
  // incoming range expected 0..100 (percent)
  // map 0..100 -> register range 0x81..0x0D (-63.5dB to +6.5dB)
  // codec register: 0x81 = -63.5dB, ..., 0xFF = -0.5dB, 0x00 = 0dB, 0x01 = +0.5dB, ..., 0x0D = +6.5dB
  // Total range: 0x81 (129) to 0x0D (13) wrapping through 0xFF->0x00
  // Linear steps from -127 to +13 in two's complement = 140 steps total

  uint8_t left_reg = 0x81;
  uint8_t right_reg = 0x81;

  // clamp to 100
  uint32_t left_clamped = left > 100 ? 100 : left;
  uint32_t right_clamped = right > 100 ? 100 : right;

  // Map 0->0x81 (-63.5dB), 100->0x0D (+6.5dB)
  // Convert to signed: 0x81=-127, 0xFF=-1, 0x00=0, 0x0D=+13
  // Range: -127 to +13 = 140 steps
  // Formula: signed_val = -127 + (input * 140 / 100)
  int16_t left_signed = (int16_t)(-127 + (left_clamped * 140 / 100));
  int16_t right_signed = (int16_t)(-127 + (right_clamped * 140 / 100));

  // Clamp to valid range
  if (left_signed > 13) left_signed = 13;
  if (left_signed < -127) left_signed = -127;
  if (right_signed > 13) right_signed = 13;
  if (right_signed < -127) right_signed = -127;

  // Convert signed to register value (two's complement for negative)
  left_reg = (uint8_t)(left_signed & 0xFF);
  right_reg = (uint8_t)(right_signed & 0xFF);

  // No mute needed - full volume range is available
  write_AIC32X4_reg(AIC32X4_DACMUTE, 0x00);

  // write volume registers
  write_AIC32X4_reg(AIC32X4_LDACVOL, left_reg);
  write_AIC32X4_reg(AIC32X4_RDACVOL, right_reg);
}

static void cfg_i2s()
{
  ESP_LOGI(TAG, "cfg codec i2s");


  // Set higher drive strength for MCLK
  gpio_set_drive_capability(I2S_MCLK, GPIO_DRIVE_CAP_3);

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = false;
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 512;

  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
    .clk_cfg = {
      //.sample_rate_hz = 44100,
      .sample_rate_hz = 48000,
      //.clk_src = I2S_CLK_SRC_DEFAULT, // direct XTAL
      .clk_src = SOC_MOD_CLK_APLL,
      .ext_clk_freq_hz = 0,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
      .bclk_div = 0,
    },
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_MCLK,
      .bclk = I2S_BCLK,
      .ws = I2S_WS,
      .dout = I2S_DOUT,
      .din = I2S_DIN,
      .invert_flags = {
        .mclk_inv = true,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };

  /* Initialize the channels */
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

void i2s_read(void* buf, uint32_t size, uint32_t* bytes_read)
{
  size_t nb;
  ESP_ERROR_CHECK(i2s_channel_read(rx_handle, buf, size, &nb, portMAX_DELAY));
  *bytes_read = nb;
}

void i2s_write(void* buf, uint32_t size, uint32_t* bytes_read)
{
  size_t nb;
  ESP_ERROR_CHECK(i2s_channel_write(tx_handle, buf, size, &nb, portMAX_DELAY));
  *bytes_read = nb;
}

void InitCodec()
{
  cfg_i2c();
  identify();
  SetOutputLevels(0, 0);
  cfg_i2s();
  cfg_codec(false);
  SetOutputLevels(58, 58);
}

void SetMute(uint32_t mute_l, uint32_t mute_r)
{
  // incoming range 0 to 63 for lvol and rvol, default 0dB is 58
  uint8_t dac_mute = 0x00;
  if (mute_l)
  {
    dac_mute |= 0b00001000; // mute left channel
  }
  if (mute_r)
  {
    dac_mute |= 0b00000100; // mute right channel
  }
  write_AIC32X4_reg(AIC32X4_DACMUTE, dac_mute);
}

