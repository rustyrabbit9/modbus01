#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include "stm32h7xx_hal.h"

void ModbusSlave_Init(UART_HandleTypeDef *uart);
void ModbusSlave_Poll(void);

#endif /* MODBUS_SLAVE_H */
