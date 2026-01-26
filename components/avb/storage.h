//
// Created by max on 1/26/26.
//

#ifndef ETHERNET_PTP_STORAGE_H
#define ETHERNET_PTP_STORAGE_H

#include <esp_err.h>
#include <nvs_flash.h>
#include <lwip/err.h>

esp_err_t init_nvs();
esp_err_t nvs_read_str(const char* key, char* out_data, size_t size);
esp_err_t nvs_write_str(const char* key, const char* data);
#endif //ETHERNET_PTP_STORAGE_H
