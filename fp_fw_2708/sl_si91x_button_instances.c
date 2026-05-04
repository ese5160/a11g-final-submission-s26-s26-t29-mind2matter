#include "sl_si91x_button_instances.h"

const sl_button_t button_btn0 = {
  .pin = SL_BUTTON_BTN0_PIN,
  .port = SL_BUTTON_BTN0_PORT,
  .button_number = SL_BUTTON_BTN0_NUMBER,
  .pad = 0U,
  .interrupt_config = SL_BUTTON_CONFIG_BTN0_INTR,
};

void button_init_instances(void)
{
  sl_si91x_button_init(&button_btn0);
}
