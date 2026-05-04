#include "m2m_dev_board.h"
#include "m2m_app_config.h"

#include "RTE_Device_917.h"
#include "cmsis_os2.h"
#include "rsi_rom_egpio.h"
#include "sl_si91x_button_instances.h"
#include "sl_si91x_i2c.h"
#include "sl_si91x_i2c_init_i2c2_config.h"
#include "sl_si91x_i2s.h"
#include "sl_si91x_i2s_config.h"
#include "sl_si91x_peripheral_i2c.h"
#include "sl_si91x_usart.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define M2M_AUDIO_I2S_INSTANCE            0U
#define M2M_AUDIO_CAPTURE_CHUNK_SAMPLES   2048U
#define M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES  1024U
#define M2M_AUDIO_PLAYBACK_CHANNEL_COUNT  2U
#define M2M_AUDIO_PLAYBACK_ITEM_COUNT     (M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT)
#define M2M_AUDIO_SAMPLE_RATE             SL_I2S_SAMPLING_RATE_16000
#define M2M_AUDIO_FLAG_TX_DONE            (1UL << 0)
#define M2M_AUDIO_FLAG_RX_DONE            (1UL << 1)
#define M2M_AUDIO_I2S32_TO_PCM16_SHIFT    16U
// Keep capture at unity gain by default; the previous x8 boost clipped normal speech badly.
#define M2M_AUDIO_CAPTURE_SOFTWARE_GAIN   3U
#define M2M_AUDIO_TONE_AMPLITUDE_PCM16    24000
#define M2M_AUDIO_WAV_PLAYBACK_GAIN_NUM   2U
#define M2M_AUDIO_WAV_PLAYBACK_GAIN_DEN   1U
#define M2M_AUDIO_PROMPT_REPEAT_COUNT     8U
#define M2M_AUDIO_SELF_TEST_REPEAT_COUNT  20U
#define M2M_AUDIO_TX_DRAIN_MS             40U
#define M2M_AUDIO_WAV_HEADER_SIZE         44U
#define M2M_AUDIO_SLOT_DETECT_MIN_ENERGY  4096ULL

#define M2M_CAM_UART_INSTANCE     UART_1
#define M2M_CAM_BAUDRATE          115200U
#define M2M_CAM_FLAG_TX_DONE      (1UL << 2)
#define M2M_CAM_FLAG_RX_DONE      (1UL << 3)

#define CAM_CMD_PREFIX            0xAA
#define CAM_CMD_INIT              0x01
#define CAM_CMD_GET_PIC           0x04
#define CAM_CMD_SNAPSHOT          0x05
#define CAM_CMD_SETSIZE           0x06
#define CAM_CMD_DATA              0x0A
#define CAM_CMD_SYNC              0x0D
#define CAM_CMD_ACK               0x0E
#define CAM_CMD_NAK               0x0F
#define CAM_FORMAT_JPEG           0x07
#define CAM_RESOLUTION_640X480    0x07
#define CAM_PIC_TYPE_SNAPSHOT     0x00
#define CAM_PIC_TYPE_CAPTURE      0x01

#define M2M_IMU_I2C_INSTANCE                2U
#define M2M_IMU_I2C_TX_FIFO_THRESHOLD       0U
#define M2M_IMU_I2C_RX_FIFO_THRESHOLD       0U
#define M2M_IMU_I2C_INIT_DELAY_MS           20U
#define M2M_IMU_I2C_RECOVERY_DELAY_MS       100U
#define M2M_IMU_INIT_RETRY_MS               1000U
#define M2M_IMU_ROUTE_SWITCH_DELAY_MS       5U
#define M2M_IMU_LSM6DSOX_ADDRESS_PRIMARY    0x6BU
#define M2M_IMU_LSM6DSOX_ADDRESS_ALT        0x6AU
#define M2M_IMU_LSM6DSOX_WHO_AM_I           0x0FU
#define M2M_IMU_LSM6DSOX_WHO_AM_I_VALUE     0x6CU
#define M2M_IMU_LSM6DSOX_CTRL1_XL           0x10U
#define M2M_IMU_LSM6DSOX_CTRL2_G            0x11U
#define M2M_IMU_LSM6DSOX_CTRL3_C            0x12U
#define M2M_IMU_LSM6DSOX_OUTX_L_G           0x22U
#define M2M_IMU_LSM6DSOX_SAMPLE_BYTES       12U
#define M2M_IMU_ACCEL_MG_NUMERATOR          61L
#define M2M_IMU_ACCEL_MG_DENOMINATOR        1000L
#define M2M_IMU_GYRO_MDPS_NUMERATOR         875L
#define M2M_IMU_GYRO_MDPS_DENOMINATOR       100000L
#define M2M_IMU_FREE_FALL_THRESHOLD_MG      300L
#define M2M_IMU_IMPACT_THRESHOLD_MG         1800L
#define M2M_IMU_STABLE_LOWER_THRESHOLD_MG   850L
#define M2M_IMU_STABLE_UPPER_THRESHOLD_MG   1150L
#define M2M_IMU_RECOVERY_THRESHOLD_MG       1400L
#define M2M_IMU_FREE_FALL_MIN_SAMPLES       3U
#define M2M_IMU_IMPACT_MIN_SAMPLES          2U
#define M2M_IMU_STABLE_MIN_SAMPLES          10U
#define M2M_IMU_FALL_EVENT_LATCH_SAMPLES    1U
#define M2M_IMU_FREE_FALL_TIMEOUT_MS        800U
#define M2M_IMU_POST_IMPACT_WAIT_MS         1000U

static bool debug_led_on;

static osMutexId_t audio_mutex;
static osMutexId_t camera_mutex;
static osMutexId_t imu_mutex;
static osMutexId_t imu_init_mutex;

static sl_i2s_handle_t audio_i2s_handle = NULL;
static bool audio_i2s_initialized;
static osThreadId_t audio_owner_thread;
static int32_t audio_capture_stage_buffer[M2M_AUDIO_CAPTURE_CHUNK_SAMPLES];
static int16_t audio_prompt_buffer_a[M2M_AUDIO_PLAYBACK_ITEM_COUNT];
static int16_t audio_prompt_buffer_b[M2M_AUDIO_PLAYBACK_ITEM_COUNT];

static sl_usart_handle_t camera_uart_handle = NULL;
static bool camera_ready;
static osThreadId_t camera_owner_thread;
static uint8_t camera_tx_buf[6];
static uint8_t camera_rx_buf[6];
static bool imu_ready;
static bool imu_i2c_initialized;
static bool imu_read_failure_logged;
static uint32_t imu_next_init_retry_ms;
static uint32_t imu_init_attempt_count;
static uint8_t imu_i2c_address = M2M_IMU_LSM6DSOX_ADDRESS_PRIMARY;

typedef enum {
  M2M_IMU_STATE_NORMAL = 0,
  M2M_IMU_STATE_FREE_FALL,
  M2M_IMU_STATE_IMPACT,
  M2M_IMU_STATE_FALL_DETECTED,
} m2m_imu_fall_state_t;

typedef struct {
  const char *name;
  uint8_t log_scl_pin;
  uint8_t log_sda_pin;
  sl_i2c_pin_init_t pin_init;
} m2m_imu_i2c_route_t;

static const m2m_imu_i2c_route_t imu_i2c_routes[] = {
  {
    .name = "gpio71_70",
    .log_scl_pin = 71U,
    .log_sda_pin = 70U,
    .pin_init = {
      .sda_port = ULP,
      .sda_pin = 6,
      .sda_mux = SL_SI91X_I2C2_SDA_MUX,
      .sda_pad = 0,
      .scl_port = ULP,
      .scl_pin = 7,
      .scl_mux = SL_SI91X_I2C2_SCL_MUX,
      .scl_pad = 0,
      .instance = M2M_IMU_I2C_INSTANCE,
    },
  },
  {
    .name = "gpio46_47",
    .log_scl_pin = 46U,
    .log_sda_pin = 47U,
    .pin_init = {
      .sda_port = ULP,
      .sda_pin = 47,
      .sda_mux = SL_SI91X_I2C2_SDA_MUX,
      .sda_pad = 11,
      .scl_port = ULP,
      .scl_pin = 46,
      .scl_mux = SL_SI91X_I2C2_SCL_MUX,
      .scl_pad = 10,
      .instance = M2M_IMU_I2C_INSTANCE,
    },
  },
  {
    .name = "gpio15_47",
    .log_scl_pin = 15U,
    .log_sda_pin = 47U,
    .pin_init = {
      .sda_port = ULP,
      .sda_pin = 47,
      .sda_mux = SL_SI91X_I2C2_SDA_MUX,
      .sda_pad = 11,
      .scl_port = ULP,
      .scl_pin = 15,
      .scl_mux = SL_SI91X_I2C2_SCL_MUX,
      .scl_pad = 8,
      .instance = M2M_IMU_I2C_INSTANCE,
    },
  },
};

static const m2m_imu_i2c_route_t *imu_active_route;

static void camera_abort_receive(const char *reason)
{
  int32_t arm_status;

  if (camera_uart_handle == NULL) {
    return;
  }

  arm_status = ((sl_usart_driver_t *)camera_uart_handle)->Control(SL_USART_ABORT_RECEIVE, 0U);
  (void)osThreadFlagsClear(M2M_CAM_FLAG_RX_DONE);

  if (arm_status != 0) {
    printf("[m2m-devboard] camera RX abort failed reason=%s arm_status=%ld\r\n",
           (reason == NULL) ? "unknown" : reason,
           (long)arm_status);
  }
}

