// Copyright 2019 Aivaras Spaicys
// Licensed under the GNU Affero General Public License v3.0
// You may obtain a copy of the License at
//     https://opensource.org/licenses/AGPL-3.0


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "eeprom.h"
#include "esp_bt_device.h"



#define EE_TAG "EEPROM"
#define NVS_PAGE "eeprom"
#define LOCATION_BDA "last-mac"



static nvs_handle_t eeprom_handle = 0;



esp_err_t init_settings()
{
	return nvs_open(NVS_PAGE, NVS_READWRITE, &eeprom_handle);
}

esp_err_t save_bda(uint8_t *bda)
{
	esp_err_t err = nvs_set_blob(eeprom_handle, LOCATION_BDA, bda, ESP_BD_ADDR_LEN);
	if (err == ESP_OK) {
		err = nvs_commit(eeprom_handle);
	}

	if (err == ESP_OK) {
		ESP_LOGI(EE_TAG, "BDA saved successfully");
	}

	return err;
}

esp_err_t load_bda(uint8_t *bda)
{
	size_t required_size = 0;

	esp_err_t err = nvs_get_blob(eeprom_handle, LOCATION_BDA, NULL, &required_size);
	if (required_size == ESP_BD_ADDR_LEN) {
		err = nvs_get_blob(eeprom_handle, LOCATION_BDA, bda, &required_size);
	}

	if (err == ESP_OK) {
		ESP_LOGI(EE_TAG, "Got BDA: [%02x:%02x:%02x:%02x:%02x:%02x]", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
	} else {
		ESP_LOGE(EE_TAG, "Failed to get BDA");
	}

	return err;
}
