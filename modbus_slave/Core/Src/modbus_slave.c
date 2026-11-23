/* modbus_slave.c -- bare-metal Modbus RTU slave, function 0x03 only.
 * Nucleo-F446RE, USART1 (PA9/PA10) on the RS485 module.
 * PA8 pulsed high while building+sending the response (logic-analyzer marker).
 *
 * STATUS: written and code-reviewed; not yet run on hardware (bus solder
 * joint failed before the first exchange). Flash and validate once repaired.
 *
 * Frame detection: per-byte RX interrupt records the arrival tick; the main
 * loop declares a frame complete after >= 2 ms of silence (1.75 ms t3.5,
 * rounded up to the 1 ms SysTick granularity).
 */
#include "main.h"
#include "usart.h"
#include "modbus_slave.h"
#include <string.h>

#define SLAVE_ADDR 0x01
#define T35_MS 2 /* 1.75 ms rounded up to SysTick ticks */
#define NREGS 8

static volatile uint8_t rxbuf[256];
static volatile uint16_t rxlen = 0;
static volatile uint32_t last_byte_tick = 0;
static volatile uint8_t frame_ready = 0;
static uint8_t rx_byte;

/* The eight holding registers the master reads. Dummy values;
 * a real PLC would map these to sensor inputs / actuator setpoints. */
static uint16_t holding_regs[NREGS] = {
	100, 200, 300, 400, 500, 600, 700, 800};

static uint16_t modbus_crc16(const uint8_t *buf, uint16_t len)
{
	uint16_t crc = 0xFFFF;
	for (uint16_t i = 0; i < len; i++)
	{
		crc ^= buf[i];
		for (uint8_t b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
	}
	return crc;
}

/* HAL RX-complete callback: one byte per interrupt. Re-arm immediately so
 * the next byte also interrupts. This is the slave-side equivalent of the
 * driver's receive_buf -- every byte refreshes the silence deadline. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance != USART1)
		return;

	last_byte_tick = HAL_GetTick();
	if (rxlen < sizeof(rxbuf))
		rxbuf[rxlen++] = rx_byte;
	else
		rxlen = 0; /* overflow: drop the frame */

	HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
}

static void reset_rx(void)
{
	rxlen = 0;
	frame_ready = 0;
}

/* Poll for t3.5 silence; call every loop iteration. The main loop, not the
 * ISR, decides a frame is complete -- keeps the ISR minimal (just capture). */
static void modbus_poll_frame_timeout(void)
{
	if (rxlen > 0 && !frame_ready &&
		(HAL_GetTick() - last_byte_tick) >= T35_MS)
	{
		frame_ready = 1;
	}
}

static void modbus_handle_frame(void)
{
	/* Minimum valid 0x03 request: addr func start(2) count(2) crc(2) = 8 */
	if (rxlen < 8)
	{
		reset_rx();
		return;
	}

	uint16_t rx_crc = rxbuf[rxlen - 2] | (rxbuf[rxlen - 1] << 8);
	if (modbus_crc16((uint8_t *)rxbuf, rxlen - 2) != rx_crc ||
		rxbuf[0] != SLAVE_ADDR || rxbuf[1] != 0x03)
	{
		reset_rx();
		return;
	}

	uint16_t start_reg = (rxbuf[2] << 8) | rxbuf[3];
	uint16_t nregs = (rxbuf[4] << 8) | rxbuf[5];
	if (nregs == 0 || start_reg + nregs > NREGS)
	{
		reset_rx();
		return;
	}

	/* PA8 high: "slave is building + sending the response."
	 * This is the slave-side timing marker for the logic analyzer --
	 * its rising edge is T(slave-start), falling edge T(slave-done). */
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);

	uint8_t resp[5 + 2 * NREGS];
	uint16_t rlen = 0;
	resp[rlen++] = SLAVE_ADDR;
	resp[rlen++] = 0x03;
	resp[rlen++] = (uint8_t)(nregs * 2);
	for (uint16_t i = 0; i < nregs; i++)
	{
		resp[rlen++] = holding_regs[start_reg + i] >> 8;
		resp[rlen++] = holding_regs[start_reg + i] & 0xFF;
	}
	uint16_t crc = modbus_crc16(resp, rlen);
	resp[rlen++] = crc & 0xFF;
	resp[rlen++] = crc >> 8;

	/* Auto direction control on the RS485 board: plain blocking transmit,
	 * no DE/RE GPIO toggling needed. */
	HAL_UART_Transmit(&huart1, resp, rlen, 50);

	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);

	reset_rx();
}

void modbus_slave_init(void)
{
	HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
}

void modbus_slave_task(void)
{
	modbus_poll_frame_timeout();
	if (frame_ready)
		modbus_handle_frame();
}