static void configure_hp_gpio_output(uint8_t pin, uint8_t pad)
{
  RSI_EGPIO_PadSelectionEnable(pad);
  RSI_EGPIO_PadReceiverDisable(pin);
  RSI_EGPIO_SetPinMux(EGPIO, GPIO_PORT_0, pin, EGPIO_PIN_MUX_MODE0);
  RSI_EGPIO_SetDir(EGPIO, GPIO_PORT_0, pin, EGPIO_CONFIG_DIR_OUTPUT);
}

static void set_hp_gpio_output(uint8_t pin, bool high)
{
  RSI_EGPIO_SetPin(EGPIO, GPIO_PORT_0, pin, high ? 1U : 0U);
}

static uint32_t board_now_ms(void)
{
  uint32_t freq = osKernelGetTickFreq();
  uint32_t tick = osKernelGetTickCount();

  if (freq == 0U) {
    return tick;
  }

  return (uint32_t)(((uint64_t)tick * 1000ULL) / freq);
}

static void imu_schedule_retry(uint32_t delay_ms)
{
  imu_ready = false;
  imu_next_init_retry_ms = board_now_ms() + delay_ms;
}

static void imu_log_transition(const char *event, const m2m_imu_sample_t *sample)
{
  if ((event == NULL) || (sample == NULL)) {
    return;
  }

  printf("[imu] %s accel_mg=[%d,%d,%d] gyro_dps=[%d,%d,%d]\r\n",
         event,
         sample->accel_mg[0],
         sample->accel_mg[1],
         sample->accel_mg[2],
         sample->gyro_dps[0],
         sample->gyro_dps[1],
         sample->gyro_dps[2]);
}

static const char *imu_route_name(const m2m_imu_i2c_route_t *route)
{
  return (route == NULL) ? "unknown" : route->name;
}

static void fill_idle_imu_sample(m2m_imu_sample_t *sample)
{
  if (sample == NULL) {
    return;
  }

  sample->accel_mg[0] = 0;
  sample->accel_mg[1] = 0;
  sample->accel_mg[2] = 1000;
  sample->gyro_dps[0] = 0;
  sample->gyro_dps[1] = 0;
  sample->gyro_dps[2] = 0;
}

static int16_t imu_raw_accel_to_mg(int16_t raw)
{
  return (int16_t)(((int32_t)raw * M2M_IMU_ACCEL_MG_NUMERATOR) / M2M_IMU_ACCEL_MG_DENOMINATOR);
}

static int16_t imu_raw_gyro_to_dps(int16_t raw)
{
  return (int16_t)(((int32_t)raw * M2M_IMU_GYRO_MDPS_NUMERATOR) / M2M_IMU_GYRO_MDPS_DENOMINATOR);
}

static void imu_decode_sample(m2m_imu_sample_t *sample, const uint8_t *sample_buffer)
{
  if ((sample == NULL) || (sample_buffer == NULL)) {
    return;
  }

  sample->gyro_dps[0] = imu_raw_gyro_to_dps((int16_t)((sample_buffer[1] << 8) | sample_buffer[0]));
  sample->gyro_dps[1] = imu_raw_gyro_to_dps((int16_t)((sample_buffer[3] << 8) | sample_buffer[2]));
  sample->gyro_dps[2] = imu_raw_gyro_to_dps((int16_t)((sample_buffer[5] << 8) | sample_buffer[4]));
  sample->accel_mg[0] = imu_raw_accel_to_mg((int16_t)((sample_buffer[7] << 8) | sample_buffer[6]));
  sample->accel_mg[1] = imu_raw_accel_to_mg((int16_t)((sample_buffer[9] << 8) | sample_buffer[8]));
  sample->accel_mg[2] = imu_raw_accel_to_mg((int16_t)((sample_buffer[11] << 8) | sample_buffer[10]));
}

static bool imu_write_reg_addr(uint8_t i2c_address, uint8_t reg_addr, uint8_t value)
{
  sl_i2c_status_t status;
  uint8_t tx_buffer[2] = { reg_addr, value };

  if (imu_mutex == NULL) {
    return false;
  }

  (void)osMutexAcquire(imu_mutex, osWaitForever);
  status = sl_i2c_driver_send_data_blocking(M2M_IMU_I2C_INSTANCE,
                                            i2c_address,
                                            tx_buffer,
                                            sizeof(tx_buffer));
  (void)osMutexRelease(imu_mutex);

  return status == SL_I2C_SUCCESS;
}

static bool imu_write_reg(uint8_t reg_addr, uint8_t value)
{
  return imu_write_reg_addr(imu_i2c_address, reg_addr, value);
}

static bool imu_read_regs_addr(uint8_t i2c_address, uint8_t reg_addr, uint8_t *buffer, uint16_t length)
{
  sl_i2c_status_t status;
  sl_i2c_transfer_config_t transfer_config = {
    .tx_buffer = &reg_addr,
    .tx_len = 1,
    .rx_buffer = buffer,
    .rx_len = length,
  };

  if ((imu_mutex == NULL) || (buffer == NULL) || (length == 0U)) {
    return false;
  }

  (void)osMutexAcquire(imu_mutex, osWaitForever);
  status = sl_i2c_driver_transfer_data(M2M_IMU_I2C_INSTANCE,
                                       &transfer_config,
                                       i2c_address);
  (void)osMutexRelease(imu_mutex);

  return status == SL_I2C_SUCCESS;
}

static bool imu_read_regs(uint8_t reg_addr, uint8_t *buffer, uint16_t length)
{
  return imu_read_regs_addr(imu_i2c_address, reg_addr, buffer, length);
}

static bool imu_read_sample(m2m_imu_sample_t *sample)
{
  uint8_t sample_buffer[M2M_IMU_LSM6DSOX_SAMPLE_BYTES];

  if (sample == NULL) {
    return false;
  }

  if (!imu_read_regs(M2M_IMU_LSM6DSOX_OUTX_L_G, sample_buffer, sizeof(sample_buffer))) {
    return false;
  }

  imu_decode_sample(sample, sample_buffer);
  return true;
}

static bool imu_probe_route_and_address(const m2m_imu_i2c_route_t *route,
                                        uint8_t i2c_address,
                                        uint8_t *who_am_i)
{
  uint8_t local_who_am_i = 0U;
  uint8_t *who_am_i_target = (who_am_i != NULL) ? who_am_i : &local_who_am_i;

  if (route == NULL) {
    return false;
  }

  sl_si91x_i2c_pin_init((sl_i2c_pin_init_t *)&route->pin_init);
  osDelay(M2M_IMU_ROUTE_SWITCH_DELAY_MS);

  if (!imu_read_regs_addr(i2c_address, M2M_IMU_LSM6DSOX_WHO_AM_I, who_am_i_target, 1U)) {
    printf("[m2m-devboard] IMU probe read failed route=%s addr=0x%02X\r\n",
           imu_route_name(route),
           i2c_address);
    return false;
  }

  printf("[m2m-devboard] IMU probe route=%s addr=0x%02X who_am_i=0x%02X\r\n",
         imu_route_name(route),
         i2c_address,
         *who_am_i_target);
  return *who_am_i_target == M2M_IMU_LSM6DSOX_WHO_AM_I_VALUE;
}

