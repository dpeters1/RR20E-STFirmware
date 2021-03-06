/*
 * bm78.c
 *
 *  Created on: Mar 17, 2020
 *      Author: Dominic
 */

#include "bm78.h"
#include "main.h"
#include <string.h>
#include "scheduler.h"

static BM78_handler_t bm78;

static uint8_t rx_buffer[200];
static uint8_t rx_buffer_index = 0;


BM78_state_t BT_get_state()
{
	return bm78.state;
}


void BT_init(UART_HandleTypeDef * uart_bt){

	bm78.huart = uart_bt;

	BT_power_off();
}


static void eeprom_enable_config_mode()
{
	if(bm78.mode != MODE_EEPROM) return;

	const uint8_t open_eeprom_command[] = {0x01, 0x03, 0x0c, 0x00};
	const uint8_t write_enable_config_command[] = {0x01, 0x27, 0xfc, 0x04, 0x03, 0x8b, 0x01, 0x02};
	const uint8_t write_led_brightness_command[] = {0x01, 0x27, 0xfc, 0x04, 0x01, 0xee, 0x01, 0x2f};
	const uint8_t write_link_led_solid_command[] = {0x01, 0x27, 0xfc, 0x04, 0x01, 0xf1, 0x01, 0x02};

	HAL_UART_Transmit_IT(bm78.huart, (uint8_t *)open_eeprom_command, sizeof(open_eeprom_command));
	HAL_Delay(50);
	HAL_UART_Transmit_IT(bm78.huart, (uint8_t *)write_enable_config_command, sizeof(write_enable_config_command));
	HAL_Delay(50);
	HAL_UART_Transmit_IT(bm78.huart, (uint8_t *)write_led_brightness_command, sizeof(write_led_brightness_command));
	HAL_Delay(50);
	HAL_UART_Transmit_IT(bm78.huart, (uint8_t *)write_link_led_solid_command, sizeof(write_link_led_solid_command));
	HAL_Delay(50);
}


static void config_status_timeout(void * handle)
{
	// No config status received
	HAL_UART_AbortReceive_IT(bm78.huart);

	BT_power_off();
	HAL_Delay(100);
	// Restart the module in eeprom write mode
	BT_power_on(MODE_EEPROM);
	HAL_Delay(1000);

	eeprom_enable_config_mode();

	BT_power_off();
	HAL_Delay(250);
	// Restart the module in normal mode
	BT_power_on(MODE_NORMAL);
}

