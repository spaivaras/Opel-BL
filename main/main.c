// Original work: Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
// Released under the Apache License, Version 2.0
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0

// Modified work: Copyright 2019 Aivaras Spaicys
// Licensed under the GNU Affero General Public License v3.0
// You may obtain a copy of the License at
//     https://opensource.org/licenses/AGPL-3.0


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "bt_app_av.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "driver/i2s.h"
#include "driver/can.h"
#include "eeprom.h"

#define GPIO_INPUT_IO_0     0
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0))
#define ESP_INTR_FLAG_DEFAULT 0

#define CAN_TIMING_CONFIG_95KBITS()  {.brp = 42, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}
#define CAN_TIMING_CONFIG_33K3BITS() {.brp = 120, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}

/* event for handler "bt_av_hdl_stack_up */
enum {
	BT_APP_EVT_STACK_UP = 0,
};

/* GPIO ISR event queue */
static xQueueHandle gpio_evt_queue = NULL;

/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

static const can_filter_config_t f_config = {
		// Acceptance code for 29b  ID[31..3], RTR[2], dosnt' matter[1..0]. Note ID bits moved to left by 3 bits. 32BIT id << 3
		// Acceptance code for 11b: ID[13..3], RTR[2], dosnt' matter[1..0]. Docu lies!!! on 33.3k? but not on 95k? need to investigate
		.acceptance_code = 0b01000000110000000000000000000000,  //0x206
		.acceptance_mask = 0b00000000000111111111111111111111,
		//.acceptance_code = 0,
		//.acceptance_mask = 0xFFFFFFFF,
		.single_filter = true
};
static const can_timing_config_t t_config = CAN_TIMING_CONFIG_95KBITS();
static const can_general_config_t g_config = {
		.mode = CAN_MODE_LISTEN_ONLY,
		.tx_io = 23, .rx_io = 34,
		.clkout_io = CAN_IO_UNUSED,
		.bus_off_io = CAN_IO_UNUSED,
		.tx_queue_len = 0,
		.rx_queue_len = 5,
		.alerts_enabled = CAN_ALERT_NONE,
		.clkout_divider = 0
};



static void IRAM_ATTR gpio_isr_handler(void *arg) {
	uint32_t gpio_num = (uint32_t) arg;
	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_manager(void *arg) {
	ESP_LOGI("GPIO", "GPIO task on core %d", xPortGetCoreID());

	uint32_t io_num;
	for (;;) {
		if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
			ESP_LOGI("GPIO", "GPIO[%d] INTERRUPT value: %d", io_num,
					gpio_get_level(io_num));

			if (io_num == 0 && gpio_get_level(io_num) == 0) {
				bt_av_send_next_press();
			}
		}
	}
}

static volatile bool next_pressed = 0;
static volatile bool prev_pressed = 0;

static void can_receive_task(void *arg) {
	ESP_LOGI("CAN", "CAN task on core %d", xPortGetCoreID());

	for (;;) {
		can_message_t rx_msg;
		can_receive(&rx_msg, portMAX_DELAY);

		ESP_LOGI("CAN", "ID: %u, DLC: %d", rx_msg.identifier, rx_msg.data_length_code);

		if (rx_msg.data[1] == 0x91) { // UP
			if (rx_msg.data[0] == 1) {
				if (!next_pressed) {
					next_pressed = true;
					bt_av_send_next_press();
				}
			} else {
				next_pressed = false;
				ESP_LOGI("CAN", "Next released");
			}
		} else {
			if (rx_msg.data[1] == 0x92) { // DOWN
				if (rx_msg.data[0] == 1) {
					if (!prev_pressed) {
						prev_pressed = true;
						bt_av_send_prev_press();
					}
				} else {
					prev_pressed = false;
					ESP_LOGI("CAN", "Prev released");
				}
			}
		}
	}
}


