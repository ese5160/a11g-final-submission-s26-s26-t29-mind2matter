/***************************************************************************/ /**
 * @file sl_si91x_debug_uc_config.h
 * @brief Local debug configuration override.
 *
 * BRD2708A routes the on-board USB VCOM through ULP UART, so keep debug enabled
 * on that peripheral and reserve it for the J-Link virtual COM port.
 ******************************************************************************/

#ifndef SL_SI91X_DEBUG_UC_CONFIG_H
#define SL_SI91X_DEBUG_UC_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>
// <h> Debug Configuration

//  <e>Enable/Disable Debug Configuration
#define DEBUG_UART_ENABLE 1
#if (DEBUG_UART_ENABLE == 1)
#define DEBUG_UART 1
#else
#undef DEBUG_UART
#endif

#define SL_M4_USART0_INSTANCE 0
#define SL_M4_UART1_INSTANCE  1
#define SL_ULP_UART_INSTANCE  2

// <o SL_DEBUG_INSTANCE> Instance (Select TX/RX pins from chosen component)
#define SL_DEBUG_INSTANCE SL_ULP_UART_INSTANCE

// <o SL_DEBUG_BAUD_RATE> Baud Rate (Baud/Second) <300-7372800>
#define SL_DEBUG_BAUD_RATE 115200

// </e>
// </h>
// <<< end of configuration section >>>

#endif /* SL_SI91X_DEBUG_UC_CONFIG_H */
