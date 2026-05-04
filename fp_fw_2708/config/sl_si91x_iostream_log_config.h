/***************************************************************************/ /**
 * @file
 * @brief Local override for Silicon Labs iostream log routing.
 *
 * The default SDK config enables SI91x iostream-backed stdout prints. On this
 * target that path goes through a buggy `_write()` implementation that can
 * repeatedly emit the same buffer and corrupt VCOM logs. Keep iostream log
 * prints disabled so `printf()` uses the board debug UART path instead.
 ******************************************************************************/
#ifndef SL_SI91X_IOSTREAM_LOG_CONFIG_H
#define SL_SI91X_IOSTREAM_LOG_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>LOG PRINTS settings
// <q SL_SI91X_IOSTREAM_LOG_PRINTS_ENABLE> IOSTREAM LOG ENABLE
// <i> Default: 0
#define SL_SI91X_IOSTREAM_LOG_PRINTS_ENABLE 0

// <o  IOSTREAM_LOG_TYPE> Stream type
// <SL_SI91X_IOSTREAM_SWO_LOG=> SWO
// <SL_SI91X_IOSTREAM_RTT_LOG=> RTT
// <SL_SI91X_IOSTREAM_VUART_LOG=> VUART
// <i> Kept only for compatibility when the SDK includes this config.
#define IOSTREAM_LOG_TYPE SL_SI91X_IOSTREAM_RTT_LOG

// </h>
// <<< end of configuration section >>>

#endif
