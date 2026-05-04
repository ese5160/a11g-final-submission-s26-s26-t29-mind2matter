/***************************************************************************/ /**
 * @file sl_si91x_uart_common_config.h
 * @brief SL SI91X UART Common Config.
 *******************************************************************************/

#ifndef SL_SI91X_UART_COMMON_CONFIG_H
#define SL_SI91X_UART_COMMON_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>
// <h>DMA Configuration

// <q SL_UART1_DMA_CONFIG_ENABLE> UART1 DMA
// <i> Default: 1
#define SL_UART1_DMA_CONFIG_ENABLE 1

// </h>
// <<< end of configuration section >>>

// <<< sl:start pin_tool >>>
// <uart1 signal=TX,RX,(CTS),(RTS)> SL_UART1
// $[UART1_SL_UART1]
#ifndef SL_UART1_PERIPHERAL                     
#define SL_UART1_PERIPHERAL                      UART1
#endif
#ifndef SL_UART1_PERIPHERAL_NO                  
#define SL_UART1_PERIPHERAL_NO                   1
#endif

// UART1 TX on GPIO_7
#ifndef SL_UART1_TX_PORT                        
#define SL_UART1_TX_PORT                         HP
#endif
#ifndef SL_UART1_TX_PIN                         
#define SL_UART1_TX_PIN                          7
#endif
#ifndef SL_UART1_TX_LOC                         
#define SL_UART1_TX_LOC                          0
#endif

// UART1 RX on GPIO_6
#ifndef SL_UART1_RX_PORT                        
#define SL_UART1_RX_PORT                         HP
#endif
#ifndef SL_UART1_RX_PIN                         
#define SL_UART1_RX_PIN                          6
#endif
#ifndef SL_UART1_RX_LOC                         
#define SL_UART1_RX_LOC                          5
#endif


// [UART1_SL_UART1]$
// <<< sl:end pin_tool >>>

#endif // SL_SI91X_UART_COMMON_CONFIG_H