static bool imu_hw_init(void)
{
  sl_i2c_status_t i2c_status;
  bool imu_found = false;
  uint8_t who_am_i = 0U;
  uint32_t now_ms = board_now_ms();
  uint32_t attempt_number;
  sl_i2c_config_t i2c_config = {
    .mode = SL_I2C_I2C2_MODE,
    .operating_mode = SL_I2C_I2C2_OPERATING_MODE,
    .transfer_type = SL_I2C_I2C2_TRANSFER_TYPE,
    .i2c_callback = NULL,
  };
  size_t route_index;

  if (imu_ready) {
    return true;
  }

  if ((imu_next_init_retry_ms != 0U) && ((int32_t)(now_ms - imu_next_init_retry_ms) < 0)) {
    return false;
  }

  if (imu_init_mutex == NULL) {
    imu_init_mutex = osMutexNew(NULL);
  }
  if (imu_init_mutex == NULL) {
    printf("[m2m-devboard] IMU init mutex creation failed\r\n");
    return false;
  }
  (void)osMutexAcquire(imu_init_mutex, osWaitForever);

  if (imu_ready) {
    (void)osMutexRelease(imu_init_mutex);
    return true;
  }

  now_ms = board_now_ms();
  if ((imu_next_init_retry_ms != 0U) && ((int32_t)(now_ms - imu_next_init_retry_ms) < 0)) {
    (void)osMutexRelease(imu_init_mutex);
    return false;
  }

  attempt_number = ++imu_init_attempt_count;
  printf("[m2m-devboard] IMU init attempt=%lu\r\n", (unsigned long)attempt_number);

  if (imu_mutex == NULL) {
    imu_mutex = osMutexNew(NULL);
  }
  if (imu_mutex == NULL) {
    printf("[m2m-devboard] IMU mutex creation failed\r\n");
    (void)osMutexRelease(imu_init_mutex);
    return false;
  }

  if (!imu_i2c_initialized) {
    sl_si91x_i2c_pin_init((sl_i2c_pin_init_t *)&imu_i2c_routes[0].pin_init);
    i2c_status = sl_i2c_driver_init(M2M_IMU_I2C_INSTANCE, &i2c_config);
    if (i2c_status != SL_I2C_SUCCESS) {
      printf("[m2m-devboard] IMU I2C init failed attempt=%lu status=0x%lx retry_in_ms=%u\r\n",
             (unsigned long)attempt_number,
             (unsigned long)i2c_status,
             (unsigned int)M2M_IMU_INIT_RETRY_MS);
      imu_schedule_retry(M2M_IMU_INIT_RETRY_MS);
      (void)osMutexRelease(imu_init_mutex);
      return false;
    }

    sl_i2c_driver_configure_fifo_threshold(M2M_IMU_I2C_INSTANCE,
                                           M2M_IMU_I2C_TX_FIFO_THRESHOLD,
                                           M2M_IMU_I2C_RX_FIFO_THRESHOLD);
    sl_i2c_driver_enable_repeated_start(M2M_IMU_I2C_INSTANCE, true);
    imu_i2c_initialized = true;
    printf("[m2m-devboard] IMU I2C bus ready instance=%u route_probe_count=%lu\r\n",
           (unsigned int)M2M_IMU_I2C_INSTANCE,
           (unsigned long)(sizeof(imu_i2c_routes) / sizeof(imu_i2c_routes[0])));
  }

  osDelay(M2M_IMU_I2C_INIT_DELAY_MS);

  for (route_index = 0U; route_index < (sizeof(imu_i2c_routes) / sizeof(imu_i2c_routes[0])); route_index++) {
    const m2m_imu_i2c_route_t *route = &imu_i2c_routes[route_index];

    if (imu_probe_route_and_address(route, M2M_IMU_LSM6DSOX_ADDRESS_PRIMARY, &who_am_i)) {
      imu_active_route = route;
      imu_i2c_address = M2M_IMU_LSM6DSOX_ADDRESS_PRIMARY;
      imu_found = true;
      break;
    }
    if (imu_probe_route_and_address(route, M2M_IMU_LSM6DSOX_ADDRESS_ALT, &who_am_i)) {
      imu_active_route = route;
      imu_i2c_address = M2M_IMU_LSM6DSOX_ADDRESS_ALT;
      imu_found = true;
      break;
    }
  }

  if (!imu_found) {
    printf("[m2m-devboard] IMU probe exhausted attempt=%lu expected_who_am_i=0x%02X retry_in_ms=%u\r\n",
           (unsigned long)attempt_number,
           M2M_IMU_LSM6DSOX_WHO_AM_I_VALUE,
           (unsigned int)M2M_IMU_INIT_RETRY_MS);
    imu_schedule_retry(M2M_IMU_INIT_RETRY_MS);
    (void)osMutexRelease(imu_init_mutex);
    return false;
  }

  if (!imu_write_reg(M2M_IMU_LSM6DSOX_CTRL1_XL, 0x60U)
      || !imu_write_reg(M2M_IMU_LSM6DSOX_CTRL2_G, 0x60U)
      || !imu_write_reg(M2M_IMU_LSM6DSOX_CTRL3_C, 0x44U)) {
    printf("[m2m-devboard] IMU register init failed attempt=%lu route=%s addr=0x%02X retry_in_ms=%u\r\n",
           (unsigned long)attempt_number,
           imu_route_name(imu_active_route),
           imu_i2c_address,
           (unsigned int)M2M_IMU_INIT_RETRY_MS);
    imu_schedule_retry(M2M_IMU_INIT_RETRY_MS);
    (void)osMutexRelease(imu_init_mutex);
    return false;
  }

  osDelay(M2M_IMU_I2C_RECOVERY_DELAY_MS);
  imu_ready = true;
  imu_next_init_retry_ms = 0U;
  imu_init_attempt_count = 0U;
  imu_read_failure_logged = false;
  printf("[m2m-devboard] IMU online route=%s scl=GPIO%u sda=GPIO%u addr=0x%02X\r\n",
         imu_route_name(imu_active_route),
         (unsigned int)((imu_active_route == NULL) ? 0U : imu_active_route->log_scl_pin),
         (unsigned int)((imu_active_route == NULL) ? 0U : imu_active_route->log_sda_pin),
         imu_i2c_address);
  (void)osMutexRelease(imu_init_mutex);
  return true;
}

static void audio_event_callback(uint32_t event)
{
  if ((event & SL_I2S_RECEIVE_COMPLETE) != 0U) {
    if (audio_owner_thread != NULL) {
      (void)osThreadFlagsSet(audio_owner_thread, M2M_AUDIO_FLAG_RX_DONE);
    }
  }

  if ((event & SL_I2S_SEND_COMPLETE) != 0U) {
    if (audio_owner_thread != NULL) {
      (void)osThreadFlagsSet(audio_owner_thread, M2M_AUDIO_FLAG_TX_DONE);
    }
  }
}

static bool audio_hw_init(void)
{
  sl_status_t status;

  if (audio_i2s_initialized) {
    return true;
  }

  status = sl_si91x_i2s_init(M2M_AUDIO_I2S_INSTANCE, &audio_i2s_handle);
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] I2S init failed: 0x%lx\r\n", (unsigned long)status);
    return false;
  }

  status = sl_si91x_i2s_register_event_callback(audio_i2s_handle, audio_event_callback);
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] I2S callback registration failed: 0x%lx\r\n", (unsigned long)status);
    return false;
  }

  audio_i2s_initialized = true;
  return true;
}

static int16_t i2s32_to_pcm16(int32_t sample)
{
  int32_t shifted = sample >> M2M_AUDIO_I2S32_TO_PCM16_SHIFT;

  shifted *= (int32_t)M2M_AUDIO_CAPTURE_SOFTWARE_GAIN;

  if (shifted > INT16_MAX) {
    shifted = INT16_MAX;
  } else if (shifted < INT16_MIN) {
    shifted = INT16_MIN;
  }

  return (int16_t)shifted;
}

typedef enum {
  M2M_AUDIO_CAPTURE_LAYOUT_UNKNOWN = 0,
  M2M_AUDIO_CAPTURE_LAYOUT_SLOT0,
  M2M_AUDIO_CAPTURE_LAYOUT_SLOT1,
  M2M_AUDIO_CAPTURE_LAYOUT_AVERAGE,
} m2m_audio_capture_layout_t;

static const char *audio_capture_layout_name(m2m_audio_capture_layout_t layout)
{
  switch (layout) {
    case M2M_AUDIO_CAPTURE_LAYOUT_SLOT0:
      return "slot0";
    case M2M_AUDIO_CAPTURE_LAYOUT_SLOT1:
      return "slot1";
    case M2M_AUDIO_CAPTURE_LAYOUT_AVERAGE:
      return "average";
    default:
      return "unknown";
  }
}

static size_t copy_capture_chunk_to_pcm(int16_t *pcm_buffer,
                                        size_t pcm_offset,
                                        size_t pcm_capacity,
                                        const int32_t *capture_buffer,
                                        m2m_audio_capture_layout_t *layout)
{
  size_t remaining_capacity;
  size_t pair_count;
  uint64_t slot0_energy = 0ULL;
  uint64_t slot1_energy = 0ULL;

  if ((pcm_buffer == NULL) || (capture_buffer == NULL) || (layout == NULL) || (pcm_offset >= pcm_capacity)) {
    return 0U;
  }

  remaining_capacity = pcm_capacity - pcm_offset;
  pair_count = M2M_AUDIO_CAPTURE_CHUNK_SAMPLES / 2U;
  if (pair_count > remaining_capacity) {
    pair_count = remaining_capacity;
  }

  if (*layout == M2M_AUDIO_CAPTURE_LAYOUT_UNKNOWN) {
    for (size_t i = 0; i < pair_count; i++) {
      int32_t slot0 = (int32_t)i2s32_to_pcm16(capture_buffer[i * 2U]);
      int32_t slot1 = (int32_t)i2s32_to_pcm16(capture_buffer[(i * 2U) + 1U]);
      slot0_energy += (uint64_t)((slot0 < 0) ? -slot0 : slot0);
      slot1_energy += (uint64_t)((slot1 < 0) ? -slot1 : slot1);
    }

    if ((slot0_energy + slot1_energy) >= M2M_AUDIO_SLOT_DETECT_MIN_ENERGY) {
      if (slot0_energy > (slot1_energy * 2ULL)) {
        *layout = M2M_AUDIO_CAPTURE_LAYOUT_SLOT0;
      } else if (slot1_energy > (slot0_energy * 2ULL)) {
        *layout = M2M_AUDIO_CAPTURE_LAYOUT_SLOT1;
      } else {
        *layout = M2M_AUDIO_CAPTURE_LAYOUT_AVERAGE;
      }

      printf("[m2m-devboard] mic slot detect slot0_energy=%lu slot1_energy=%lu selected=%s\r\n",
             (unsigned long)slot0_energy,
             (unsigned long)slot1_energy,
             audio_capture_layout_name(*layout));
    }
  }

  for (size_t i = 0; i < pair_count; i++) {
    int32_t slot0 = (int32_t)i2s32_to_pcm16(capture_buffer[i * 2U]);
    int32_t slot1 = (int32_t)i2s32_to_pcm16(capture_buffer[(i * 2U) + 1U]);
    int32_t mixed = slot0;

    switch (*layout) {
      case M2M_AUDIO_CAPTURE_LAYOUT_SLOT1:
        mixed = slot1;
        break;
      case M2M_AUDIO_CAPTURE_LAYOUT_AVERAGE:
        mixed = (slot0 + slot1) / 2;
        break;
      case M2M_AUDIO_CAPTURE_LAYOUT_SLOT0:
      case M2M_AUDIO_CAPTURE_LAYOUT_UNKNOWN:
      default:
        mixed = slot0;
        break;
    }

    if (mixed > INT16_MAX) {
      mixed = INT16_MAX;
    } else if (mixed < INT16_MIN) {
      mixed = INT16_MIN;
    }

    pcm_buffer[pcm_offset + i] = (int16_t)mixed;
  }

  return pair_count;
}

