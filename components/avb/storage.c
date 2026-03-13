//
// Created by max on 1/26/26.
//

#include "storage.h"

#include <esp_log.h>
#define NVS_NAMESPACE "avb"

esp_err_t init_nvs()
{
  // Initialize NVS
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    // NVS partition was truncated and needs to be erased
    ESP_ERROR_CHECK(nvs_flash_erase());
    // Retry nvs_flash_init
    err = nvs_flash_init();
  }
  return err;
}

esp_err_t nvs_write_str(const char* key, const char* data)
{
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
  {
    return err;
  }
  err = nvs_set_str(nvs_handle, key, data);
  nvs_close(nvs_handle);
  return err;
}

esp_err_t nvs_read_str(const char* key, char* out_data, size_t size)
{
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err != ESP_OK)
  {
    return err;
  }
  err = nvs_get_str(nvs_handle, key, out_data, &size);

  nvs_close(nvs_handle);
  return err;
}
