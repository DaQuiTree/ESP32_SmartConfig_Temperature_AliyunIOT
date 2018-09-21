#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "tcpip_adapter.h"
#include "esp_smartconfig.h"

#include "nvs_str.h"

#define STORAGE_NAMESPACE "storage"

esp_err_t save_AP_Info(wifi_config_t *wifi_config)
{
    nvs_handle my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Write value including previously saved blob if available
    err = nvs_set_str(my_handle, "AP_SSID", (const char*)wifi_config->sta.ssid);
    if (err != ESP_OK) return err;

    err = nvs_set_str(my_handle, "AP_PASSWORD", (const char*)wifi_config->sta.password);
    if (err != ESP_OK) return err;

    // Commit
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t get_AP_Info(wifi_config_t *wifi_config)
{
    nvs_handle my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read run time blob
    size_t required_size = 0;  // value will default to 0, if not set yet in NVS
    // obtain required memory space to store blob being read from NVS
    err = nvs_get_str(my_handle, "AP_SSID", NULL, &required_size);
    if (err != ESP_OK) return err;

    if (required_size == 0) {
        printf("Nothing saved yet!\n");
    } else {
        err = nvs_get_str(my_handle, "AP_SSID", (char*)wifi_config->sta.ssid, &required_size);
        printf("Read out saved ssid: %s\n",wifi_config->sta.ssid);
        if (err != ESP_OK) return err;
    }

    err = nvs_get_str(my_handle, "AP_PASSWORD", NULL, &required_size);
    if (err != ESP_OK) return err;

    if (required_size == 0) {
        printf("Nothing saved yet!\n");
    } else {
        err = nvs_get_str(my_handle, "AP_PASSWORD", (char*)wifi_config->sta.password, &required_size);
        printf("Read out saved password: %s\n",wifi_config->sta.password);
        if (err != ESP_OK) return err;
    }

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}