static void fill_square_wave(int16_t *buffer,
                             size_t start_index,
                             size_t end_index,
                             uint32_t half_period,
                             int16_t amplitude)
{
  if ((buffer == NULL) || (half_period == 0U) || (start_index >= end_index)) {
    return;
  }

  for (size_t i = start_index; i < end_index; i++) {
    size_t local_index = i - start_index;
    int16_t sample = (((local_index / half_period) & 1U) == 0U) ? amplitude : (int16_t)-amplitude;
    size_t sample_index = i * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT;
    buffer[sample_index] = sample;
    buffer[sample_index + 1U] = sample;
  }
}

static int16_t scale_cloud_wav_sample(int32_t sample)
{
  int32_t scaled = (sample * (int32_t)M2M_AUDIO_WAV_PLAYBACK_GAIN_NUM)
                   / (int32_t)M2M_AUDIO_WAV_PLAYBACK_GAIN_DEN;

  if (scaled > INT16_MAX) {
    scaled = INT16_MAX;
  } else if (scaled < INT16_MIN) {
    scaled = INT16_MIN;
  }

  return (int16_t)scaled;
}

static bool prompt_detail_is(const char *detail, const char *expected)
{
  return (detail != NULL) && (expected != NULL) && (strcmp(detail, expected) == 0);
}

static bool prompt_detail_contains(const char *detail, const char *needle)
{
  return (detail != NULL) && (needle != NULL) && (strstr(detail, needle) != NULL);
}

