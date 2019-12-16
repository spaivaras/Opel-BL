// Copyright 2019 Aivaras Spaicys
// Licensed under the GNU Affero General Public License v3.0
// You may obtain a copy of the License at
//     https://opensource.org/licenses/AGPL-3.0


#ifndef MAIN_EEPROM_H_
#define MAIN_EEPROM_H_

#include <stdint.h>
#include "esp_system.h"


esp_err_t init_settings();
esp_err_t save_bda(uint8_t *bda);
esp_err_t load_bda(uint8_t *bda);


#endif /* MAIN_EEPROM_H_ */