void app_main() {
	// Initialize NVS it is used to store PHY calibration data
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	ESP_ERROR_CHECK_WITHOUT_ABORT(init_settings());

	i2s_config_t i2s_config = {
		#ifdef CONFIG_OUTPUT_INTERNAL_DAC
			.mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
		#else
			.mode = I2S_MODE_MASTER | I2S_MODE_TX,        // Only TX
		#endif
		.sample_rate = 48000,
		.bits_per_sample = 16,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,     // 2-channels
		.communication_format = I2S_COMM_FORMAT_I2S_MSB,
		.dma_buf_count = 6,
		.dma_buf_len = 60,
		.intr_alloc_flags = 0,                            //Default interrupt priority
		.tx_desc_auto_clear = true                        //Auto clear tx descriptor on underflow
	};
	i2s_driver_install(0, &i2s_config, 0, NULL);

	#ifdef CONFIG_OUTPUT_INTERNAL_DAC
    	i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    	i2s_set_pin(0, NULL);
	#else
		i2s_pin_config_t pin_config = {
			.bck_io_num = CONFIG_I2S_BCK_PIN,
			.ws_io_num = CONFIG_I2S_LRCK_PIN,
			.data_out_num = CONFIG_I2S_DATA_PIN,
			.data_in_num = -1                                 //Not used
		};
		i2s_set_pin(0, &pin_config);
	#endif

	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

	if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(err));
		return;
	}

	if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(err));
		return;
	}

	if ((err = esp_bluedroid_init()) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(err));
		return;
	}

	if ((err = esp_bluedroid_enable()) != ESP_OK) {
		ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(err));
		return;
	}

	// Create BT app task
	bt_app_task_start_up();

	// Bluetooth device name, connection mode and profile set up
	bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

	#if (CONFIG_BT_SSP_ENABLED == true)
		/* Set default parameters for Secure Simple Pairing */
		esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
		esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
		esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
	#endif

	// Set default parameters for Legacy Pairing
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
	esp_bt_pin_code_t pin_code;
	pin_code[0] = '1';
	pin_code[1] = '2';
	pin_code[2] = '3';
	pin_code[3] = '4';
	esp_bt_gap_set_pin(pin_type, 4, pin_code);


	// Prepare GPIO
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_PIN_INTR_NEGEDGE;
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	gpio_config(&io_conf);

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	xTaskCreate(gpio_manager, "gpio_manager", 2048, NULL, 10, NULL);

	// Prepare GPIO ISR
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*)GPIO_INPUT_IO_0);

	// Prepare CAN
	ESP_ERROR_CHECK(can_driver_install(&g_config, &t_config, &f_config));
	ESP_LOGI("CAN", "Driver installed");
	ESP_ERROR_CHECK(can_start());
	xTaskCreate(can_receive_task, "CAN_rx", 4096, NULL, 9, NULL);
	ESP_LOGI("CAN", "Driver started");

	uint32_t a = 0x206;
	uint32_t b = a << 3;
	ESP_LOGI("MATH", "a: %u, b: %u", a, b);


}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
	switch (event) {
		case ESP_BT_GAP_AUTH_CMPL_EVT: {
			if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
				ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
				esp_log_buffer_hex(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);

				ESP_ERROR_CHECK_WITHOUT_ABORT(save_bda(param->auth_cmpl.bda));
			} else {
				ESP_LOGE(BT_AV_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
			}
			break;
		}
		#if (CONFIG_BT_SSP_ENABLED == true)
			case ESP_BT_GAP_CFM_REQ_EVT:
				ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
				esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
				break;
			case ESP_BT_GAP_KEY_NOTIF_EVT:
				ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
				break;
			case ESP_BT_GAP_KEY_REQ_EVT:
				ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
				break;
		#endif

		default: {
			ESP_LOGI(BT_AV_TAG, "event: %d", event);
			break;
		}
	}
	return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param) {
	ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);

	switch (event) {
		case BT_APP_EVT_STACK_UP: {
			// Dev name
			char *dev_name = "Zhopel";
			esp_bt_dev_set_device_name(dev_name);

			esp_bt_gap_register_callback(bt_app_gap_cb);

			// Initialize AVRCP controller
			esp_avrc_ct_init();
			esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

			// initialize AVRCP target
			assert(esp_avrc_tg_init() == ESP_OK);
			esp_avrc_tg_register_callback(bt_app_rc_tg_cb);

			// Initialize A2DP sink
			esp_a2d_register_callback(&bt_app_a2d_cb);
			esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
			esp_a2d_sink_init();

			uint8_t *bda = malloc(ESP_BD_ADDR_LEN);
			esp_err_t err = load_bda(bda);

			if (!err) {
				ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_sink_connect(bda));
			}

			free(bda);

			// Set discoverable and connectable mode, wait to be connected
			esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
			break;
		}
		default:
			ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
			break;
	}
}