static void fill_prompt_buffer(int16_t *buffer,
                               size_t sample_count,
                               const char *prompt_tag,
                               const char *detail)
{
  if (buffer == NULL) {
    return;
  }

  memset(buffer, 0, sample_count * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT * sizeof(*buffer));

  if ((prompt_tag != NULL) && (strcmp(prompt_tag, "speaker_self_test") == 0)) {
    size_t quarter = sample_count / 4U;
    size_t midpoint = sample_count / 2U;
    size_t third = (sample_count * 3U) / 4U;
    fill_square_wave(buffer, 0U, quarter, 10U, INT16_MAX);
    fill_square_wave(buffer, quarter, midpoint, 6U, INT16_MAX);
    fill_square_wave(buffer, midpoint, third, 14U, INT16_MAX);
    fill_square_wave(buffer, third, sample_count, 8U, INT16_MAX);
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "voice_start") == 0)) {
    size_t split = sample_count / 2U;
    fill_square_wave(buffer, 0U, split, 20U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, split, sample_count, 11U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "vision_start") == 0)) {
    size_t first_end = sample_count / 3U;
    size_t second_start = sample_count / 2U;
    fill_square_wave(buffer, 0U, first_end, 18U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, second_start, sample_count, 9U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "record_stop_upload") == 0)) {
    size_t split = sample_count / 2U;
    fill_square_wave(buffer, 0U, split, 8U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, split, sample_count, 22U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "fall_confirm") == 0)) {
    fill_square_wave(buffer, 0U, sample_count, 8U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "ota_status") == 0)) {
    if (prompt_detail_contains(detail, "failed")) {
      size_t midpoint = sample_count / 2U;
      fill_square_wave(buffer, 0U, midpoint, 30U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
      fill_square_wave(buffer, midpoint, sample_count, 44U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    } else {
      fill_square_wave(buffer, 0U, sample_count, 24U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    }
  } else if ((prompt_tag != NULL) && (strcmp(prompt_tag, "voice_request_sent") == 0)) {
    size_t half = sample_count / 2U;
    fill_square_wave(buffer, 0U, half, 12U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, half, sample_count, 8U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if (prompt_detail_is(detail, "system_ready")) {
    size_t third = sample_count / 3U;
    fill_square_wave(buffer, 0U, third, 18U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, third, third * 2U, 12U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, third * 2U, sample_count, 8U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if (prompt_detail_is(detail, "voice_busy")) {
    size_t half = sample_count / 2U;
    fill_square_wave(buffer, 0U, half, 7U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, half, sample_count, 7U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else if (prompt_detail_contains(detail, "failed")
             || prompt_detail_is(detail, "wifi_failed")
             || prompt_detail_is(detail, "mqtt_failed")
             || prompt_detail_is(detail, "cloud_offline")) {
    size_t midpoint = sample_count / 2U;
    fill_square_wave(buffer, 0U, midpoint, 34U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
    fill_square_wave(buffer, midpoint, sample_count, 48U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  } else {
    fill_square_wave(buffer, 0U, sample_count, 16U, M2M_AUDIO_TONE_AMPLITUDE_PCM16);
  }
}

static void log_audio_playback_status(const char *tag, uint32_t requested_items)
{
  uint32_t tx_count = 0U;
  sl_i2s_status_t status = { 0 };

  if (audio_i2s_handle != NULL) {
    tx_count = sl_si91x_i2s_get_transmit_data_count(audio_i2s_handle);
    status = sl_si91x_i2s_get_status(audio_i2s_handle);
  }

  printf("[m2m-devboard] audio playback status tag=%s requested_items=%lu tx_count=%lu tx_busy=%u tx_underflow=%u frame_error=%u\r\n",
         (tag == NULL) ? "unknown" : tag,
         (unsigned long)requested_items,
         (unsigned long)tx_count,
         (unsigned int)status.tx_busy,
         (unsigned int)status.tx_underflow,
         (unsigned int)status.frame_error);
}

static bool play_prompt_internal(const char *prompt_tag, const char *detail, uint32_t repeat_count)
{
  sl_status_t status;
  uint32_t flags;
  uint32_t prompt_duration_ms;
  uint32_t transmitted_items = 0U;
  sl_i2s_xfer_config_t speaker_config = {
    .mode = SL_I2S_MASTER,
    .protocol = SL_I2S_PROTOCOL,
    .sync = SL_I2S_ASYNC,
    .resolution = SL_I2S_RESOLUTION_16,
    .data_size = SL_I2S_DATA_SIZE16,
    .transfer_type = SL_I2S_TRANSMIT,
    .sampling_rate = M2M_AUDIO_SAMPLE_RATE,
  };

  if ((audio_mutex == NULL) || (osMutexAcquire(audio_mutex, osWaitForever) != osOK)) {
    printf("[m2m-devboard] prompt busy tag=%s detail=%s\r\n",
           (prompt_tag == NULL) ? "unknown" : prompt_tag,
           (detail == NULL) ? "" : detail);
    return false;
  }

  if (!audio_hw_init()) {
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  fill_prompt_buffer(audio_prompt_buffer_a, M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES, prompt_tag, detail);
  fill_prompt_buffer(audio_prompt_buffer_b, M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES, prompt_tag, detail);
  audio_owner_thread = osThreadGetId();
  (void)osThreadFlagsClear(M2M_AUDIO_FLAG_TX_DONE | M2M_AUDIO_FLAG_RX_DONE);
  prompt_duration_ms = (uint32_t)((M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES * repeat_count * 1000U)
                                  / M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ);

  status = sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_FULL_POWER);
  if (status == SL_STATUS_OK) {
    status = sl_si91x_i2s_config_transmit_receive(audio_i2s_handle, &speaker_config);
  }
  for (uint32_t chunk = 0U; (status == SL_STATUS_OK) && (chunk < repeat_count); ++chunk) {
    int16_t *active_buffer = ((chunk & 1U) == 0U) ? audio_prompt_buffer_a : audio_prompt_buffer_b;

    (void)osThreadFlagsClear(M2M_AUDIO_FLAG_TX_DONE);
    status = sl_si91x_i2s_transmit_data(audio_i2s_handle, active_buffer, M2M_AUDIO_PLAYBACK_ITEM_COUNT);
    if (status != SL_STATUS_OK) {
      break;
    }

    transmitted_items += M2M_AUDIO_PLAYBACK_ITEM_COUNT;
    flags = osThreadFlagsWait(M2M_AUDIO_FLAG_TX_DONE, osFlagsWaitAny, 500U);
    if ((flags == (uint32_t)osErrorTimeout) || ((flags & osFlagsError) != 0U)) {
      status = SL_STATUS_TIMEOUT;
    }
  }

  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] prompt playback failed tag=%s status=0x%lx\r\n",
           (prompt_tag == NULL) ? "unknown" : prompt_tag,
           (unsigned long)status);
    log_audio_playback_status(prompt_tag, transmitted_items);
    audio_owner_thread = NULL;
    (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_SEND_ABORT);
    (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  osDelay(M2M_AUDIO_TX_DRAIN_MS);
  log_audio_playback_status(prompt_tag, transmitted_items);
  (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_SEND_ABORT);
  (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
  audio_owner_thread = NULL;
  (void)osMutexRelease(audio_mutex);

  printf("[m2m-devboard] prompt=%s detail=%s duration_ms=%lu\r\n",
         (prompt_tag == NULL) ? "unknown" : prompt_tag,
         (detail == NULL) ? "" : detail,
         (unsigned long)prompt_duration_ms);
  return true;
}

static uint16_t read_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int32_t read_wav_mono_sample(const uint8_t *frame_ptr, uint16_t num_channels)
{
  int32_t sample = (int16_t)read_le16(frame_ptr);

  if (num_channels == 2U) {
    sample += (int16_t)read_le16(frame_ptr + 2);
    sample /= 2;
  }

  return sample;
}

static size_t capture_timeout_ms_for_chunk(size_t sample_count)
{
  uint32_t capture_ms = (uint32_t)((sample_count * 1000U) / M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ);
  return (size_t)(capture_ms + 250U);
}

static bool map_audio_sample_rate(uint32_t sample_rate_hz, sl_i2s_sampling_rate_t *i2s_rate)
{
  if (i2s_rate == NULL) {
    return false;
  }

  switch (sample_rate_hz) {
    case 8000U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_8000;
      return true;
    case 11025U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_11025;
      return true;
    case 16000U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_16000;
      return true;
    case 22050U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_22050;
      return true;
    case 24000U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_24000;
      return true;
    case 32000U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_32000;
      return true;
    case 44100U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_44100;
      return true;
    case 48000U:
      *i2s_rate = SL_I2S_SAMPLING_RATE_48000;
      return true;
    default:
      return false;
  }
}

static bool parse_wav_payload(const uint8_t *wav_data,
                              size_t wav_size,
                              uint16_t *num_channels,
                              uint16_t *bits_per_sample,
                              uint32_t *sample_rate_hz,
                              const uint8_t **pcm_data,
                              size_t *pcm_size)
{
  size_t offset = 12U;
  bool fmt_found = false;
  bool data_found = false;
  uint16_t audio_format = 0U;

  if ((wav_data == NULL) || (wav_size < M2M_AUDIO_WAV_HEADER_SIZE) || (num_channels == NULL)
      || (bits_per_sample == NULL) || (sample_rate_hz == NULL) || (pcm_data == NULL) || (pcm_size == NULL)) {
    return false;
  }

  if ((memcmp(wav_data, "RIFF", 4) != 0) || (memcmp(wav_data + 8, "WAVE", 4) != 0)) {
    return false;
  }

  while ((offset + 8U) <= wav_size) {
    const uint8_t *chunk = wav_data + offset;
    uint32_t chunk_size = read_le32(chunk + 4);
    size_t payload_offset = offset + 8U;
    size_t remaining_payload = wav_size - payload_offset;
    size_t bounded_chunk_size = (size_t)chunk_size;
    size_t next_offset;

    if (chunk_size > remaining_payload) {
      if (memcmp(chunk, "data", 4) != 0) {
        return false;
      }

      bounded_chunk_size = remaining_payload;
      next_offset = wav_size;
      printf("[m2m-devboard] WAV data chunk header=%lu exceeds payload=%lu, clamping\r\n",
             (unsigned long)chunk_size,
             (unsigned long)remaining_payload);
    } else {
      next_offset = payload_offset + bounded_chunk_size + (bounded_chunk_size & 1U);
      if ((next_offset < payload_offset) || (next_offset > wav_size)) {
        return false;
      }
    }

    if ((memcmp(chunk, "fmt ", 4) == 0) && (bounded_chunk_size >= 16U)) {
      audio_format = read_le16(chunk + 8);
      *num_channels = read_le16(chunk + 10);
      *sample_rate_hz = read_le32(chunk + 12);
      *bits_per_sample = read_le16(chunk + 22);
      fmt_found = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      *pcm_data = chunk + 8;
      *pcm_size = bounded_chunk_size;
      data_found = true;
    }

    if (fmt_found && data_found) {
      break;
    }

    offset = next_offset;
  }

  return fmt_found && data_found && (audio_format == 1U) && (*num_channels >= 1U) && (*num_channels <= 2U)
         && (*bits_per_sample == 16U);
}

static void camera_uart_callback_event(uint32_t event)
{
  if ((event == SL_USART_EVENT_SEND_COMPLETE) && (camera_owner_thread != NULL)) {
    (void)osThreadFlagsSet(camera_owner_thread, M2M_CAM_FLAG_TX_DONE);
  } else if ((event == SL_USART_EVENT_RECEIVE_COMPLETE) && (camera_owner_thread != NULL)) {
    (void)osThreadFlagsSet(camera_owner_thread, M2M_CAM_FLAG_RX_DONE);
  }
}

static void camera_set_reset(bool high)
{
  set_hp_gpio_output((uint8_t)RTE_GPIO_12_PIN, high);
}

static sl_status_t camera_send_command(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4)
{
  sl_status_t status;

  if ((camera_owner_thread = osThreadGetId()) == NULL) {
    return SL_STATUS_FAIL;
  }

  camera_tx_buf[0] = CAM_CMD_PREFIX;
  camera_tx_buf[1] = cmd;
  camera_tx_buf[2] = p1;
  camera_tx_buf[3] = p2;
  camera_tx_buf[4] = p3;
  camera_tx_buf[5] = p4;

  (void)osThreadFlagsClear(M2M_CAM_FLAG_TX_DONE);
  status = sl_si91x_usart_send_data(camera_uart_handle, camera_tx_buf, sizeof(camera_tx_buf));
  if (status != SL_STATUS_OK) {
    return status;
  }

  return (osThreadFlagsWait(M2M_CAM_FLAG_TX_DONE, osFlagsWaitAny, 1000U) & osFlagsError) == 0U
           ? SL_STATUS_OK
           : SL_STATUS_TIMEOUT;
}

static sl_status_t camera_receive_bytes(uint8_t *buffer, uint32_t length, uint32_t timeout_ms)
{
  sl_status_t status;
  uint32_t flags;
  uint8_t attempt;

  if ((buffer == NULL) || (length == 0U)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  if ((camera_owner_thread = osThreadGetId()) == NULL) {
    return SL_STATUS_FAIL;
  }

  for (attempt = 0U; attempt < 2U; ++attempt) {
    (void)osThreadFlagsClear(M2M_CAM_FLAG_RX_DONE);
    status = sl_si91x_usart_receive_data(camera_uart_handle, buffer, length);
    if (status == SL_STATUS_BUSY) {
      camera_abort_receive("rx_busy");
      osDelay(2U);
      continue;
    }
    if (status != SL_STATUS_OK) {
      return status;
    }

    flags = osThreadFlagsWait(M2M_CAM_FLAG_RX_DONE, osFlagsWaitAny, timeout_ms);
    if (flags == (uint32_t)osErrorTimeout) {
      camera_abort_receive("rx_timeout");
      return SL_STATUS_TIMEOUT;
    }
    if ((flags & osFlagsError) != 0U) {
      camera_abort_receive("rx_flag_error");
      return SL_STATUS_FAIL;
    }

    return SL_STATUS_OK;
  }

  return SL_STATUS_BUSY;
}

static sl_status_t camera_wait_for_specific_ack(uint8_t cmd_id, uint32_t timeout_ms)
{
  uint8_t response[6];
  sl_status_t status = camera_receive_bytes(response, sizeof(response), timeout_ms);

  if (status != SL_STATUS_OK) {
    return status;
  }

  if ((response[0] == CAM_CMD_PREFIX) && (response[1] == CAM_CMD_ACK) && (response[2] == cmd_id)) {
    return SL_STATUS_OK;
  }

  if ((response[0] == CAM_CMD_PREFIX) && (response[1] == CAM_CMD_NAK)) {
    printf("[m2m-devboard] camera NAK for command 0x%02x (error 0x%02x)\r\n",
           cmd_id,
           response[4]);
  }

  return SL_STATUS_FAIL;
}

static sl_status_t camera_sync(int max_retries,
                               uint32_t stabilize_ms,
                               uint32_t start_delay_ms,
                               uint32_t delay_increment_ms)
{
  uint8_t response[6];
  uint32_t delay_ms = start_delay_ms;

  for (int attempt = 0; attempt < max_retries; attempt++) {
    sl_status_t status;

    if ((attempt == 0) || (((attempt + 1) % 10) == 0)) {
      printf("[m2m-devboard] camera sync attempt=%d/%d delay_ms=%lu\r\n",
             attempt + 1,
             max_retries,
             (unsigned long)delay_ms);
    }

    status = camera_send_command(CAM_CMD_SYNC, 0x00, 0x00, 0x00, 0x00);
    if (status != SL_STATUS_OK) {
      if ((attempt == 0) || (((attempt + 1) % 10) == 0)) {
        printf("[m2m-devboard] camera sync TX failed status=0x%lx\r\n", (unsigned long)status);
      }
      osDelay(delay_ms);
      delay_ms += delay_increment_ms;
      continue;
    }

    status = camera_wait_for_specific_ack(CAM_CMD_SYNC, 100U);
    if (status != SL_STATUS_OK) {
      if ((attempt == 0) || (((attempt + 1) % 10) == 0)) {
        printf("[m2m-devboard] camera sync ACK wait failed status=0x%lx\r\n", (unsigned long)status);
      }
      osDelay(delay_ms);
      delay_ms += delay_increment_ms;
      continue;
    }

    status = camera_receive_bytes(response, sizeof(response), 100U);
    if ((status == SL_STATUS_OK)
        && (response[0] == CAM_CMD_PREFIX)
        && (response[1] == CAM_CMD_SYNC)) {
      if (camera_send_command(CAM_CMD_ACK, CAM_CMD_SYNC, 0x00, 0x00, 0x00) == SL_STATUS_OK) {
        osDelay(stabilize_ms);
        printf("[m2m-devboard] camera sync ok attempt=%d/%d\r\n", attempt + 1, max_retries);
        return SL_STATUS_OK;
      }
    } else if ((attempt == 0) || (((attempt + 1) % 10) == 0)) {
      printf("[m2m-devboard] camera sync response failed status=0x%lx header=%02X %02X %02X %02X %02X %02X\r\n",
             (unsigned long)status,
             response[0],
             response[1],
             response[2],
             response[3],
             response[4],
             response[5]);
    }

    osDelay(delay_ms);
    delay_ms += delay_increment_ms;
  }

  printf("[m2m-devboard] camera sync failed after %d attempts\r\n", max_retries);
  return SL_STATUS_FAIL;
}

static bool camera_initialize_if_needed(void)
{
  sl_si91x_usart_control_config_t uart_config = {
    .baudrate = M2M_CAM_BAUDRATE,
    .mode = SL_USART_MODE_ASYNCHRONOUS,
    .parity = SL_USART_NO_PARITY,
    .stopbits = SL_USART_STOP_BITS_1,
    .hwflowcontrol = SL_USART_FLOW_CONTROL_NONE,
    .databits = SL_USART_DATA_BITS_8,
    .misc_control = SL_USART_MISC_CONTROL_NONE,
    .usart_module = M2M_CAM_UART_INSTANCE,
    .config_enable = ENABLE,
    .synch_mode = DISABLE,
  };
  sl_status_t status;

  if (camera_ready) {
    return true;
  }

  printf("[m2m-devboard] camera init begin\r\n");
  camera_set_reset(false);
  osDelay(20U);
  camera_set_reset(true);
  osDelay(500U);

  status = sl_si91x_usart_init(M2M_CAM_UART_INSTANCE, &camera_uart_handle);
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] camera UART init failed: 0x%lx\r\n", (unsigned long)status);
    return false;
  }
  printf("[m2m-devboard] camera UART init ok\r\n");

  status = sl_si91x_usart_set_configuration(camera_uart_handle, &uart_config);
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] camera UART config failed: 0x%lx\r\n", (unsigned long)status);
    return false;
  }
  printf("[m2m-devboard] camera UART config ok baud=%lu\r\n", (unsigned long)M2M_CAM_BAUDRATE);

  sl_si91x_usart_multiple_instance_register_event_callback(M2M_CAM_UART_INSTANCE, camera_uart_callback_event);

  if (camera_sync(60, 2000U, 5U, 1U) != SL_STATUS_OK) {
    printf("[m2m-devboard] camera sync failed during init\r\n");
    return false;
  }
  printf("[m2m-devboard] camera sync stage ok\r\n");

  status = camera_send_command(CAM_CMD_INIT, 0x00, CAM_FORMAT_JPEG, CAM_RESOLUTION_640X480, CAM_RESOLUTION_640X480);
  if ((status != SL_STATUS_OK) || (camera_wait_for_specific_ack(CAM_CMD_INIT, 5000U) != SL_STATUS_OK)) {
    printf("[m2m-devboard] camera init command failed\r\n");
    return false;
  }
  printf("[m2m-devboard] camera init command ok\r\n");

  status = camera_send_command(CAM_CMD_SETSIZE, 0x08, 0x40, 0x00, 0x00);
  if ((status != SL_STATUS_OK) || (camera_wait_for_specific_ack(CAM_CMD_SETSIZE, 5000U) != SL_STATUS_OK)) {
    printf("[m2m-devboard] camera packet size config failed\r\n");
    return false;
  }
  printf("[m2m-devboard] camera packet size config ok\r\n");

  camera_ready = true;
  return true;
}

void m2m_dev_board_init(void)
{
  if (audio_mutex == NULL) {
    audio_mutex = osMutexNew(NULL);
  }

  if (camera_mutex == NULL) {
    camera_mutex = osMutexNew(NULL);
  }

  configure_hp_gpio_output((uint8_t)RTE_LED0_PIN, (uint8_t)RTE_LED0_PAD);
  configure_hp_gpio_output((uint8_t)RTE_GPIO_12_PIN, (uint8_t)RTE_GPIO_12_PAD);
  camera_set_reset(true);
  m2m_dev_board_set_debug_led(false);
  button_init_instances();
  (void)audio_hw_init();
  (void)imu_hw_init();

  printf("[m2m-devboard] button=HP%u, debug_led=HP%u, debug_vcom={tx=ULP11,rx=ULP9}, cam_uart={tx=HP7,rx=HP6}, imu_i2c=auto_probe{gpio71_70,gpio46_47,gpio15_47}, cam_reset=HP12, i2s0={sclk=HP25, ws=HP26, din=HP27, dout=HP28}\r\n",
         (unsigned int)RTE_BUTTON0_PIN,
         (unsigned int)RTE_LED0_PIN);
  printf("[m2m-devboard] speaker path expects external amp enable/SD already asserted in hardware; firmware does not drive a dedicated amp enable pin\r\n");
}

void m2m_dev_board_set_debug_led(bool on)
{
  debug_led_on = on;
  set_hp_gpio_output((uint8_t)RTE_LED0_PIN, on);
}

bool m2m_dev_board_get_debug_led(void)
{
  return debug_led_on;
}

bool m2m_dev_board_read_ptt_button(void)
{
  return sl_si91x_button_state(button_btn0.pin, button_btn0.port) == 0U;
}

void m2m_dev_board_read_imu(m2m_imu_sample_t *sample)
{
  if (sample == NULL) {
    return;
  }

  if (!imu_ready && !imu_hw_init()) {
    fill_idle_imu_sample(sample);
    return;
  }

  if (!imu_read_sample(sample)) {
    if (!imu_read_failure_logged) {
      printf("[m2m-devboard] IMU sample read failed, scheduling reinit\r\n");
      imu_read_failure_logged = true;
    }
    imu_schedule_retry(M2M_IMU_I2C_RECOVERY_DELAY_MS);
    fill_idle_imu_sample(sample);
    return;
  }

  imu_read_failure_logged = false;
}

bool m2m_dev_board_run_imu_self_test(m2m_imu_sample_t *sample)
{
  m2m_imu_sample_t local_sample = { 0 };
  m2m_imu_sample_t *target_sample = (sample != NULL) ? sample : &local_sample;

  fill_idle_imu_sample(target_sample);
  if (!imu_ready) {
    imu_next_init_retry_ms = 0U;
  }

  if (!imu_hw_init()) {
    return false;
  }

  if (!imu_read_sample(target_sample)) {
    printf("[m2m-devboard] IMU self-test read failed, scheduling reinit\r\n");
    imu_schedule_retry(M2M_IMU_I2C_RECOVERY_DELAY_MS);
    return false;
  }

  return true;
}

bool m2m_dev_board_is_fall_candidate(const m2m_imu_sample_t *sample)
{
  static m2m_imu_fall_state_t fall_state = M2M_IMU_STATE_NORMAL;
  static uint32_t state_timer_ms = 0U;
  static uint8_t free_fall_count = 0U;
  static uint8_t impact_count = 0U;
  static uint8_t stable_count = 0U;
  static uint8_t latched_reports_remaining = 0U;
  int32_t magnitude_sq;
  const int32_t free_fall_threshold_sq = (int32_t)(M2M_IMU_FREE_FALL_THRESHOLD_MG * M2M_IMU_FREE_FALL_THRESHOLD_MG);
  const int32_t impact_threshold_sq = (int32_t)(M2M_IMU_IMPACT_THRESHOLD_MG * M2M_IMU_IMPACT_THRESHOLD_MG);
  const int32_t stable_lower_threshold_sq = (int32_t)(M2M_IMU_STABLE_LOWER_THRESHOLD_MG * M2M_IMU_STABLE_LOWER_THRESHOLD_MG);
  const int32_t stable_upper_threshold_sq = (int32_t)(M2M_IMU_STABLE_UPPER_THRESHOLD_MG * M2M_IMU_STABLE_UPPER_THRESHOLD_MG);
  const int32_t recovery_threshold_sq = (int32_t)(M2M_IMU_RECOVERY_THRESHOLD_MG * M2M_IMU_RECOVERY_THRESHOLD_MG);

  if (sample == NULL) {
    return false;
  }

  magnitude_sq = ((int32_t)sample->accel_mg[0] * sample->accel_mg[0])
                 + ((int32_t)sample->accel_mg[1] * sample->accel_mg[1])
                 + ((int32_t)sample->accel_mg[2] * sample->accel_mg[2]);

  switch (fall_state) {
    case M2M_IMU_STATE_NORMAL:
      if (magnitude_sq < free_fall_threshold_sq) {
        free_fall_count++;
        if (free_fall_count >= M2M_IMU_FREE_FALL_MIN_SAMPLES) {
          fall_state = M2M_IMU_STATE_FREE_FALL;
          state_timer_ms = 0U;
          free_fall_count = 0U;
          impact_count = 0U;
          imu_log_transition("free_fall_confirmed", sample);
        }
      } else {
        free_fall_count = 0U;
      }
      break;

    case M2M_IMU_STATE_FREE_FALL:
      state_timer_ms += M2M_IMU_SAMPLE_PERIOD_MS;

      if (magnitude_sq > impact_threshold_sq) {
        impact_count++;
        if (impact_count >= M2M_IMU_IMPACT_MIN_SAMPLES) {
          fall_state = M2M_IMU_STATE_IMPACT;
          state_timer_ms = 0U;
          impact_count = 0U;
          stable_count = 0U;
          imu_log_transition("impact_confirmed", sample);
        }
      } else if (state_timer_ms > M2M_IMU_FREE_FALL_TIMEOUT_MS) {
        fall_state = M2M_IMU_STATE_NORMAL;
        state_timer_ms = 0U;
        impact_count = 0U;
        imu_log_transition("free_fall_timeout_reset", sample);
      }
      break;

    case M2M_IMU_STATE_IMPACT:
      state_timer_ms += M2M_IMU_SAMPLE_PERIOD_MS;

      if (state_timer_ms > M2M_IMU_POST_IMPACT_WAIT_MS) {
        if ((magnitude_sq > stable_lower_threshold_sq) && (magnitude_sq < stable_upper_threshold_sq)) {
          stable_count++;
          if (stable_count >= M2M_IMU_STABLE_MIN_SAMPLES) {
            fall_state = M2M_IMU_STATE_FALL_DETECTED;
            state_timer_ms = 0U;
            stable_count = 0U;
            latched_reports_remaining = M2M_IMU_FALL_EVENT_LATCH_SAMPLES;
            imu_log_transition("fall_detected", sample);
          }
        } else {
          fall_state = M2M_IMU_STATE_NORMAL;
          state_timer_ms = 0U;
          stable_count = 0U;
          imu_log_transition("impact_rejected_reset", sample);
        }
      }
      break;

    case M2M_IMU_STATE_FALL_DETECTED:
      if (magnitude_sq > recovery_threshold_sq) {
        fall_state = M2M_IMU_STATE_NORMAL;
        state_timer_ms = 0U;
        imu_log_transition("recovered", sample);
      }
      break;

    default:
      fall_state = M2M_IMU_STATE_NORMAL;
      state_timer_ms = 0U;
      free_fall_count = 0U;
      impact_count = 0U;
      stable_count = 0U;
      latched_reports_remaining = 0U;
      break;
  }

  if (latched_reports_remaining > 0U) {
    latched_reports_remaining--;
    return true;
  }

  return false;
}

bool m2m_dev_board_capture_audio_pcm(int16_t *pcm_buffer,
                                     size_t max_samples,
                                     uint32_t max_duration_ms,
                                     const volatile bool *stop_requested,
                                     bool *stopped_by_request,
                                     size_t *captured_samples)
{
  sl_status_t status = SL_STATUS_OK;
  uint32_t start_ms;
  uint32_t tick_freq;
  size_t total_samples = 0U;
  bool stopped_by_button = false;
  m2m_audio_capture_layout_t capture_layout = M2M_AUDIO_CAPTURE_LAYOUT_UNKNOWN;
  sl_i2s_xfer_config_t mic_config = {
    .mode = SL_I2S_MASTER,
    .protocol = SL_I2S_PROTOCOL,
    .sync = SL_I2S_ASYNC,
    .resolution = SL_I2S_RESOLUTION_32,
    .data_size = SL_I2S_DATA_SIZE32,
    // The generic receive path has produced stronger, cleaner captures on BRD2708A than
    // the BRD2605A-specific ICS43434 helper mode.
    .transfer_type = SL_I2S_RECEIVE,
    .sampling_rate = M2M_AUDIO_SAMPLE_RATE,
  };

  if ((pcm_buffer == NULL) || (max_samples == 0U) || (captured_samples == NULL)) {
    return false;
  }

  *captured_samples = 0U;
  if (stopped_by_request != NULL) {
    *stopped_by_request = false;
  }

  if ((audio_mutex == NULL) || (osMutexAcquire(audio_mutex, osWaitForever) != osOK)) {
    printf("[m2m-devboard] audio capture could not acquire mutex\r\n");
    return false;
  }

  if (!audio_hw_init()) {
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  audio_owner_thread = osThreadGetId();
  memset(audio_capture_stage_buffer, 0, sizeof(audio_capture_stage_buffer));
  (void)osThreadFlagsClear(M2M_AUDIO_FLAG_RX_DONE | M2M_AUDIO_FLAG_TX_DONE);

  // Reset both directions before switching from prompt playback into mic capture.
  (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_SEND_ABORT);
  (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_RECEIVE_ABORT);

  status = sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_FULL_POWER);
  if (status == SL_STATUS_OK) {
    status = sl_si91x_i2s_config_transmit_receive(audio_i2s_handle, &mic_config);
  }
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] I2S mic config failed: 0x%lx\r\n", (unsigned long)status);
    audio_owner_thread = NULL;
    (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  start_ms = osKernelGetTickCount();
  tick_freq = osKernelGetTickFreq();
  if (tick_freq == 0U) {
    tick_freq = 1000U;
  }
  status = sl_si91x_i2s_receive_data(audio_i2s_handle, audio_capture_stage_buffer, M2M_AUDIO_CAPTURE_CHUNK_SAMPLES);
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] I2S mic DMA start failed: 0x%lx\r\n", (unsigned long)status);
    (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_RECEIVE_ABORT);
    (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
    audio_owner_thread = NULL;
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  while (total_samples < max_samples) {
    uint32_t flags = osThreadFlagsWait(M2M_AUDIO_FLAG_RX_DONE,
                                       osFlagsWaitAny,
                                       capture_timeout_ms_for_chunk(M2M_AUDIO_CAPTURE_CHUNK_SAMPLES));
    size_t chunk_samples;

    if ((flags == (uint32_t)osErrorTimeout) || ((flags & osFlagsError) != 0U)) {
      printf("[m2m-devboard] audio capture timeout\r\n");
      status = SL_STATUS_TIMEOUT;
      break;
    }

    if ((stop_requested != NULL) && *stop_requested) {
      stopped_by_button = true;
    }

    if (((osKernelGetTickCount() - start_ms) * 1000U / tick_freq) >= max_duration_ms) {
      stopped_by_button = false;
      status = SL_STATUS_TIMEOUT;
    }

    chunk_samples = copy_capture_chunk_to_pcm(pcm_buffer,
                                              total_samples,
                                              max_samples,
                                              audio_capture_stage_buffer,
                                              &capture_layout);

    total_samples += chunk_samples;

    if ((total_samples >= max_samples) || stopped_by_button || (status == SL_STATUS_TIMEOUT)) {
      break;
    }

    (void)osThreadFlagsClear(M2M_AUDIO_FLAG_RX_DONE);
    status = sl_si91x_i2s_receive_data(audio_i2s_handle, audio_capture_stage_buffer, M2M_AUDIO_CAPTURE_CHUNK_SAMPLES);
    if (status != SL_STATUS_OK) {
      printf("[m2m-devboard] I2S mic DMA requeue failed: 0x%lx\r\n", (unsigned long)status);
      break;
    }
  }

  (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_RECEIVE_ABORT);
  (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
  audio_owner_thread = NULL;
  (void)osMutexRelease(audio_mutex);

  *captured_samples = total_samples;
  if (stopped_by_request != NULL) {
    *stopped_by_request = stopped_by_button;
  }

  printf("[m2m-devboard] audio capture complete (%lu samples, stop=%s)\r\n",
         (unsigned long)total_samples,
         stopped_by_button ? "button" : "timeout");
  return (status == SL_STATUS_OK) || ((status == SL_STATUS_TIMEOUT) && (total_samples > 0U));
}

bool m2m_dev_board_capture_image(uint8_t *buffer, size_t buffer_size, size_t *captured_size)
{
  sl_status_t status;
  bool capture_ok = false;

  if ((buffer == NULL) || (buffer_size == 0U) || (captured_size == NULL)) {
    return false;
  }

  *captured_size = 0U;

  if ((camera_mutex == NULL) || (osMutexAcquire(camera_mutex, osWaitForever) != osOK)) {
    printf("[m2m-devboard] camera mutex unavailable\r\n");
    return false;
  }

  if (!camera_initialize_if_needed()) {
    (void)osMutexRelease(camera_mutex);
    return false;
  }

  camera_owner_thread = osThreadGetId();

  if (camera_sync(5, 500U, 50U, 0U) != SL_STATUS_OK) {
    printf("[m2m-devboard] camera resync failed before snapshot\r\n");
    camera_owner_thread = NULL;
    (void)osMutexRelease(camera_mutex);
    return false;
  }

  status = camera_send_command(CAM_CMD_SNAPSHOT, CAM_PIC_TYPE_SNAPSHOT, 0x00, 0x00, 0x00);
  if ((status != SL_STATUS_OK) || (camera_wait_for_specific_ack(CAM_CMD_SNAPSHOT, 5000U) != SL_STATUS_OK)) {
    printf("[m2m-devboard] camera snapshot command failed\r\n");
    camera_owner_thread = NULL;
    (void)osMutexRelease(camera_mutex);
    return false;
  }

  osDelay(500U);

  status = camera_send_command(CAM_CMD_GET_PIC, CAM_PIC_TYPE_CAPTURE, 0x00, 0x00, 0x00);
  if ((status != SL_STATUS_OK) || (camera_wait_for_specific_ack(CAM_CMD_GET_PIC, 5000U) != SL_STATUS_OK)) {
    printf("[m2m-devboard] camera get-picture command failed\r\n");
    camera_owner_thread = NULL;
    (void)osMutexRelease(camera_mutex);
    return false;
  }

  if ((camera_receive_bytes(camera_rx_buf, sizeof(camera_rx_buf), 1000U) == SL_STATUS_OK)
      && (camera_rx_buf[1] == CAM_CMD_DATA)) {
    const uint32_t image_len = (uint32_t)camera_rx_buf[3]
                               | ((uint32_t)camera_rx_buf[4] << 8)
                               | ((uint32_t)camera_rx_buf[5] << 16);
    size_t bytes_received = 0U;

    (void)camera_send_command(CAM_CMD_ACK, 0x00, 0x00, 0x00, 0x00);

    while (bytes_received < image_len) {
      uint8_t package_header[4];
      uint8_t checksum[2];
      uint16_t current_package_id;
      uint16_t payload_size;

      if (camera_receive_bytes(package_header, sizeof(package_header), 2000U) != SL_STATUS_OK) {
        printf("[m2m-devboard] camera packet header timeout\r\n");
        break;
      }

      current_package_id = (uint16_t)package_header[0] | ((uint16_t)package_header[1] << 8);
      payload_size = (uint16_t)package_header[2] | ((uint16_t)package_header[3] << 8);

      if ((bytes_received + payload_size) > buffer_size) {
        printf("[m2m-devboard] camera image overflow cap=%lu need=%lu\r\n",
               (unsigned long)buffer_size,
               (unsigned long)(bytes_received + payload_size));
        break;
      }

      if (camera_receive_bytes(&buffer[bytes_received], payload_size, 2000U) != SL_STATUS_OK) {
        printf("[m2m-devboard] camera packet payload timeout\r\n");
        break;
      }
      bytes_received += payload_size;

      (void)camera_receive_bytes(checksum, sizeof(checksum), 500U);
      (void)camera_send_command(CAM_CMD_ACK,
                                0x00,
                                0x00,
                                (uint8_t)(current_package_id & 0xFFU),
                                (uint8_t)(current_package_id >> 8));
    }

    (void)camera_send_command(CAM_CMD_ACK, 0x00, 0x00, 0xF0, 0xF0);
    if (bytes_received == image_len) {
      *captured_size = bytes_received;
      capture_ok = true;
      printf("[m2m-devboard] camera capture complete (%lu bytes)\r\n", (unsigned long)*captured_size);
    } else {
      printf("[m2m-devboard] camera capture incomplete (%lu/%lu bytes)\r\n",
             (unsigned long)bytes_received,
             (unsigned long)image_len);
    }
  } else {
    printf("[m2m-devboard] camera did not send a DATA header\r\n");
    (void)camera_send_command(CAM_CMD_ACK, 0x00, 0x00, 0xF0, 0xF0);
  }

  camera_owner_thread = NULL;
  (void)osMutexRelease(camera_mutex);
  return capture_ok;
}

bool m2m_dev_board_play_wav(const uint8_t *wav_data, size_t wav_size)
{
  sl_status_t status;
  sl_i2s_sampling_rate_t playback_rate = SL_I2S_SAMPLING_RATE_16000;
  sl_i2s_xfer_config_t speaker_config = {
    .mode = SL_I2S_MASTER,
    .protocol = SL_I2S_PROTOCOL,
    .sync = SL_I2S_ASYNC,
    .resolution = SL_I2S_RESOLUTION_16,
    .data_size = SL_I2S_DATA_SIZE16,
    .transfer_type = SL_I2S_TRANSMIT,
    .sampling_rate = SL_I2S_SAMPLING_RATE_16000,
  };
  const uint8_t *pcm_data = NULL;
  uint16_t num_channels = 0U;
  uint16_t bits_per_sample = 0U;
  uint32_t sample_rate_hz = 0U;
  size_t pcm_size = 0U;
  size_t pcm_offset = 0U;
  size_t bytes_per_frame;
  size_t offset = 0U;
  uint32_t transmitted_items = 0U;

  if (!parse_wav_payload(wav_data,
                         wav_size,
                         &num_channels,
                         &bits_per_sample,
                         &sample_rate_hz,
                         &pcm_data,
                         &pcm_size)) {
    printf("[m2m-devboard] unsupported WAV payload\r\n");
    return false;
  }

  if (!map_audio_sample_rate(sample_rate_hz, &playback_rate)) {
    printf("[m2m-devboard] unsupported WAV sample rate=%lu\r\n", (unsigned long)sample_rate_hz);
    return false;
  }

  pcm_offset = (size_t)(pcm_data - wav_data);
  bytes_per_frame = (size_t)num_channels * (bits_per_sample / 8U);
  if (bytes_per_frame == 0U) {
    printf("[m2m-devboard] invalid WAV frame size channels=%u bits=%u\r\n",
           (unsigned int)num_channels,
           (unsigned int)bits_per_sample);
    return false;
  }
  printf("[m2m-devboard] WAV playback begin wav_bytes=%lu pcm_bytes=%lu data_offset=%lu rate=%lu channels=%u bits=%u gain=%u/%u first4=%02X %02X %02X %02X\r\n",
         (unsigned long)wav_size,
         (unsigned long)pcm_size,
         (unsigned long)pcm_offset,
         (unsigned long)sample_rate_hz,
         (unsigned int)num_channels,
         (unsigned int)bits_per_sample,
         (unsigned int)M2M_AUDIO_WAV_PLAYBACK_GAIN_NUM,
         (unsigned int)M2M_AUDIO_WAV_PLAYBACK_GAIN_DEN,
         wav_data[0],
         wav_data[1],
         wav_data[2],
         wav_data[3]);

  if ((audio_mutex == NULL) || (osMutexAcquire(audio_mutex, osWaitForever) != osOK)) {
    printf("[m2m-devboard] WAV playback busy\r\n");
    return false;
  }

  if (!audio_hw_init()) {
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  speaker_config.sampling_rate = playback_rate;
  audio_owner_thread = osThreadGetId();
  (void)osThreadFlagsClear(M2M_AUDIO_FLAG_TX_DONE | M2M_AUDIO_FLAG_RX_DONE);

  status = sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_FULL_POWER);
  if (status == SL_STATUS_OK) {
    status = sl_si91x_i2s_config_transmit_receive(audio_i2s_handle, &speaker_config);
  }
  if (status != SL_STATUS_OK) {
    printf("[m2m-devboard] WAV playback config failed: 0x%lx\r\n", (unsigned long)status);
    audio_owner_thread = NULL;
    (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
    (void)osMutexRelease(audio_mutex);
    return false;
  }

  while ((offset + bytes_per_frame) <= pcm_size) {
    uint32_t wait_flags;
    size_t chunk_frames = (pcm_size - offset) / bytes_per_frame;

    if (chunk_frames > M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES) {
      chunk_frames = M2M_AUDIO_PLAYBACK_CHUNK_SAMPLES;
    }

    for (size_t frame = 0U; frame < chunk_frames; frame++) {
      const uint8_t *frame_ptr = pcm_data + offset + (frame * bytes_per_frame);
      int16_t sample = scale_cloud_wav_sample(read_wav_mono_sample(frame_ptr, num_channels));
      size_t sample_index = frame * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT;

      audio_prompt_buffer_a[sample_index] = sample;
      audio_prompt_buffer_a[sample_index + 1U] = sample;
    }

    (void)osThreadFlagsClear(M2M_AUDIO_FLAG_TX_DONE);
    status = sl_si91x_i2s_transmit_data(audio_i2s_handle,
                                        audio_prompt_buffer_a,
                                        (uint32_t)(chunk_frames * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT));
    if (status != SL_STATUS_OK) {
      printf("[m2m-devboard] WAV playback TX failed: 0x%lx\r\n", (unsigned long)status);
      log_audio_playback_status("wav_tx_failed", transmitted_items);
      break;
    }
    transmitted_items += (uint32_t)(chunk_frames * M2M_AUDIO_PLAYBACK_CHANNEL_COUNT);

    wait_flags = osThreadFlagsWait(M2M_AUDIO_FLAG_TX_DONE, osFlagsWaitAny, 1000U);
    if ((wait_flags == (uint32_t)osErrorTimeout) || ((wait_flags & osFlagsError) != 0U)) {
      printf("[m2m-devboard] WAV playback TX timeout\r\n");
      status = SL_STATUS_TIMEOUT;
      log_audio_playback_status("wav_tx_timeout", transmitted_items);
      break;
    }

    offset += chunk_frames * bytes_per_frame;
  }

  if ((status == SL_STATUS_OK) && (offset > 0U)) {
    osDelay(M2M_AUDIO_TX_DRAIN_MS);
    log_audio_playback_status("wav_complete", transmitted_items);
  }

  (void)sl_si91x_i2s_end_transfer(audio_i2s_handle, SL_I2S_SEND_ABORT);
  (void)sl_si91x_i2s_configure_power_mode(audio_i2s_handle, SL_I2S_POWER_OFF);
  audio_owner_thread = NULL;
  (void)osMutexRelease(audio_mutex);

  if (status == SL_STATUS_OK) {
    printf("[m2m-devboard] WAV playback complete (%lu bytes @ %lu Hz, gain=%u/%u)\r\n",
           (unsigned long)pcm_size,
           (unsigned long)sample_rate_hz,
           (unsigned int)M2M_AUDIO_WAV_PLAYBACK_GAIN_NUM,
           (unsigned int)M2M_AUDIO_WAV_PLAYBACK_GAIN_DEN);
    return true;
  }

  return false;
}

bool m2m_dev_board_run_speaker_self_test(void)
{
  return play_prompt_internal("speaker_self_test", "startup_self_test", M2M_AUDIO_SELF_TEST_REPEAT_COUNT);
}

void m2m_dev_board_play_prompt(const char *prompt_tag, const char *detail)
{
  (void)play_prompt_internal(prompt_tag, detail, M2M_AUDIO_PROMPT_REPEAT_COUNT);
}
