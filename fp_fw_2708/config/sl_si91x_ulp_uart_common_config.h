/***************************************************************************/ /**
 * @file sl_si91x_ulp_uart_common_config.h
 * @brief Local ULP UART common config for on-board USB VCOM/debug.
 ******************************************************************************/

#ifndef SL_SI91X_ULP_UART_COMMON_CONFIG_H
#define SL_SI91X_ULP_UART_COMMON_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>
// <h>DMA Configuration

// <q SL_ULPUART_DMA_CONFIG_ENABLE> ULP UART DMA
// <i> Keep DMA off for the board debug/VCOM path.
#define SL_ULPUART_DMA_CONFIG_ENABLE 0

// </h>
// <<< end of configuration section >>>

// <<< sl:start pin_tool >>>
// <ulp_uart signal=TX,RX,(CTS),(RTS)> SL_ULP_UART
// $[ULP_UART_SL_ULP_UART]
#ifndef SL_ULP_UART_PERIPHERAL                  
#define SL_ULP_UART_PERIPHERAL                   ULP_UART
#endif

// ULP_UART TX on ULP_GPIO_11/GPIO_75
#ifndef SL_ULP_UART_TX_PORT                     
#define SL_ULP_UART_TX_PORT                      ULP
#endif
#ifndef SL_ULP_UART_TX_PIN                      
#define SL_ULP_UART_TX_PIN                       11
#endif
#ifndef SL_ULP_UART_TX_LOC                      
#define SL_ULP_UART_TX_LOC                       1
#endif

// ULP_UART RX on ULP_GPIO_9/GPIO_73
#ifndef SL_ULP_UART_RX_PORT                     
#define SL_ULP_UART_RX_PORT                      ULP
#endif
#ifndef SL_ULP_UART_RX_PIN                      
#define SL_ULP_UART_RX_PIN                       9
#endif
#ifndef SL_ULP_UART_RX_LOC                      
#define SL_ULP_UART_RX_LOC                       3
#endif


// [ULP_UART_SL_ULP_UART]$
// <<< sl:end pin_tool >>>

#endif // SL_SI91X_ULP_UART_COMMON_CONFIG_H