void BT_power_on(BM78_mode_t mode)
{
	HAL_GPIO_WritePin(BT_SW_BTN_GPIO_Port, BT_SW_BTN_Pin, GPIO_PIN_SET);
	// Set configuration pins for normal operation
	HAL_GPIO_WritePin(BT_CFG1_GPIO_Port, BT_CFG1_Pin, GPIO_PIN_SET);
	// P2_0 must be low if in test mode
	HAL_GPIO_WritePin(BT_CFG2_GPIO_Port, BT_CFG2_Pin, mode == MODE_NORMAL ? GPIO_PIN_SET : GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BT_EAN_GPIO_Port, BT_EAN_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(BT_RST_GPIO_Port, BT_RST_Pin, GPIO_PIN_SET);

	bm78.state = STATE_POWER_ON;
	bm78.mode = mode;

	if(mode == MODE_NORMAL){
		// If configure mode is enabled, the module will respond with a configure mode status on power up
		HAL_UART_Receive_IT(bm78.huart, rx_buffer, BM78_UART_CMD_HEADER_SIZE);

		scheduler_add(config_status_timeout, NULL, BM78_CONFIG_MODE_TIMEOUT_MS, 0);
	}
}


void BT_power_off()
{
	HAL_UART_AbortReceive_IT(bm78.huart);

	HAL_GPIO_WritePin(BT_SW_BTN_GPIO_Port, BT_SW_BTN_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BT_CFG1_GPIO_Port, BT_CFG1_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BT_CFG2_GPIO_Port, BT_CFG2_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BT_EAN_GPIO_Port, BT_EAN_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BT_RST_GPIO_Port, BT_RST_Pin, GPIO_PIN_RESET);

	bm78.state = STATE_POWER_OFF;
}


void BT_enter_setup()
{
	if(bm78.state != STATE_POWER_ON) return;

	while(BT_get_state() != STATE_CONFIGURATION){
	  scheduler_exec();
	}
	BT_send_command(BM78_CMD_READ_LOCAL_INFORMATION, NULL, 0);
}


void BT_exit_setup(){
	BT_send_command(BM78_CMD_LEAVE_CFG_MODE, NULL, 0);
}


void BT_transmit(uint8_t * buffer, uint16_t len)
{
	if(bm78.state != STATE_ACCESS_READY || bm78.mode != MODE_NORMAL) return;

	HAL_UART_Transmit_IT(bm78.huart, buffer, len);
}

static uint8_t calculate_chksum_byte(uint8_t * buffer, uint16_t len)
{
	uint8_t checksum = 0;

	for(uint8_t i=0; i<len; i++){
		checksum += buffer[i];
	}

	return 0xFF - checksum + 1;
}

void BT_send_command(uint8_t command, uint8_t * params, uint16_t params_len)
{
	static uint8_t command_buffer[BM78_COMMAND_BUFFER_SIZE];
	uint16_t encoded_len = 0;

	memset(command_buffer, 0, sizeof(command_buffer));

	command_buffer[encoded_len++] = BM78_UART_SYNC_WORD;

	uint16_t message_len = params_len + sizeof(command);
	command_buffer[encoded_len++] = (message_len & 0xFF00) >> 8;
	command_buffer[encoded_len++] = message_len & 0xFF;

	command_buffer[encoded_len++] = command;

	memcpy(&command_buffer[encoded_len], params, params_len);
	encoded_len += params_len;

	command_buffer[encoded_len++] = calculate_chksum_byte(&command_buffer[1], message_len + sizeof(message_len));

	HAL_UART_Transmit_IT(bm78.huart, command_buffer, encoded_len);

	// Delay to allow for the module response
	// Commands are all sent in the setup phase so introducing delays isn't so bad
	HAL_Delay(10);
}


void BT_set_device_name(char * name)
{
	if(bm78.state != STATE_CONFIGURATION) return;
	if(strlen(name) > BM78_MAX_ADVERTISING_NAME_SIZE) return;

	uint8_t name_buffer[BM78_MAX_ADVERTISING_NAME_SIZE];
	name_buffer[0] = 0x00; // Don't store in eeprom
	/* Note:
	 * There is an issue with checksums not matching or something, after editing the eeprom to enable
	 * config mode. (See fn 'eeprom_enable_config_mode()')
	 * This causes the eeprom preferences to reset to default when any config write commands are
	 * issued with the eeprom_save flag set. For now, just set the preferences each time
	 */
	strcpy((char *)&name_buffer[1], name);

	BT_send_command(BM78_CMD_WRITE_DEVICE_NAME, name_buffer, strlen(name)+1);
}


void BT_set_pairing_method(BM78_pair_method_t pair_method)
{
	if(bm78.state != STATE_CONFIGURATION) return;

	uint8_t pair_mode_buffer[2];
	pair_mode_buffer[0] = 0x00;
	pair_mode_buffer[1] = pair_method;

	BT_send_command(BM78_CMD_WRITE_PAIR_MODE, pair_mode_buffer, sizeof(pair_mode_buffer));
}


void BT_set_pin(char * pin_code)
{
	if(bm78.state != STATE_CONFIGURATION) return;

	// Pin must be 4 or 6 digits
	uint8_t pin_size = strlen(pin_code);
	if(pin_size != 4 && pin_size != 6) return;

	uint8_t pin_buffer[7];
	pin_buffer[0] = 0x00;
	strcpy((char *)&pin_buffer[1], pin_code);

	BT_send_command(BM78_CMD_WRITE_PIN_CODE, pin_buffer, pin_size+1);
}


void BT_erase_bonds()
{
	if(bm78.state != STATE_CONFIGURATION) return;

	BT_send_command(BM78_CMD_ERASE_PAIR_INFO, NULL, 0);
}

static void config_response_handler(uint8_t * msg, uint16_t len)
{
	uint8_t evt = msg[0];
	uint8_t * params = &msg[1];

	if(evt == BM78_EVT_CONFIG_STATUS){
		// Received the configuration status event, cancel the timeout
		scheduler_del(config_status_timeout, NULL);

		bm78.state = params[0] == 1 ? STATE_CONFIGURATION : STATE_ACCESS_READY;
	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if(huart == bm78.huart){
		if(bm78.state == STATE_POWER_ON || bm78.state == STATE_CONFIGURATION){
			static uint16_t response_msg_len = 0;
			if(response_msg_len == 0){
				response_msg_len = ((rx_buffer[1] << 8) | rx_buffer[2]) + 1;
				HAL_UART_Receive_IT(bm78.huart, rx_buffer, response_msg_len);
			}
			else{
				config_response_handler(rx_buffer, response_msg_len);
				response_msg_len = 0;

				if(bm78.state == STATE_ACCESS_READY){
					// Module entered UART pass-though mode
					memset(rx_buffer, 0, sizeof(rx_buffer));
					HAL_UART_Receive_IT(bm78.huart, rx_buffer, 1);
				}
				else{
					HAL_UART_Receive_IT(bm78.huart, rx_buffer, BM78_UART_CMD_HEADER_SIZE);
				}
			}
		}
		else if(bm78.state == STATE_ACCESS_READY){

			if(rx_buffer[rx_buffer_index] == '\n'){
#ifdef DEBUG
				printf("%s", rx_buffer);
#endif
				memset(rx_buffer, 0, rx_buffer_index+1);
				rx_buffer_index = 0;
			}
			else rx_buffer_index++;

			HAL_UART_Receive_IT(bm78.huart, &rx_buffer[rx_buffer_index], 1);
		}
	}
}
