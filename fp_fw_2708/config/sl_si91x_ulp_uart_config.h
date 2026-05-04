/***************************************************************************/ /**
 * @file sl_si91x_ulp_uart_config.h
 * @brief Local ULP UART runtime config.
 ******************************************************************************/

#ifndef SL_SI91X_ULP_UART_CONFIG_H
#define SL_SI91X_ULP_UART_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sl_si91x_usart.h"

// <<< Use Configuration Wizard in Context Menu >>>

//  <e>ULP UART UC Configuration
//  <i> Keep the UC config available so shared SDK helpers can query defaults.
#define ULP_UART_UC    1
#define ULPUART_MODULE 2

// <h>UART Configuration
#define ENABLE  1
#define DISABLE 0

#if (ULP_UART_UC)
// <o SL_ULP_UART_BAUDRATE> Baud Rate (Baud/Second) <300-7372800>
#define SL_ULP_UART_BAUDRATE 115200

// <o SL_ULP_UART_PARITY> Parity
#define SL_ULP_UART_PARITY SL_USART_NO_PARITY

// <o SL_ULP_UART_STOP_BITS> Stop Bits
#define SL_ULP_UART_STOP_BITS SL_USART_STOP_BITS_1

// <o SL_ULP_UART_DATA_BITS> Data Width
#define SL_ULP_UART_DATA_BITS SL_USART_DATA_BITS_8

// <o SL_ULP_UART_FLOW_CONTROL_TYPE> Flow control
#define SL_ULP_UART_FLOW_CONTROL_TYPE SL_USART_FLOW_CONTROL_NONE
#endif

// </h>  ULP_UART Configuration
// </e>

#ifdef __cplusplus
}
#endif
// <<< end of configuration section >>>

#if (ULP_UART_UC)
sl_si91x_usart_control_config_t ulp_uart_configuration = {
  .baudrate      = SL_ULP_UART_BAUDRATE,
  .mode          = SL_USART_MODE_ASYNCHRONOUS,
  .parity        = SL_ULP_UART_PARITY,
  .stopbits      = SL_ULP_UART_STOP_BITS,
  .hwflowcontrol = SL_ULP_UART_FLOW_CONTROL_TYPE,
  .databits      = SL_ULP_UART_DATA_BITS,
  .usart_module  = ULPUART_MODULE,
};
#endif

#endif // SL_SI91X_ULP_UART_CONFIG_H
