#include "rsi_rom_egpio.h"

// QSPI/PSRAM code in the SDK references the legacy EGPIO symbols directly
// even when the ROM driver path is enabled. Bridge those calls into the ROM API.
void egpio_set_dir(EGPIO_Type *pEGPIO, uint8_t port, uint8_t pin, boolean_t dir)
{
  ROMAPI_EGPIO_API->egpio_set_dir(pEGPIO, port, pin, dir);
}

void egpio_set_pin(EGPIO_Type *pEGPIO, uint8_t port, uint8_t pin, uint8_t val)
{
  ROMAPI_EGPIO_API->egpio_set_pin(pEGPIO, port, pin, val);
}

void egpio_set_pin_mux(EGPIO_Type *pEGPIO, uint8_t port, uint8_t pin, uint8_t mux)
{
  ROMAPI_EGPIO_API->egpio_set_pin_mux(pEGPIO, port, pin, mux);
}
