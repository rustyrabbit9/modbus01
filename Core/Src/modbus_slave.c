#include "modbus_slave.h"

#include "main.h"
#include <string.h>

#define MODBUS_SLAVE_ADDRESS       1U
#define MODBUS_READ_INPUT_REGISTERS 0x04U
#define MODBUS_REQUEST_SIZE        8U
#define MODBUS_RX_RING_SIZE        64U
#define MODBUS_UART_TIMEOUT_MS     100U
#define MODBUS_RESPONSE_DELAY_MS   30U
#define MODBUS_MAX_RESPONSE_SIZE   7U
#define CAN_ID_PREFIX_SIZE         2U

#define INPUT_REGISTER_TEMPERATURE 0x0000U
#define INPUT_REGISTER_CURRENT     0x0001U
#define INPUT_REGISTER_VOLTAGE     0x0002U

#define RESPONSE_CAN_ID_EXCEPTION  0x330U
#define RESPONSE_CAN_ID_TEMPERATURE 0x331U
#define RESPONSE_CAN_ID_CURRENT     0x332U
#define RESPONSE_CAN_ID_VOLTAGE     0x333U

/* Fixed simulated values: 25.0 C, 1.500 A and 24.00 V. */
#define SIMULATED_TEMPERATURE      250U
#define SIMULATED_CURRENT_MA       1500U
#define SIMULATED_VOLTAGE_CENTIVOLT 2400U

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

static void Modbus_Send(const uint8_t *data, uint16_t length, uint16_t can_id)
{
  uint8_t serial_frame[CAN_ID_PREFIX_SIZE + MODBUS_MAX_RESPONSE_SIZE];

  if (length > MODBUS_MAX_RESPONSE_SIZE)
  {
    return;
  }

  /*
   * CS-CANET100 "transparent conversion with identifier" extracts the first
   * two serial bytes as a standard CAN ID. These bytes are not part of the
   * CAN payload, so PCAN still receives the original Modbus RTU response.
   */
  serial_frame[0] = (uint8_t)(can_id >> 8U);
  serial_frame[1] = (uint8_t)(can_id & 0x00FFU);
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
  Modbus_Send(response, sizeof(response), RESPONSE_CAN_ID_EXCEPTION);
}

static void Modbus_ProcessRequest(const uint8_t *frame)
{
  uint16_t register_address;
  uint16_t register_count;
  uint16_t register_value;
  uint16_t response_can_id;
  uint16_t crc;
  uint8_t response[7];

  if (frame[0] != MODBUS_SLAVE_ADDRESS)
  {
    return;
  }

  if (frame[1] != MODBUS_READ_INPUT_REGISTERS)
  {
    Modbus_SendException(frame[1], 0x01U);
    return;
  }

  register_address = ((uint16_t)frame[2] << 8U) | frame[3];
  register_count = ((uint16_t)frame[4] << 8U) | frame[5];

  if (register_count != 1U)
  {
    Modbus_SendException(frame[1], 0x03U);
    return;
  }

  switch (register_address)
  {
    case INPUT_REGISTER_TEMPERATURE:
      register_value = SIMULATED_TEMPERATURE;
      response_can_id = RESPONSE_CAN_ID_TEMPERATURE;
      break;

    case INPUT_REGISTER_CURRENT:
      register_value = SIMULATED_CURRENT_MA;
      response_can_id = RESPONSE_CAN_ID_CURRENT;
      break;

    case INPUT_REGISTER_VOLTAGE:
      register_value = SIMULATED_VOLTAGE_CENTIVOLT;
      response_can_id = RESPONSE_CAN_ID_VOLTAGE;
      break;

    default:
      Modbus_SendException(frame[1], 0x02U);
      return;
  }

  response[0] = MODBUS_SLAVE_ADDRESS;
  response[1] = MODBUS_READ_INPUT_REGISTERS;
  response[2] = 2U;
  response[3] = (uint8_t)(register_value >> 8U);
  response[4] = (uint8_t)(register_value & 0x00FFU);
  crc = Modbus_Crc16(response, 5U);
  response[5] = (uint8_t)(crc & 0x00FFU);
  response[6] = (uint8_t)(crc >> 8U);
  Modbus_Send(response, sizeof(response), response_can_id);
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
