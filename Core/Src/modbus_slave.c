#include "modbus_slave.h"

#include "main.h"
#include <string.h>

/**
 *  CAN ID is added by the converter, outside the CRC. CRC is low byte first.
 *  reg num is 1..6; bytes num is 2 x reg num, and reg value grows to match.
 *
 *  Example:
 *  Request
 *  CAN ID | slave addr | func | start reg addr | reg num | crc
 *  0220   | 01         | 03   | 00 02          | 00 01   | 25 CA
 *
 *  Response
 *  CAN ID | slave addr | func | bytes num | reg value | crc
 *  0333   | 01         | 03   | 02        | 09 60     | BE 3C
 */

#define MODBUS_SLAVE_ADDRESS       1U
#define MODBUS_READ_HOLDING_REGISTERS 0x03U
#define MODBUS_REQUEST_SIZE        8U
#define MODBUS_RX_RING_SIZE        64U
#define MODBUS_UART_TIMEOUT_MS     100U
#define MODBUS_RESPONSE_DELAY_MS   30U
#define MODBUS_MAX_REGISTERS_PER_READ 6U
#define MODBUS_MAX_RESPONSE_SIZE   (5U + (2U * MODBUS_MAX_REGISTERS_PER_READ))
#define CAN_ID_PREFIX_SIZE         2U

/* Single response ID for every reply, data and exception alike. */
#define RESPONSE_CAN_ID            0x0333U

#define HOLDING_REGISTER_TEMPERATURE 0x0000U
#define HOLDING_REGISTER_CURRENT     0x0001U
#define HOLDING_REGISTER_VOLTAGE     0x0002U

/* Six consecutive registers: year, month, day, hour, minute, second. */
#define HOLDING_REGISTER_DATETIME    0x4002U
#define DATETIME_REGISTER_COUNT      6U

/* Fixed simulated values: 25.0 C, 1.500 A and 24.00 V. */
#define SIMULATED_TEMPERATURE      250U
#define SIMULATED_CURRENT_MA       1500U
#define SIMULATED_VOLTAGE_CENTIVOLT 2400U

/* Fixed simulated timestamp: 2021-06-20 13:25:42. */
#define SIMULATED_DATETIME_YEAR    2021U
#define SIMULATED_DATETIME_MONTH      6U
#define SIMULATED_DATETIME_DAY       20U
#define SIMULATED_DATETIME_HOUR      13U
#define SIMULATED_DATETIME_MINUTE    25U
#define SIMULATED_DATETIME_SECOND    42U

#define MODBUS_CRC16_INITIAL_VALUE  0xFFFFU
#define MODBUS_CRC16_POLYNOMIAL     0xA001U

static UART_HandleTypeDef *modbus_uart;
static uint8_t uart_rx_byte;
static volatile uint8_t rx_ring[MODBUS_RX_RING_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static uint8_t request[MODBUS_REQUEST_SIZE];
static uint8_t request_length;

static uint16_t Modbus_Crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = MODBUS_CRC16_INITIAL_VALUE;

  for (uint16_t index = 0U; index < length; ++index)
  {
    crc ^= data[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit)
    {
      if ((crc & 0x0001U) != 0U)
      {
        crc = (crc >> 1U) ^ MODBUS_CRC16_POLYNOMIAL;
      }
      else
      {
        crc >>= 1U;
      }
    }
  }

  return crc;
}

static void Modbus_Send(const uint8_t *data, uint16_t length)
{
  uint8_t serial_frame[CAN_ID_PREFIX_SIZE + MODBUS_MAX_RESPONSE_SIZE];

  if (length > MODBUS_MAX_RESPONSE_SIZE)
  {
    return;
  }

  /*
   * CS-CANET100 "transparent conversion with identifier" consumes the first
   * two serial bytes as a standard CAN ID. They are not part of the CAN
   * payload, so the receiver still sees an unmodified Modbus RTU response.
   */
  serial_frame[0] = (uint8_t)(RESPONSE_CAN_ID >> 8U);
  serial_frame[1] = (uint8_t)(RESPONSE_CAN_ID & 0x00FFU);
  memcpy(&serial_frame[CAN_ID_PREFIX_SIZE], data, length);

  /* Let a short burst of CAN-to-RS485 requests finish before driving the bus. */
  HAL_Delay(MODBUS_RESPONSE_DELAY_MS);
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);

  if (HAL_UART_Transmit(modbus_uart, serial_frame,
                        length + CAN_ID_PREFIX_SIZE,
                        MODBUS_UART_TIMEOUT_MS) == HAL_OK)
  {
    while (__HAL_UART_GET_FLAG(modbus_uart, UART_FLAG_TC) == RESET)
    {
    }
  }

  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
}

static void Modbus_SendException(uint8_t function, uint8_t exception_code)
{
  uint8_t response[5];
  uint16_t crc;

  response[0] = MODBUS_SLAVE_ADDRESS;
  response[1] = function | 0x80U;
  response[2] = exception_code;
  crc = Modbus_Crc16(response, 3U);
  response[3] = (uint8_t)(crc & 0x00FFU);
  response[4] = (uint8_t)(crc >> 8U);
  Modbus_Send(response, sizeof(response));
}

