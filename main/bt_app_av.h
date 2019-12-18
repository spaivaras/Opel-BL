// Copyright 2019 Aivaras Spaicys
// Licensed under the GNU Affero General Public License v3.0
// You may obtain a copy of the License at
//     https://opensource.org/licenses/AGPL-3.0


#ifndef __BT_APP_AV_H__
#define __BT_APP_AV_H__

#include <stdint.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#define BT_AV_TAG               "BT_AV"
#define BT_RC_TG_TAG            "RCTG"
#define BT_RC_CT_TAG            "RCCT"

/**
 * @brief     callback function for A2DP sink
 */
void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/**
 * @brief     callback function for A2DP sink audio data stream
 */
void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);

/**
 * @brief     callback function for AVRCP controller
 */
void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/**
 * @brief     callback function for AVRCP target
 */
void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

void bt_av_send_next_press();

void bt_av_send_prev_press();

#endif /* __BT_APP_AV_H__*/