/* Returns 0 for an address that is not mapped, 1 after storing its value. */
static uint8_t Modbus_ReadRegister(uint16_t address, uint16_t *value)
{
  static const uint16_t datetime[DATETIME_REGISTER_COUNT] = {
    SIMULATED_DATETIME_YEAR,
    SIMULATED_DATETIME_MONTH,
    SIMULATED_DATETIME_DAY,
    SIMULATED_DATETIME_HOUR,
    SIMULATED_DATETIME_MINUTE,
    SIMULATED_DATETIME_SECOND,
  };

  if ((address >= HOLDING_REGISTER_DATETIME) &&
      (address < (HOLDING_REGISTER_DATETIME + DATETIME_REGISTER_COUNT)))
  {
    *value = datetime[address - HOLDING_REGISTER_DATETIME];
    return 1U;
  }

  switch (address)
  {
    case HOLDING_REGISTER_TEMPERATURE:
      *value = SIMULATED_TEMPERATURE;
      break;

    case HOLDING_REGISTER_CURRENT:
      *value = SIMULATED_CURRENT_MA;
      break;

    case HOLDING_REGISTER_VOLTAGE:
      *value = SIMULATED_VOLTAGE_CENTIVOLT;
      break;

    default:
      return 0U;
  }

  return 1U;
}

static void Modbus_ProcessRequest(const uint8_t *frame)
{
  uint16_t register_address;
  uint16_t register_count;
  uint16_t data_length;
  uint16_t crc;
  uint8_t response[MODBUS_MAX_RESPONSE_SIZE];

  if (frame[0] != MODBUS_SLAVE_ADDRESS)
  {
    return;
  }

  if (frame[1] != MODBUS_READ_HOLDING_REGISTERS)
  {
    Modbus_SendException(frame[1], 0x01U);
    return;
  }

  register_address = ((uint16_t)frame[2] << 8U) | frame[3];
  register_count = ((uint16_t)frame[4] << 8U) | frame[5];

  if ((register_count == 0U) ||
      (register_count > MODBUS_MAX_REGISTERS_PER_READ))
  {
    Modbus_SendException(frame[1], 0x03U);
    return;
  }

  /* Response, offsets 2..: address, function, byte count, data, CRC. */
  data_length = (uint16_t)(register_count * 2U);
  response[0] = MODBUS_SLAVE_ADDRESS;
  response[1] = MODBUS_READ_HOLDING_REGISTERS;
  response[2] = (uint8_t)data_length;

  for (uint16_t index = 0U; index < register_count; ++index)
  {
    uint16_t register_value;

    if (Modbus_ReadRegister((uint16_t)(register_address + index),
                            &register_value) == 0U)
    {
      Modbus_SendException(frame[1], 0x02U);
      return;
    }

    response[3U + (index * 2U)] = (uint8_t)(register_value >> 8U);
    response[4U + (index * 2U)] = (uint8_t)(register_value & 0x00FFU);
  }

  crc = Modbus_Crc16(response, (uint16_t)(3U + data_length));
  response[3U + data_length] = (uint8_t)(crc & 0x00FFU);
  response[4U + data_length] = (uint8_t)(crc >> 8U);
  Modbus_Send(response, (uint16_t)(5U + data_length));
}

void ModbusSlave_Init(UART_HandleTypeDef *uart)
{
  modbus_uart = uart;
  rx_head = 0U;
  rx_tail = 0U;
  request_length = 0U;
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);

  if (HAL_UART_Receive_IT(modbus_uart, &uart_rx_byte, 1U) != HAL_OK)
  {
    Error_Handler();
  }
}

/*
 * The converter prefixes each inbound frame with its two CAN ID bytes as well.
 * They are discarded by the CRC resynchronisation below, which slides the
 * window one byte at a time until the eight request bytes line up.
 */
void ModbusSlave_Poll(void)
{
  while (rx_tail != rx_head)
  {
    request[request_length++] = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) % MODBUS_RX_RING_SIZE);

    if (request_length == MODBUS_REQUEST_SIZE)
    {
      uint16_t received_crc = (uint16_t)request[6] | ((uint16_t)request[7] << 8U);

      if (Modbus_Crc16(request, 6U) == received_crc)
      {
        Modbus_ProcessRequest(request);
        request_length = 0U;
      }
      else
      {
        memmove(request, &request[1], MODBUS_REQUEST_SIZE - 1U);
        request_length = MODBUS_REQUEST_SIZE - 1U;
      }
    }
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == modbus_uart)
  {
    uint16_t next_head = (uint16_t)((rx_head + 1U) % MODBUS_RX_RING_SIZE);

    if (next_head != rx_tail)
    {
      rx_ring[rx_head] = uart_rx_byte;
      rx_head = next_head;
    }

    (void)HAL_UART_Receive_IT(modbus_uart, &uart_rx_byte, 1U);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == modbus_uart)
  {
    (void)HAL_UART_Receive_IT(modbus_uart, &uart_rx_byte, 1U);
  }
}
