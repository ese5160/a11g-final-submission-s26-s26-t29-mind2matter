/***************************************************************************/ /**
 * @file
 * @brief Mind2Matter dev-board MVP application.
 *******************************************************************************
 * This replaces the one-shot HTTP OTAF demo loop with a FreeRTOS application
 * skeleton that follows the A08G/A09G architecture: Wi-Fi + MQTT control
 * messages, dev-board button/LED checkoff behavior, and portable task
 * boundaries for future custom-PCB drivers.
 ******************************************************************************/

#include "app.h"
#include "cmsis_os2.h"
#include "m2m_app_config.h"
#include "m2m_dev_board.h"
#include "sl_additional_status.h"
#include "sl_net_default_values.h"
#include "sl_constants.h"
#include "sl_http_client.h"
#include "sl_mqtt_client.h"
#include "sl_net.h"
#include "sl_si91x_hal_soc_soft_reset.h"
#include "sl_si91x_psram.h"
#include "sl_si91x_psram_handle.h"
#include "sl_utility.h"
#include "sl_wifi.h"
#include "sl_wifi_callback_framework.h"
#include "sl_wifi_types.h"
#include "rsi_wwdt.h"
#include "task.h"

#include "silabs_dgcert_ca.pem.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "firmware_upgradation.h"

#define SYSTEM_FLAG_WIFI_CONNECTED  (1UL << 0)
#define SYSTEM_FLAG_MQTT_CONNECTED  (1UL << 1)
#define SYSTEM_FLAG_VOICE_ACTIVE    (1UL << 2)
#define SYSTEM_FLAG_FALL_PENDING    (1UL << 3)
#define SYSTEM_FLAG_VOICE_RECORDING (1UL << 4)
#define SYSTEM_FLAG_RUNTIME_READY   (1UL << 5)
#define SYSTEM_FLAG_STARTUP_ACTIVE  (1UL << 6)

#define MQTT_ENCRYPT_CONNECTION 0
#define MQTT_CLEAN_SESSION      1
#define MQTT_KEEPALIVE_RETRIES  0
#define MQTT_MESSAGE_RETAINED   0
#define MQTT_DUPLICATE_MESSAGE  0
#define HTTP_FLAG_EVENT         (1UL << 4)
#define M2M_TTS_PROMPT_REPLY_WAIT_MS 12000U
#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
#define WIFI_DIAG_FLAG_SCAN_DONE (1UL << 0)
#endif

#define M2M_HTTP_HOST_MAX      64U
#define M2M_HTTP_RESOURCE_MAX  192U
#define M2M_PSRAM_TOTAL_BYTES  (8U * 1024U * 1024U)
#define M2M_PSRAM_ALIGN_BYTES  32U
#define M2M_WIFI_SCAN_TIMEOUT_MS 10000U
#define M2M_WIFI_SCAN_MAX_APS   24U

typedef struct {
  const uint8_t *data;
  size_t length;
} http_body_segment_t;

typedef enum {
  WIFI_START_PHASE_IDLE = 0,
  WIFI_START_PHASE_SETTLE,
  WIFI_START_PHASE_NET_INIT_BEGIN,
  WIFI_START_PHASE_NET_INIT_DONE,
  WIFI_START_PHASE_SCAN,
  WIFI_START_PHASE_CONNECTING,
  WIFI_START_PHASE_CONNECTED,
  WIFI_START_PHASE_FAILED,
} wifi_start_phase_t;

typedef enum {
  BUTTON_EVENT_PRESSED = 0,
  BUTTON_EVENT_RELEASED,
  BUTTON_EVENT_LONG_PRESS,
} button_event_type_t;

typedef enum {
  SENSOR_EVENT_FALL_CANDIDATE = 0,
} sensor_event_type_t;

typedef enum {
  AUDIO_PROMPT_VOICE_REQUEST_SENT = 0,
  AUDIO_PROMPT_VOICE_REPLY,
  AUDIO_PROMPT_FALL_CONFIRM,
  AUDIO_PROMPT_REMINDER,
  AUDIO_PROMPT_NOTICE,
  AUDIO_PROMPT_OTA_STATUS,
} audio_prompt_type_t;

typedef enum {
  VOICE_EVENT_START_QA = 0,
  VOICE_EVENT_START_VISION,
} voice_event_type_t;

typedef enum {
  LOCAL_ACTIVITY_IDLE = 0,
  LOCAL_ACTIVITY_VOICE_RECORDING,
  LOCAL_ACTIVITY_UPLOADING,
  LOCAL_ACTIVITY_WAITING_REPLY,
  LOCAL_ACTIVITY_DOWNLOADING,
  LOCAL_ACTIVITY_PLAYING_AUDIO,
  LOCAL_ACTIVITY_OTA,
  LOCAL_ACTIVITY_ERROR,
} local_activity_t;

typedef enum {
  LED_STATUS_BOOT = 0,
  LED_STATUS_WIFI_CONNECTING,
  LED_STATUS_WIFI_FAILED,
  LED_STATUS_MQTT_CONNECTING,
  LED_STATUS_READY,
  LED_STATUS_VOICE_RECORDING,
  LED_STATUS_UPLOADING,
  LED_STATUS_WAITING_REPLY,
  LED_STATUS_DOWNLOADING,
  LED_STATUS_PLAYING_AUDIO,
  LED_STATUS_OTA,
  LED_STATUS_ERROR,
} led_status_t;

typedef struct {
  button_event_type_t type;
  uint32_t tick_ms;
} button_event_t;

typedef struct {
  sensor_event_type_t type;
  m2m_imu_sample_t sample;
  uint32_t tick_ms;
} sensor_event_t;

typedef struct {
  const char *topic;
  sl_mqtt_qos_t qos;
  bool retained;
  char payload[M2M_MQTT_PAYLOAD_MAX];
} cloud_tx_msg_t;

typedef struct {
  audio_prompt_type_t type;
  bool use_cloud_speech;
  char text[M2M_AUDIO_PROMPT_TEXT_MAX];
} audio_prompt_msg_t;

typedef struct {
  voice_event_type_t type;
} voice_event_msg_t;

typedef struct {
  char request_id[24];
  char audio_url[M2M_OTA_URL_MAX];
  char text[M2M_AUDIO_PROMPT_TEXT_MAX];
  char format[16];
  uint32_t sample_rate_hz;
} voice_reply_msg_t;

typedef struct {
  char url[M2M_OTA_URL_MAX];
  char target_version[M2M_OTA_TARGET_VERSION_MAX];
} ota_event_msg_t;

typedef struct {
  const char *key;
  const char *value;
} http_request_header_t;

typedef struct {
  osThreadId_t owner_thread;
  uint8_t *buffer;
  size_t buffer_capacity;
  size_t response_length;
  sl_status_t callback_status;
  uint16_t http_response_code;
  bool completed;
} http_request_context_t;

typedef struct {
  bool wifi_connected;
  bool mqtt_connected;
  bool voice_busy;
  bool voice_recording;
  bool fall_pending;
  bool ota_in_progress;
  bool debug_button_pressed;
  bool debug_led_on;
  uint32_t request_counter;
  uint32_t boot_tick_ms;
  uint32_t fall_deadline_ms;
  char latest_reminder[M2M_REMINDER_CONFIG_TEXT_MAX];
  char ota_status[M2M_OTA_STATUS_TEXT_MAX];
  char ota_target_version[M2M_OTA_TARGET_VERSION_MAX];
} device_state_t;

typedef struct {
  bool ready;
  int16_t *recording_pcm;
  size_t recording_pcm_capacity_samples;
  uint8_t *image_buffer;
  size_t image_buffer_capacity;
  uint8_t *reply_audio_buffer;
  size_t reply_audio_capacity;
} psram_media_t;

typedef struct {
  char payload[M2M_MQTT_PAYLOAD_MAX];
  char request_id[24];
  char text_preview[128];
  char escaped_speech_text[(M2M_AUDIO_PROMPT_TEXT_MAX * 2U) + 1U];
  device_state_t snapshot;
  voice_reply_msg_t reply;
} tts_prompt_workspace_t;

static osMessageQueueId_t button_event_q;
static osMessageQueueId_t sensor_event_q;
static osMessageQueueId_t cloud_tx_q;
static osMessageQueueId_t audio_q;
static osMessageQueueId_t voice_event_q;
static osMessageQueueId_t voice_reply_q;
static osMessageQueueId_t ota_event_q;
static osEventFlagsId_t system_flags;
static osEventFlagsId_t wifi_diag_flags;
static osMutexId_t device_state_mutex;

static sl_mqtt_client_t mqtt_client;
static volatile bool mqtt_connected;
static volatile bool mqtt_subscribed;
static volatile bool ota_exclusive_mode;
static volatile bool ota_response_complete;
static volatile sl_status_t ota_callback_status;
static volatile bool status_publish_pending;
static volatile bool voice_stop_requested;
static volatile wifi_start_phase_t wifi_start_phase;
static volatile local_activity_t local_activity;
#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
static volatile sl_status_t wifi_scan_diag_status;
#endif
static device_state_t device_state;
static psram_media_t psram_media;
static tts_prompt_workspace_t tts_prompt_workspace;
static char mqtt_handler_payload[M2M_MQTT_PAYLOAD_MAX];
static char mqtt_handler_payload_preview[128];
static char mqtt_handler_topic[128];
static voice_reply_msg_t mqtt_handler_voice_reply;
static osThreadId_t mqtt_task_thread_id;
static osThreadId_t voice_ai_task_thread_id;
#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
static sl_wifi_extended_scan_result_t wifi_scan_diag_results[M2M_WIFI_SCAN_MAX_APS];
#endif

static const osThreadAttr_t mqtt_task_attributes = {
  .name = "mqtt_task",
  .stack_size = 4096,
  .priority = osPriorityNormal,
};

static const osThreadAttr_t system_control_task_attributes = {
  .name = "system_control_task",
  .stack_size = 3072,
  .priority = osPriorityNormal,
};

static const osThreadAttr_t imu_task_attributes = {
  .name = "imu_task",
  .stack_size = 2048,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t voice_ai_task_attributes = {
  .name = "voice_ai_task",
  .stack_size = 9216,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t reminder_audio_task_attributes = {
  .name = "reminder_audio_task",
  .stack_size = 6144,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t startup_diag_task_attributes = {
  .name = "startup_diag_task",
  .stack_size = 4096,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t ota_task_attributes = {
  .name = "ota_task",
  .stack_size = 4096,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t dev_board_input_task_attributes = {
  .name = "dev_board_input_task",
  .stack_size = 1536,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t status_indicator_task_attributes = {
  .name = "status_indicator_task",
  .stack_size = 1536,
  .priority = osPriorityLow,
};

static const sl_wifi_device_configuration_t wifi_client_configuration = {
  .boot_option = LOAD_NWP_FW,
  .mac_address = NULL,
  .band = M2M_WIFI_DEVICE_RADIO_BAND,
  .region_code = M2M_WIFI_DEVICE_REGION_CODE,
  .boot_config = {
    .oper_mode = SL_SI91X_CLIENT_MODE,
    .coex_mode = SL_SI91X_WLAN_ONLY_MODE,
    .feature_bit_map = (SL_SI91X_FEAT_SECURITY_PSK | SL_SI91X_FEAT_AGGREGATION),
    .tcp_ip_feature_bit_map = (SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT
                               | SL_SI91X_TCP_IP_FEAT_HTTP_CLIENT
                               | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT
                               | SL_SI91X_TCP_IP_FEAT_EXTENSION_VALID
#if MQTT_ENCRYPT_CONNECTION
                               | SL_SI91X_TCP_IP_FEAT_SSL
#endif
#ifdef SLI_SI91X_ENABLE_IPV6
                               | SL_SI91X_TCP_IP_FEAT_DHCPV6_CLIENT
                               | SL_SI91X_TCP_IP_FEAT_IPV6
#endif
                               ),
    .custom_feature_bit_map = SL_SI91X_CUSTOM_FEAT_EXTENTION_VALID,
    .ext_custom_feature_bit_map = (SL_SI91X_EXT_FEAT_XTAL_CLK
                                   | SL_SI91X_EXT_FEAT_UART_SEL_FOR_DEBUG_PRINTS
                                   | MEMORY_CONFIG
#if defined(SLI_SI917) || defined(SLI_SI915)
                                   | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
#endif
                                   ),
    .bt_feature_bit_map = 0,
    .ext_tcp_ip_feature_bit_map = (SL_SI91X_EXT_TCP_IP_TOTAL_SELECTS(10)
                                   | SL_SI91X_EXT_EMB_MQTT_ENABLE
                                   | SL_SI91X_EXT_FEAT_HTTP_OTAF_SUPPORT
                                   | SL_SI91X_CONFIG_FEAT_EXTENTION_VALID),
    .ble_feature_bit_map = 0,
    .ble_ext_feature_bit_map = 0,
    .config_feature_bit_map = 0,
  },
};

static sl_mqtt_client_configuration_t mqtt_client_configuration = {
  .auto_reconnect = true,
  .retry_count = 3,
  .minimum_back_off_time = 1,
  .maximum_back_off_time = 8,
  .is_clean_session = MQTT_CLEAN_SESSION,
  .mqt_version = SL_MQTT_VERSION_3_1,
  .client_port = M2M_MQTT_CLIENT_PORT,
  .client_id = (uint8_t *)M2M_MQTT_CLIENT_ID,
  .client_id_length = sizeof(M2M_MQTT_CLIENT_ID) - 1,
};

static const uint8_t m2m_last_will_payload[] =
  "{\"mqtt\":false,\"fw_version\":\"" M2M_FIRMWARE_VERSION "\"}";

static sl_mqtt_broker_t mqtt_broker_configuration = {
  .port = M2M_MQTT_BROKER_PORT,
  .is_connection_encrypted = MQTT_ENCRYPT_CONNECTION,
  .connect_timeout = M2M_MQTT_CONNECT_TIMEOUT_MS,
  .keep_alive_interval = M2M_MQTT_KEEPALIVE_SECONDS,
  .keep_alive_retries = MQTT_KEEPALIVE_RETRIES,
};

static sl_mqtt_client_last_will_message_t last_will_message = {
  .is_retained = true,
  .will_qos_level = SL_MQTT_QOS_LEVEL_1,
  .will_topic = (uint8_t *)M2M_TOPIC_DEVICE_STATUS,
  .will_topic_length = sizeof(M2M_TOPIC_DEVICE_STATUS) - 1,
  .will_message = (uint8_t *)m2m_last_will_payload,
  .will_message_length = sizeof(m2m_last_will_payload) - 1,
};

static uint32_t now_ms(void);
static void state_set_connectivity(bool wifi, bool mqtt);
static void state_set_voice_busy(bool busy);
static void state_set_voice_recording(bool recording);
static void state_set_fall_pending(bool pending, uint32_t deadline_ms);
static void state_set_button_pressed(bool pressed);
static void state_set_debug_led(bool on);
static void state_store_reminder(const char *payload);
static void state_set_ota(bool in_progress, const char *status, const char *target_version);
static void state_set_local_activity(local_activity_t activity);
static void queue_status_publish(void);
static void queue_cloud_payload(const char *topic, const char *payload, sl_mqtt_qos_t qos, bool retained);
static void queue_audio_prompt_internal(audio_prompt_type_t type, const char *text, bool use_cloud_speech);
static void queue_audio_prompt(audio_prompt_type_t type, const char *text);
static void queue_spoken_audio_prompt(audio_prompt_type_t type, const char *text);
static bool queue_voice_event(voice_event_type_t type);
static void request_status_publish(void);
static void queue_ota_status_publish(const char *phase,
                                     const char *detail,
                                     const char *url,
                                     const char *target_version);
static void publish_button_state(bool pressed);
static void publish_fall_event(const char *phase, const m2m_imu_sample_t *sample);
static void publish_fall_ack(const char *result);
static void mqtt_client_event_handler(void *client,
                                      sl_mqtt_client_event_t event,
                                      void *event_data,
                                      void *context);
static void mqtt_message_handler(void *client, sl_mqtt_client_message_t *message, void *context);
static sl_status_t wifi_start(void);
static sl_status_t mqtt_start(void);
static sl_status_t mqtt_subscribe_all(void);
static sl_status_t mqtt_publish_message(const cloud_tx_msg_t *queued_message);
static sl_status_t mqtt_stop(void);
static bool payload_is_truthy(const char *payload);
static void copy_text(char *dest, size_t dest_size, const char *src);
static bool append_text(char *dest, size_t dest_size, size_t *offset, const char *src);
static bool append_uint32_text(char *dest, size_t dest_size, size_t *offset, uint32_t value);
static void copy_mqtt_payload(char *dest, size_t dest_size, const sl_mqtt_client_message_t *message);
static void copy_mqtt_topic(char *dest, size_t dest_size, const sl_mqtt_client_message_t *message);
static void build_log_preview(const uint8_t *data, size_t data_length, char *dest, size_t dest_size);
static bool payload_contains(const char *payload, const char *needle);
static bool extract_json_string_value(const char *payload,
                                      const char *key,
                                      char *dest,
                                      size_t dest_size);
static bool extract_json_uint32_value(const char *payload, const char *key, uint32_t *value);
static size_t escape_json_string(char *dest, size_t dest_size, const char *src, size_t src_limit);
static bool parse_http_url(const char *url,
                           bool *use_tls,
                           char *host,
                           size_t host_size,
                           uint16_t *port,
                           char *resource,
                           size_t resource_size);
static bool parse_http_request_url(const char *url,
                                   bool *use_tls,
                                   char *host,
                                   size_t host_size,
                                   uint16_t *port,
                                   char *resource,
                                   size_t resource_size);
static bool queue_ota_update_from_payload(const char *payload);
static bool wait_for_mqtt_connection(uint32_t timeout_ms);
#if M2M_STARTUP_WELCOME_ENABLED
static bool wait_for_mqtt_ready(uint32_t timeout_ms);
#endif
static bool wait_for_voice_reply(const char *request_id, voice_reply_msg_t *reply, uint32_t timeout_ms);
static bool try_reserve_voice_session(uint32_t *request_number);
static void format_request_id(const char *prefix,
                              uint32_t request_number,
                              char *request_id,
                              size_t request_id_size);
static sl_status_t ensure_psram_media_ready(void);
static const char *button_event_name(button_event_type_t type);
static void print_wifi_profile_summary(void);
static void print_wifi_mac_address(void);
static void print_audio_capture_stats(const int16_t *pcm_samples, size_t sample_count);
static void run_dev_board_diagnostic(void);
#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
static sl_status_t wifi_scan_diagnostic_callback(sl_wifi_event_t event,
                                                 sl_wifi_scan_result_t *data,
                                                 uint32_t data_length,
                                                 void *optional_arg);
static sl_status_t run_wifi_scan_diagnostic(void);
#endif
static sl_status_t ensure_http_client_credentials(void);
static sl_status_t http_client_response_handler(const sl_http_client_t *client,
                                                sl_http_client_event_t event,
                                                void *data,
                                                void *request_context);
static sl_status_t http_wait_for_event(http_request_context_t *context, const char *url, uint32_t timeout_ms);
static sl_status_t http_execute_request(sl_http_client_method_type_t method,
                                        const char *url,
                                        const http_request_header_t *headers,
                                        size_t header_count,
                                        const uint8_t *body,
                                        size_t body_length,
                                        uint8_t *response,
                                        size_t response_capacity,
                                        size_t *response_length,
                                        uint32_t timeout_ms);
static sl_status_t http_execute_chunked_request(sl_http_client_method_type_t method,
                                                const char *url,
                                                const http_request_header_t *headers,
                                                size_t header_count,
                                                const http_body_segment_t *segments,
                                                size_t segment_count,
                                                size_t body_length,
                                                uint8_t *response,
                                                size_t response_capacity,
                                                size_t *response_length,
                                                uint32_t timeout_ms);
static sl_status_t http_upload_binary_chunked(const char *request_id,
                                              const char *kind,
                                              const char *filename,
                                              const char *content_type,
                                              const uint8_t *data,
                                              size_t data_length,
                                              char *uploaded_url,
                                              size_t uploaded_url_size);
static sl_status_t http_upload_audio_recording(const char *request_id,
                                               const int16_t *pcm_data,
                                               size_t sample_count,
                                               char *uploaded_url,
                                               size_t uploaded_url_size);
static sl_status_t http_download_binary(const char *url,
                                        uint8_t *buffer,
                                        size_t buffer_capacity,
                                        size_t *response_length,
                                        uint32_t timeout_ms);
static void play_boot_led_pattern(void);
static sl_status_t ota_fw_update_response_handler(sl_wifi_event_t event,
                                                  uint16_t *data,
                                                  uint32_t data_length,
                                                  void *arg);
static sl_status_t perform_http_ota_update(const ota_event_msg_t *ota_event);
static void mqtt_task(void *argument);
static void system_control_task(void *argument);
static void imu_task(void *argument);
static void voice_ai_task(void *argument);
static void reminder_audio_task(void *argument);
static void startup_diag_task(void *argument);
static void ota_task(void *argument);
static void dev_board_input_task(void *argument);
static void status_indicator_task(void *argument);
static void clear_voice_reply_queue(void);
static bool play_cloud_text_prompt(const char *prompt_tag, const char *speech_text);
static const char *sl_status_name(sl_status_t status);
static bool is_transient_mqtt_publish_status(sl_status_t status);
static const char *thread_state_name(osThreadState_t state);
static const char *wifi_start_phase_name(wifi_start_phase_t phase);
static void log_thread_snapshot(const char *tag, osThreadId_t thread_id);
static void log_heap_snapshot(const char *tag);
static const char *mqtt_client_event_name(sl_mqtt_client_event_t event);
static void wait_for_runtime_ready(void);
static void wifi_init_watchdog_start(void);
static void wifi_init_watchdog_stop(void);

void app_init(void)
{
  osThreadId_t thread_id;

  printf("[m2m] app_init begin\r\n");

  memset(&device_state, 0, sizeof(device_state));
  device_state.boot_tick_ms = now_ms();
  copy_text(device_state.ota_status, sizeof(device_state.ota_status), "idle");

  button_event_q = osMessageQueueNew(M2M_BUTTON_QUEUE_DEPTH, sizeof(button_event_t), NULL);
  sensor_event_q = osMessageQueueNew(M2M_SENSOR_QUEUE_DEPTH, sizeof(sensor_event_t), NULL);
  cloud_tx_q = osMessageQueueNew(M2M_CLOUD_TX_QUEUE_DEPTH, sizeof(cloud_tx_msg_t), NULL);
  audio_q = osMessageQueueNew(M2M_AUDIO_QUEUE_DEPTH, sizeof(audio_prompt_msg_t), NULL);
  voice_event_q = osMessageQueueNew(M2M_VOICE_QUEUE_DEPTH, sizeof(voice_event_msg_t), NULL);
  voice_reply_q = osMessageQueueNew(M2M_VOICE_QUEUE_DEPTH, sizeof(voice_reply_msg_t), NULL);
  ota_event_q = osMessageQueueNew(M2M_OTA_QUEUE_DEPTH, sizeof(ota_event_msg_t), NULL);
  system_flags = osEventFlagsNew(NULL);
  wifi_diag_flags = osEventFlagsNew(NULL);
  device_state_mutex = osMutexNew(NULL);

  if ((button_event_q == NULL) || (sensor_event_q == NULL) || (cloud_tx_q == NULL)
      || (audio_q == NULL) || (voice_event_q == NULL) || (voice_reply_q == NULL)
      || (ota_event_q == NULL) || (system_flags == NULL) || (wifi_diag_flags == NULL)
      || (device_state_mutex == NULL)) {
    printf("[m2m] RTOS object creation failed\r\n");
    return;
  }

  printf("[m2m] RTOS objects ready\r\n");
  (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_STARTUP_ACTIVE);
  log_heap_snapshot("heap-after-rtos-objects");
  wifi_init_watchdog_stop();
  printf("[m2m] psram init begin\r\n");
  if (ensure_psram_media_ready() != SL_STATUS_OK) {
    printf("[m2m] PSRAM media buffers unavailable\r\n");
  } else {
    printf("[m2m] psram init done\r\n");
  }

  printf("[m2m] dev_board init begin\r\n");
  m2m_dev_board_init();
  printf("[m2m] dev_board init done\r\n");
  log_heap_snapshot("heap-before-thread-create");

  thread_id = osThreadNew(status_indicator_task, NULL, &status_indicator_task_attributes);
  log_thread_snapshot("status-indicator-task-created", thread_id);
  log_heap_snapshot("heap-after-status-indicator-task");

  mqtt_task_thread_id = osThreadNew(mqtt_task, NULL, &mqtt_task_attributes);
  log_thread_snapshot("mqtt-task-created", mqtt_task_thread_id);
  log_heap_snapshot("heap-after-mqtt-task");

  thread_id = osThreadNew(system_control_task, NULL, &system_control_task_attributes);
  log_thread_snapshot("system-control-task-created", thread_id);
  log_heap_snapshot("heap-after-system-control-task");

  thread_id = osThreadNew(imu_task, NULL, &imu_task_attributes);
  log_thread_snapshot("imu-task-created", thread_id);
  log_heap_snapshot("heap-after-imu-task");

  voice_ai_task_thread_id = osThreadNew(voice_ai_task, NULL, &voice_ai_task_attributes);
  log_thread_snapshot("voice-ai-task-created", voice_ai_task_thread_id);
  log_heap_snapshot("heap-after-voice-ai-task");

  thread_id = osThreadNew(reminder_audio_task, NULL, &reminder_audio_task_attributes);
  log_thread_snapshot("reminder-audio-task-created", thread_id);
  log_heap_snapshot("heap-after-reminder-audio-task");

  thread_id = osThreadNew(startup_diag_task, NULL, &startup_diag_task_attributes);
  log_thread_snapshot("startup-diag-task-created", thread_id);
  log_heap_snapshot("heap-after-startup-diag-task");

  thread_id = osThreadNew(ota_task, NULL, &ota_task_attributes);
  log_thread_snapshot("ota-task-created", thread_id);
  log_heap_snapshot("heap-after-ota-task");

  thread_id = osThreadNew(dev_board_input_task, NULL, &dev_board_input_task_attributes);
  log_thread_snapshot("dev-board-input-task-created", thread_id);
  log_heap_snapshot("heap-after-dev-board-input-task");
}

void app_process_action(void)
{
}

static void fault_trap(const char *fault_name)
{
  __disable_irq();
  printf("[fault] %s current_thread=%p\r\n",
         (fault_name == NULL) ? "<unknown>" : fault_name,
         (void *)osThreadGetId());
  log_heap_snapshot("fault-heap");
  log_thread_snapshot("voice-ai-fault", voice_ai_task_thread_id);
  log_thread_snapshot("mqtt-task-fault", mqtt_task_thread_id);
  while (1) {
  }
}

void HardFault_Handler(void)
{
  fault_trap("HardFault");
}

void MemManage_Handler(void)
{
  fault_trap("MemManage");
}

void BusFault_Handler(void)
{
  fault_trap("BusFault");
}

void UsageFault_Handler(void)
{
  fault_trap("UsageFault");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  __disable_irq();
  printf("[rtos] stack overflow task=%s voice_ai_id=%p mqtt_task_id=%p\r\n",
         (pcTaskName == NULL) ? "<unknown>" : pcTaskName,
         (void *)voice_ai_task_thread_id,
         (void *)mqtt_task_thread_id);
  log_heap_snapshot("stack-overflow-heap");
  log_thread_snapshot("voice-ai-overflow", voice_ai_task_thread_id);
  log_thread_snapshot("mqtt-task-overflow", mqtt_task_thread_id);
  while (1) {
  }
}

void vApplicationMallocFailedHook(void)
{
  __disable_irq();
  printf("[rtos] malloc failed voice_ai_id=%p mqtt_task_id=%p\r\n",
         (void *)voice_ai_task_thread_id,
         (void *)mqtt_task_thread_id);
  log_heap_snapshot("malloc-failed-heap");
  log_thread_snapshot("voice-ai-malloc-failed", voice_ai_task_thread_id);
  log_thread_snapshot("mqtt-task-malloc-failed", mqtt_task_thread_id);
  while (1) {
  }
}

static uint32_t now_ms(void)
{
  uint32_t freq = osKernelGetTickFreq();
  uint32_t tick = osKernelGetTickCount();

  if (freq == 0U) {
    return tick;
  }

  return (uint32_t)(((uint64_t)tick * 1000ULL) / freq);
}

static const char *button_event_name(button_event_type_t type)
{
  switch (type) {
    case BUTTON_EVENT_PRESSED:
      return "pressed";
    case BUTTON_EVENT_RELEASED:
      return "released";
    case BUTTON_EVENT_LONG_PRESS:
      return "long_press";
    default:
      return "unknown";
  }
}

static size_t align_up(size_t value, size_t alignment)
{
  if (alignment == 0U) {
    return value;
  }

  return (value + alignment - 1U) & ~(alignment - 1U);
}

static void write_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_le32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8) & 0xFFU);
  data[2] = (uint8_t)((value >> 16) & 0xFFU);
  data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void build_wav_header(uint8_t *header, size_t pcm_bytes, uint32_t sample_rate_hz)
{
  if (header == NULL) {
    return;
  }

  memcpy(header, "RIFF", 4);
  write_le32(header + 4, (uint32_t)(pcm_bytes + 36U));
  memcpy(header + 8, "WAVEfmt ", 8);
  write_le32(header + 16, 16U);
  write_le16(header + 20, 1U);
  write_le16(header + 22, M2M_AUDIO_CAPTURE_CHANNEL_COUNT);
  write_le32(header + 24, sample_rate_hz);
  write_le32(header + 28, sample_rate_hz * M2M_AUDIO_CAPTURE_CHANNEL_COUNT * sizeof(int16_t));
  write_le16(header + 32, (uint16_t)(M2M_AUDIO_CAPTURE_CHANNEL_COUNT * sizeof(int16_t)));
  write_le16(header + 34, M2M_AUDIO_CAPTURE_BITS_PER_SAMPLE);
  memcpy(header + 36, "data", 4);
  write_le32(header + 40, (uint32_t)pcm_bytes);
}

static void print_audio_capture_stats(const int16_t *pcm_samples, size_t sample_count)
{
  int32_t peak_abs = 0;
  int64_t sum_abs = 0;
  size_t nonzero_samples = 0U;

  if ((pcm_samples == NULL) || (sample_count == 0U)) {
    printf("[diag] mic stats unavailable (empty capture)\r\n");
    return;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    int32_t sample = (int32_t)pcm_samples[i];
    int32_t abs_sample = (sample < 0) ? -sample : sample;

    if (abs_sample > peak_abs) {
      peak_abs = abs_sample;
    }
    if (abs_sample != 0) {
      nonzero_samples++;
    }
    sum_abs += abs_sample;
  }

  printf("[diag] mic stats samples=%lu peak_abs=%ld avg_abs=%ld nonzero=%lu\r\n",
         (unsigned long)sample_count,
         (long)peak_abs,
         (long)(sum_abs / (int64_t)sample_count),
         (unsigned long)nonzero_samples);
}

static void run_dev_board_diagnostic(void)
{
  static const bool stop_requested = false;
  bool stopped_by_button = false;
  size_t captured_samples = 0U;
  size_t image_size = 0U;
  m2m_imu_sample_t imu_sample = { 0 };
  bool imu_ok;
  bool mic_ok;
  bool cam_ok;
  bool button_pressed = m2m_dev_board_read_ptt_button();

  printf("[diag] startup self-test begin\r\n");
  printf("[diag] button idle_state=%s\r\n", button_pressed ? "pressed" : "released");

  printf("[diag] imu test begin\r\n");
  imu_ok = m2m_dev_board_run_imu_self_test(&imu_sample);
  if (imu_ok) {
    printf("[diag] imu test result=ok accel_mg=[%d,%d,%d] gyro_dps=[%d,%d,%d]\r\n",
           imu_sample.accel_mg[0],
           imu_sample.accel_mg[1],
           imu_sample.accel_mg[2],
           imu_sample.gyro_dps[0],
           imu_sample.gyro_dps[1],
           imu_sample.gyro_dps[2]);
  } else {
    printf("[diag] imu test result=fail\r\n");
  }

  printf("[diag] speaker test begin prompt=speaker_self_test\r\n");
  printf("[diag] speaker test result=%s\r\n", m2m_dev_board_run_speaker_self_test() ? "ok" : "fail");
  printf("[diag] speaker test end\r\n");

  if (psram_media.recording_pcm != NULL) {
    printf("[diag] mic test begin duration_ms=500\r\n");
    mic_ok = m2m_dev_board_capture_audio_pcm(psram_media.recording_pcm,
                                             M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ / 2U,
                                             500U,
                                             &stop_requested,
                                             &stopped_by_button,
                                             &captured_samples);
    printf("[diag] mic test result=%s stopped_by_button=%s\r\n",
           mic_ok ? "ok" : "fail",
           stopped_by_button ? "yes" : "no");
    print_audio_capture_stats(psram_media.recording_pcm, captured_samples);
  } else {
    printf("[diag] mic test skipped (record buffer unavailable)\r\n");
  }

  if (psram_media.image_buffer != NULL) {
    printf("[diag] camera test begin\r\n");
    cam_ok = m2m_dev_board_capture_image(psram_media.image_buffer,
                                         psram_media.image_buffer_capacity,
                                         &image_size);
    if (cam_ok) {
      bool jpeg_header_ok = (image_size >= 4U)
                            && (psram_media.image_buffer[0] == 0xFFU)
                            && (psram_media.image_buffer[1] == 0xD8U);
      printf("[diag] camera test result=ok bytes=%lu jpeg_soi=%s first4=%02X %02X %02X %02X\r\n",
             (unsigned long)image_size,
             jpeg_header_ok ? "yes" : "no",
             psram_media.image_buffer[0],
             psram_media.image_buffer[1],
             psram_media.image_buffer[2],
             psram_media.image_buffer[3]);
    } else {
      printf("[diag] camera test result=fail\r\n");
    }
  } else {
    printf("[diag] camera test skipped (image buffer unavailable)\r\n");
  }

  printf("[diag] startup self-test end\r\n");
}

static const char *wifi_security_name(sl_wifi_security_t security)
{
  switch (security) {
    case SL_WIFI_OPEN:
      return "open";
    case SL_WIFI_WPA:
      return "wpa";
    case SL_WIFI_WPA2:
      return "wpa2";
    case SL_WIFI_WPA_WPA2_MIXED:
      return "wpa/wpa2_mixed";
    case SL_WIFI_WPA3:
      return "wpa3";
    case SL_WIFI_WPA3_TRANSITION:
      return "wpa3_transition";
    default:
      return "unknown";
  }
}

static const char *wifi_band_name(sl_wifi_band_t band)
{
  switch (band) {
    case SL_WIFI_BAND_2_4GHZ:
      return "2.4GHz";
    case SL_WIFI_BAND_5GHZ:
      return "5GHz";
    case SL_WIFI_BAND_DUAL:
      return "dual";
    case SL_WIFI_AUTO_BAND:
      return "auto";
    default:
      return "unknown";
  }
}

static const char *wifi_device_band_name(uint8_t band)
{
  switch (band) {
    case SL_SI91X_WIFI_BAND_2_4GHZ:
      return "2.4GHz";
    case SL_SI91X_WIFI_BAND_5GHZ:
      return "5GHz";
    default:
      return "unknown";
  }
}

static const char *sl_status_name(sl_status_t status)
{
  switch (status) {
    case SL_STATUS_OK:
      return "ok";
    case SL_STATUS_FAIL:
      return "fail";
    case SL_STATUS_INVALID_STATE:
      return "invalid_state";
    case SL_STATUS_NOT_READY:
      return "not_ready";
    case SL_STATUS_BUSY:
      return "busy";
    case SL_STATUS_IN_PROGRESS:
      return "in_progress";
    case SL_STATUS_ABORT:
      return "abort";
    case SL_STATUS_TIMEOUT:
      return "timeout";
    case SL_STATUS_PERMISSION:
      return "permission";
    case SL_STATUS_WOULD_BLOCK:
      return "would_block";
    case SL_STATUS_IDLE:
      return "idle";
    case SL_STATUS_INVALID_PARAMETER:
      return "invalid_parameter";
    default:
      return "unknown";
  }
}

static bool is_transient_mqtt_publish_status(sl_status_t status)
{
  return (status == SL_STATUS_TIMEOUT) || (status == SL_STATUS_BUSY) || (status == SL_STATUS_NOT_READY)
         || (status == SL_STATUS_WOULD_BLOCK);
}

static const char *thread_state_name(osThreadState_t state)
{
  switch (state) {
    case osThreadInactive:
      return "inactive";
    case osThreadReady:
      return "ready";
    case osThreadRunning:
      return "running";
    case osThreadBlocked:
      return "blocked";
    case osThreadTerminated:
      return "terminated";
    case osThreadError:
      return "error";
    default:
      return "unknown";
  }
}

static const char *wifi_start_phase_name(wifi_start_phase_t phase)
{
  switch (phase) {
    case WIFI_START_PHASE_IDLE:
      return "idle";
    case WIFI_START_PHASE_SETTLE:
      return "settle";
    case WIFI_START_PHASE_NET_INIT_BEGIN:
      return "net_init_begin";
    case WIFI_START_PHASE_NET_INIT_DONE:
      return "net_init_done";
    case WIFI_START_PHASE_SCAN:
      return "scan";
    case WIFI_START_PHASE_CONNECTING:
      return "connecting";
    case WIFI_START_PHASE_CONNECTED:
      return "connected";
    case WIFI_START_PHASE_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

static void log_thread_snapshot(const char *tag, osThreadId_t thread_id)
{
  const char *name = NULL;

  if (thread_id == NULL) {
    printf("[%s] thread=<null>\r\n", (tag == NULL) ? "thread" : tag);
    return;
  }

  name = osThreadGetName(thread_id);
  printf("[%s] name=%s id=%p state=%s priority=%d stack_free=%lu\r\n",
         (tag == NULL) ? "thread" : tag,
         (name == NULL) ? "<unnamed>" : name,
         (void *)thread_id,
         thread_state_name(osThreadGetState(thread_id)),
         (int)osThreadGetPriority(thread_id),
         (unsigned long)osThreadGetStackSpace(thread_id));
}

static void log_heap_snapshot(const char *tag)
{
  printf("[%s] free=%lu min_ever=%lu\r\n",
         (tag == NULL) ? "heap" : tag,
         (unsigned long)xPortGetFreeHeapSize(),
         (unsigned long)xPortGetMinimumEverFreeHeapSize());
}

static void wait_for_runtime_ready(void)
{
  if (system_flags == NULL) {
    return;
  }

  (void)osEventFlagsWait(system_flags,
                         SYSTEM_FLAG_RUNTIME_READY,
                         osFlagsWaitAny,
                         osWaitForever);
}

static void wifi_init_watchdog_start(void)
{
  RSI_WWDT_Init(MCU_WDT);
  RSI_WWDT_IntrMask();
  RSI_WWDT_IntrClear();
  RSI_WWDT_ConfigWindowTimer(MCU_WDT, 0U);
  RSI_WWDT_ConfigIntrTimer(MCU_WDT, M2M_WIFI_INIT_WWDT_INTERRUPT_DELAY);
  RSI_WWDT_ConfigSysRstTimer(MCU_WDT, M2M_WIFI_INIT_WWDT_RESET_DELAY);
  RSI_WWDT_SysRstOnProcLockEnable(MCU_WDT);
  RSI_WWDT_Start(MCU_WDT);
  printf("[wifi] hardware watchdog armed intr_delay=%u reset_delay=%u\r\n",
         (unsigned int)M2M_WIFI_INIT_WWDT_INTERRUPT_DELAY,
         (unsigned int)M2M_WIFI_INIT_WWDT_RESET_DELAY);
}

static void wifi_init_watchdog_stop(void)
{
  RSI_WWDT_Init(MCU_WDT);
  RSI_WWDT_DeInit(MCU_WDT);
  RSI_WWDT_IntrClear();
  RSI_WWDT_IntrMask();
  printf("[wifi] hardware watchdog stopped phase=%s\r\n",
         wifi_start_phase_name((wifi_start_phase_t)wifi_start_phase));
}

static const char *wifi_status_name(sl_status_t status)
{
  switch (status) {
    case SL_STATUS_OK:
      return "ok";
    case SL_STATUS_BUSY:
      return "busy";
    case SL_STATUS_IN_PROGRESS:
      return "in_progress";
    case SL_STATUS_SI91X_NO_AP_FOUND:
      return "no_ap_found";
    case SL_STATUS_INVALID_CREDENTIALS:
      return "invalid_credentials";
    case SL_STATUS_WIFI_INVALID_ENCRYPTION_METHOD:
      return "invalid_encryption";
    case SL_STATUS_INVALID_CONFIGURATION:
      return "invalid_configuration";
    case SL_STATUS_SI91X_INVALID_SECURITY_MODE_IN_JOIN_COMMAND:
      return "invalid_security_mode";
    case SL_STATUS_SI91X_INVALID_PSK_LENGTH:
      return "invalid_psk_length";
    default:
      return "unknown";
  }
}

static const char *mqtt_client_event_name(sl_mqtt_client_event_t event)
{
  switch (event) {
    case SL_MQTT_CLIENT_CONNECTED_EVENT:
      return "connected";
    case SL_MQTT_CLIENT_DISCONNECTED_EVENT:
      return "disconnected";
    case SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT:
      return "message_published";
    case SL_MQTT_CLIENT_MESSAGED_RECEIVED_EVENT:
      return "message_received";
    case SL_MQTT_CLIENT_SUBSCRIBED_EVENT:
      return "subscribed";
    case SL_MQTT_CLIENT_UNSUBSCRIBED_EVENT:
      return "unsubscribed";
    case SL_MQTT_CLIENT_ERROR_EVENT:
      return "error";
    default:
      return "unknown";
  }
}

#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
static void wifi_format_scan_ssid(char *dest, size_t dest_size, const uint8_t *src, size_t src_size)
{
  size_t index = 0;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  if (src == NULL) {
    dest[0] = '\0';
    return;
  }

  while ((index + 1U < dest_size) && (index < src_size) && (src[index] != '\0')) {
    dest[index] = (char)src[index];
    index++;
  }

  dest[index] = '\0';
}

static sl_status_t wifi_scan_diagnostic_callback(sl_wifi_event_t event,
                                                 sl_wifi_scan_result_t *data,
                                                 uint32_t data_length,
                                                 void *optional_arg)
{
  UNUSED_PARAMETER(data_length);
  UNUSED_PARAMETER(optional_arg);

  if (SL_WIFI_CHECK_IF_EVENT_FAILED(event)) {
    wifi_scan_diag_status = (data != NULL) ? *((sl_status_t *)data) : SL_STATUS_FAIL;
  } else {
    wifi_scan_diag_status = SL_STATUS_OK;
  }

  if (wifi_diag_flags != NULL) {
    (void)osEventFlagsSet(wifi_diag_flags, WIFI_DIAG_FLAG_SCAN_DONE);
  }

  return SL_STATUS_OK;
}

static sl_status_t run_wifi_scan_diagnostic(void)
{
  sl_status_t status;
  uint32_t flags;
  uint16_t result_count = 0;
  bool target_found     = false;
  sl_wifi_scan_configuration_t scan_configuration = default_wifi_scan_configuration;
  sl_wifi_extended_scan_result_parameters_t scan_parameters = {
    .scan_results         = wifi_scan_diag_results,
    .array_length         = (uint16_t)M2M_WIFI_SCAN_MAX_APS,
    .result_count         = &result_count,
    .channel_filter       = NULL,
    .security_mode_filter = NULL,
    .rssi_filter          = NULL,
    .network_type_filter  = NULL,
  };

  memset(wifi_scan_diag_results, 0, sizeof(wifi_scan_diag_results));
  scan_configuration.type = SL_WIFI_SCAN_TYPE_EXTENDED;

  printf("[wifi] scan begin target=%s profile_channel=%u region=%u\r\n",
         DEFAULT_WIFI_CLIENT_PROFILE_SSID,
         (unsigned int)DEFAULT_WIFI_CLIENT_PROFILE.config.channel.channel,
         (unsigned int)wifi_client_configuration.region_code);

  wifi_scan_diag_status = SL_STATUS_IN_PROGRESS;
  (void)osEventFlagsClear(wifi_diag_flags, WIFI_DIAG_FLAG_SCAN_DONE);

  status = sl_wifi_set_scan_callback(wifi_scan_diagnostic_callback, NULL);
  if (status != SL_STATUS_OK) {
    printf("[wifi] scan callback setup failed: 0x%lx (%s)\r\n",
           (unsigned long)status,
           wifi_status_name(status));
    printf("[wifi] scan skipped, continuing with join\r\n");
    return status;
  }

  status = sl_wifi_start_scan(SL_WIFI_CLIENT_INTERFACE, NULL, &scan_configuration);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
    printf("[wifi] scan start failed: 0x%lx (%s)\r\n",
           (unsigned long)status,
           wifi_status_name(status));
    (void)sl_wifi_set_scan_callback(NULL, NULL);
    printf("[wifi] scan skipped, continuing with join\r\n");
    return status;
  }

  if (status == SL_STATUS_IN_PROGRESS) {
    printf("[wifi] scan started asynchronously\r\n");
  }

  flags = osEventFlagsWait(wifi_diag_flags,
                           WIFI_DIAG_FLAG_SCAN_DONE,
                           osFlagsWaitAny,
                           M2M_WIFI_SCAN_TIMEOUT_MS);
  if ((flags & osFlagsError) != 0U) {
    printf("[wifi] scan wait timed out after %u ms\r\n", (unsigned int)M2M_WIFI_SCAN_TIMEOUT_MS);
    (void)sl_wifi_set_scan_callback(NULL, NULL);
    printf("[wifi] scan timed out, continuing with join\r\n");
    return SL_STATUS_TIMEOUT;
  }

  if (wifi_scan_diag_status != SL_STATUS_OK) {
    printf("[wifi] scan failed: 0x%lx (%s)\r\n",
           (unsigned long)wifi_scan_diag_status,
           wifi_status_name(wifi_scan_diag_status));
    (void)sl_wifi_set_scan_callback(NULL, NULL);
    printf("[wifi] scan failed, continuing with join\r\n");
    return wifi_scan_diag_status;
  }

  status = sl_wifi_get_stored_scan_results(SL_WIFI_CLIENT_INTERFACE, &scan_parameters);
  if (status != SL_STATUS_OK) {
    printf("[wifi] scan result fetch failed: 0x%lx (%s)\r\n",
           (unsigned long)status,
           wifi_status_name(status));
    (void)sl_wifi_set_scan_callback(NULL, NULL);
    printf("[wifi] scan result fetch failed, continuing with join\r\n");
    return status;
  }

  printf("[wifi] scan results count=%u%s\r\n",
         (unsigned int)result_count,
         (result_count >= (uint16_t)M2M_WIFI_SCAN_MAX_APS) ? " (truncated)" : "");

  for (uint16_t index = 0; index < result_count; ++index) {
    char ssid[SL_WIFI_MAX_SSID_LENGTH + 1U];
    const sl_wifi_extended_scan_result_t *scan_result = &wifi_scan_diag_results[index];

    wifi_format_scan_ssid(ssid, sizeof(ssid), scan_result->ssid, sizeof(scan_result->ssid));
    if (ssid[0] == '\0') {
      copy_text(ssid, sizeof(ssid), "<hidden>");
    }

    printf("[wifi] ap[%u] ssid=%s ch=%u rssi=%d sec=%s bssid=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
           (unsigned int)index,
           ssid,
           (unsigned int)scan_result->rf_channel,
           (int)((int8_t)scan_result->rssi),
           wifi_security_name((sl_wifi_security_t)scan_result->security_mode),
           scan_result->bssid[0],
           scan_result->bssid[1],
           scan_result->bssid[2],
           scan_result->bssid[3],
           scan_result->bssid[4],
           scan_result->bssid[5]);

    if (strcmp(ssid, DEFAULT_WIFI_CLIENT_PROFILE_SSID) == 0) {
      target_found = true;
    }
  }

  if (target_found) {
    printf("[wifi] target_ssid_seen=yes ssid=%s\r\n", DEFAULT_WIFI_CLIENT_PROFILE_SSID);
  } else {
    printf("[wifi] target_ssid_seen=no ssid=%s\r\n", DEFAULT_WIFI_CLIENT_PROFILE_SSID);
  }

  (void)sl_wifi_set_scan_callback(NULL, NULL);
  return SL_STATUS_OK;
}
#endif

static void print_wifi_failure_hint(sl_status_t status)
{
  if (status == SL_STATUS_SI91X_NO_AP_FOUND) {
    printf("[wifi] hint: target ssid was not found in scan/join\r\n");
    printf("[wifi] hint: current device radio band=%s, profile band=%s, channel=%u, security=%s\r\n",
           wifi_device_band_name(wifi_client_configuration.band),
           wifi_band_name(DEFAULT_WIFI_CLIENT_PROFILE.config.channel.band),
           (unsigned int)DEFAULT_WIFI_CLIENT_PROFILE.config.channel.channel,
           wifi_security_name(DEFAULT_WIFI_CLIENT_PROFILE.config.security));
    printf("[wifi] hint: verify AP is broadcasting 2.4GHz, SSID matches exactly, and signal is nearby\r\n");
    printf("[wifi] hint: if 2312 is a phone hotspot, force 2.4GHz / Maximize Compatibility and retry\r\n");
  }
}

static void print_wifi_profile_summary(void)
{
#ifdef M2M_WIFI_ACTIVE_PROFILE_NAME
  printf("[wifi] profile=%s ssid=%s security=%s profile_band=%s channel=%u device_band=%s region=%u\r\n",
         M2M_WIFI_ACTIVE_PROFILE_NAME,
         DEFAULT_WIFI_CLIENT_PROFILE_SSID,
         wifi_security_name(DEFAULT_WIFI_CLIENT_PROFILE.config.security),
         wifi_band_name(DEFAULT_WIFI_CLIENT_PROFILE.config.channel.band),
         (unsigned int)DEFAULT_WIFI_CLIENT_PROFILE.config.channel.channel,
         wifi_device_band_name(wifi_client_configuration.band),
         (unsigned int)wifi_client_configuration.region_code);
#else
  printf("[wifi] ssid=%s security=%s profile_band=%s channel=%u device_band=%s region=%u\r\n",
         DEFAULT_WIFI_CLIENT_PROFILE_SSID,
         wifi_security_name(DEFAULT_WIFI_CLIENT_PROFILE.config.security),
         wifi_band_name(DEFAULT_WIFI_CLIENT_PROFILE.config.channel.band),
         (unsigned int)DEFAULT_WIFI_CLIENT_PROFILE.config.channel.channel,
         wifi_device_band_name(wifi_client_configuration.band),
         (unsigned int)wifi_client_configuration.region_code);
#endif
}

static void print_wifi_mac_address(void)
{
  sl_mac_address_t mac = { 0 };
  sl_status_t status = sl_wifi_get_mac_address(SL_WIFI_CLIENT_INTERFACE, &mac);

  if (status != SL_STATUS_OK) {
    printf("[wifi] get mac failed: 0x%lx\r\n", (unsigned long)status);
    return;
  }

  printf("[wifi] client_mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
         mac.octet[0],
         mac.octet[1],
         mac.octet[2],
         mac.octet[3],
         mac.octet[4],
         mac.octet[5]);
}

static sl_status_t ensure_psram_media_ready(void)
{
  size_t reply_offset;
  size_t total_required;
  size_t free_bytes;
  sl_psram_return_type_t psram_status;
  uint8_t *psram_base = (uint8_t *)PSRAM_BASE_ADDRESS;

  if (psram_media.ready) {
    return SL_STATUS_OK;
  }

  (void)sl_si91x_psram_uninit();
  psram_status = sl_si91x_psram_init();
  if (psram_status != PSRAM_SUCCESS) {
    printf("[psram] init failed: %d\r\n", (int)psram_status);
    return SL_STATUS_FAIL;
  }

  psram_media.recording_pcm = (int16_t *)psram_base;
  psram_media.recording_pcm_capacity_samples = M2M_VOICE_RECORDING_MAX_PCM_SAMPLES;

  psram_media.image_buffer =
    psram_base + align_up(M2M_VOICE_RECORDING_MAX_PCM_BYTES, M2M_PSRAM_ALIGN_BYTES);
  psram_media.image_buffer_capacity = M2M_DEV_BOARD_CAPTURED_IMAGE_MAX_BYTES;

  reply_offset = align_up(M2M_VOICE_RECORDING_MAX_PCM_BYTES + M2M_DEV_BOARD_CAPTURED_IMAGE_MAX_BYTES,
                          M2M_PSRAM_ALIGN_BYTES);
  psram_media.reply_audio_buffer = psram_base + reply_offset;
  psram_media.reply_audio_capacity = M2M_HTTP_DOWNLOAD_MAX_BYTES;

  total_required = reply_offset + psram_media.reply_audio_capacity;
  if (total_required > M2M_PSRAM_TOTAL_BYTES) {
    printf("[psram] layout exceeds capacity required=%lu\r\n", (unsigned long)total_required);
    return SL_STATUS_NO_MORE_RESOURCE;
  }

  psram_media.ready = true;
  free_bytes = M2M_PSRAM_TOTAL_BYTES - total_required;
  printf("[psram] ready base=0x%08lx record=%luKB image=%luKB reply=%luKB free=%luKB\r\n",
         (unsigned long)PSRAM_BASE_ADDRESS,
         (unsigned long)(M2M_VOICE_RECORDING_MAX_PCM_BYTES / 1024U),
         (unsigned long)(psram_media.image_buffer_capacity / 1024U),
         (unsigned long)(psram_media.reply_audio_capacity / 1024U),
         (unsigned long)(free_bytes / 1024U));
  return SL_STATUS_OK;
}

static void state_set_connectivity(bool wifi, bool mqtt)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.wifi_connected = wifi;
    device_state.mqtt_connected = mqtt;
    (void)osMutexRelease(device_state_mutex);
  }

  if (wifi) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_WIFI_CONNECTED);
  } else {
    (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_WIFI_CONNECTED);
  }

  if (mqtt) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_MQTT_CONNECTED);
  } else {
    (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_MQTT_CONNECTED);
  }
}

static void state_set_voice_busy(bool busy)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.voice_busy = busy;
    (void)osMutexRelease(device_state_mutex);
  }

  if (busy) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_VOICE_ACTIVE);
  } else {
    (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_VOICE_ACTIVE);
  }
}

static bool try_reserve_voice_session(uint32_t *request_number)
{
  uint32_t allocated_request_number = now_ms();
  bool reserved = false;

  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    if (!device_state.voice_busy) {
      device_state.voice_busy = true;
      device_state.request_counter++;
      allocated_request_number = device_state.request_counter;
      reserved = true;
    }
    (void)osMutexRelease(device_state_mutex);
  }

  if (reserved) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_VOICE_ACTIVE);
  }

  if (request_number != NULL) {
    *request_number = allocated_request_number;
  }

  return reserved;
}

static void format_request_id(const char *prefix,
                              uint32_t request_number,
                              char *request_id,
                              size_t request_id_size)
{
  const char *safe_prefix = (prefix == NULL) ? "req" : prefix;

  if ((request_id == NULL) || (request_id_size == 0U)) {
    return;
  }

  (void)snprintf(request_id, request_id_size, "%s_%" PRIu32, safe_prefix, request_number);
}

static void state_set_voice_recording(bool recording)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.voice_recording = recording;
    (void)osMutexRelease(device_state_mutex);
  }

  if (recording) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_VOICE_RECORDING);
  } else {
    (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_VOICE_RECORDING);
  }
}

static void state_set_fall_pending(bool pending, uint32_t deadline_ms)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.fall_pending = pending;
    device_state.fall_deadline_ms = deadline_ms;
    (void)osMutexRelease(device_state_mutex);
  }

  if (pending) {
    (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_FALL_PENDING);
  } else {
    (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_FALL_PENDING);
  }
}

static void state_set_button_pressed(bool pressed)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.debug_button_pressed = pressed;
    (void)osMutexRelease(device_state_mutex);
  }
}

static void state_set_debug_led(bool on)
{
  m2m_dev_board_set_debug_led(on);

  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.debug_led_on = on;
    (void)osMutexRelease(device_state_mutex);
  }
}

static void state_store_reminder(const char *payload)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    copy_text(device_state.latest_reminder, sizeof(device_state.latest_reminder), payload);
    (void)osMutexRelease(device_state_mutex);
  }
}

static void state_set_ota(bool in_progress, const char *status, const char *target_version)
{
  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    device_state.ota_in_progress = in_progress;
    copy_text(device_state.ota_status, sizeof(device_state.ota_status), status);
    copy_text(device_state.ota_target_version, sizeof(device_state.ota_target_version), target_version);
    (void)osMutexRelease(device_state_mutex);
  }
}

static void state_set_local_activity(local_activity_t activity)
{
  local_activity = activity;
}

static const char *led_status_name(led_status_t status)
{
  switch (status) {
    case LED_STATUS_BOOT:
      return "boot_init";
    case LED_STATUS_WIFI_CONNECTING:
      return "wifi_connecting";
    case LED_STATUS_WIFI_FAILED:
      return "wifi_failed";
    case LED_STATUS_MQTT_CONNECTING:
      return "mqtt_connecting";
    case LED_STATUS_READY:
      return "ready";
    case LED_STATUS_VOICE_RECORDING:
      return "voice_recording";
    case LED_STATUS_UPLOADING:
      return "uploading";
    case LED_STATUS_WAITING_REPLY:
      return "waiting_reply";
    case LED_STATUS_DOWNLOADING:
      return "downloading";
    case LED_STATUS_PLAYING_AUDIO:
      return "playing_audio";
    case LED_STATUS_OTA:
      return "ota";
    case LED_STATUS_ERROR:
      return "error";
    default:
      return "unknown";
  }
}

static led_status_t indicator_select_led_status(void)
{
  device_state_t snapshot;
  local_activity_t activity = local_activity;
  uint32_t flags = osEventFlagsGet(system_flags);

  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    snapshot = device_state;
    (void)osMutexRelease(device_state_mutex);
  } else {
    memset(&snapshot, 0, sizeof(snapshot));
  }

  if ((activity == LOCAL_ACTIVITY_ERROR) || (wifi_start_phase == WIFI_START_PHASE_FAILED)) {
    return LED_STATUS_ERROR;
  }

  if ((activity == LOCAL_ACTIVITY_OTA) || snapshot.ota_in_progress || ota_exclusive_mode) {
    return LED_STATUS_OTA;
  }

  switch (activity) {
    case LOCAL_ACTIVITY_VOICE_RECORDING:
      return LED_STATUS_VOICE_RECORDING;
    case LOCAL_ACTIVITY_UPLOADING:
      return LED_STATUS_UPLOADING;
    case LOCAL_ACTIVITY_WAITING_REPLY:
      return LED_STATUS_WAITING_REPLY;
    case LOCAL_ACTIVITY_DOWNLOADING:
      return LED_STATUS_DOWNLOADING;
    case LOCAL_ACTIVITY_PLAYING_AUDIO:
      return LED_STATUS_PLAYING_AUDIO;
    default:
      break;
  }

  if (((flags & SYSTEM_FLAG_STARTUP_ACTIVE) != 0U)
      && ((wifi_start_phase == WIFI_START_PHASE_IDLE) || (wifi_start_phase == WIFI_START_PHASE_SETTLE))) {
    return LED_STATUS_BOOT;
  }

  if (!snapshot.wifi_connected) {
    return LED_STATUS_WIFI_CONNECTING;
  }

  if (!snapshot.mqtt_connected || !mqtt_subscribed) {
    return LED_STATUS_MQTT_CONNECTING;
  }

  if ((flags & SYSTEM_FLAG_STARTUP_ACTIVE) != 0U) {
    return LED_STATUS_BOOT;
  }

  return LED_STATUS_READY;
}

static bool led_phase_in_window(uint32_t phase_ms, uint32_t start_ms, uint32_t duration_ms)
{
  return (phase_ms >= start_ms) && (phase_ms < (start_ms + duration_ms));
}

static bool led_status_is_on(led_status_t status, uint32_t elapsed_ms)
{
  uint32_t phase_ms;

  switch (status) {
    case LED_STATUS_BOOT:
      phase_ms = elapsed_ms % 200U;
      return phase_ms < 100U;

    case LED_STATUS_WIFI_CONNECTING:
      phase_ms = elapsed_ms % 1000U;
      return phase_ms < 150U;

    case LED_STATUS_MQTT_CONNECTING:
      phase_ms = elapsed_ms % 1600U;
      return led_phase_in_window(phase_ms, 0U, 120U)
             || led_phase_in_window(phase_ms, 260U, 120U);

    case LED_STATUS_READY:
      phase_ms = elapsed_ms % 3000U;
      return phase_ms < 80U;

    case LED_STATUS_VOICE_RECORDING:
      phase_ms = elapsed_ms % 200U;
      return phase_ms < 100U;

    case LED_STATUS_UPLOADING:
      phase_ms = elapsed_ms % 500U;
      return phase_ms < 250U;

    case LED_STATUS_WAITING_REPLY:
      phase_ms = elapsed_ms % 1200U;
      return led_phase_in_window(phase_ms, 0U, 100U)
             || led_phase_in_window(phase_ms, 220U, 100U)
             || led_phase_in_window(phase_ms, 440U, 100U);

    case LED_STATUS_DOWNLOADING:
      phase_ms = elapsed_ms % 700U;
      return led_phase_in_window(phase_ms, 0U, 120U)
             || led_phase_in_window(phase_ms, 250U, 120U);

    case LED_STATUS_PLAYING_AUDIO:
      phase_ms = elapsed_ms % 1000U;
      return phase_ms < 650U;

    case LED_STATUS_OTA:
      phase_ms = elapsed_ms % 1600U;
      return led_phase_in_window(phase_ms, 0U, 100U)
             || led_phase_in_window(phase_ms, 220U, 100U)
             || led_phase_in_window(phase_ms, 440U, 100U)
             || led_phase_in_window(phase_ms, 660U, 100U);

    case LED_STATUS_WIFI_FAILED:
    case LED_STATUS_ERROR:
    default:
      phase_ms = elapsed_ms % 1000U;
      return phase_ms < 500U;
  }
}

static void queue_status_publish(void)
{
  cloud_tx_msg_t message = { 0 };
  device_state_t snapshot;
  uint32_t uptime_s;

  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    snapshot = device_state;
    (void)osMutexRelease(device_state_mutex);
  } else {
    memset(&snapshot, 0, sizeof(snapshot));
  }

  uptime_s = (now_ms() - snapshot.boot_tick_ms) / 1000U;
  message.topic = M2M_TOPIC_DEVICE_STATUS;
  message.qos = SL_MQTT_QOS_LEVEL_0;
  message.retained = false;

  (void)snprintf(message.payload,
                 sizeof(message.payload),
                 "{\"wifi\":%s,\"mqtt\":%s,\"battery_pct\":%u,"
                 "\"voice_busy\":%s,\"voice_recording\":%s,"
                 "\"fall_pending\":%s,\"debug_led\":%s,"
                 "\"ota_in_progress\":%s,\"ota_status\":\"%s\","
                 "\"fw_version\":\"%s\",\"uptime_s\":%" PRIu32 "}",
                 snapshot.wifi_connected ? "true" : "false",
                 snapshot.mqtt_connected ? "true" : "false",
                 (unsigned int)M2M_DEVBOARD_BATTERY_PERCENT,
                 snapshot.voice_busy ? "true" : "false",
                 snapshot.voice_recording ? "true" : "false",
                 snapshot.fall_pending ? "true" : "false",
                 snapshot.debug_led_on ? "true" : "false",
                 snapshot.ota_in_progress ? "true" : "false",
                 snapshot.ota_status,
                 M2M_FIRMWARE_VERSION,
                 uptime_s);

  if (osMessageQueuePut(cloud_tx_q, &message, 0U, 0U) != osOK) {
    printf("[m2m] status queue full\r\n");
  }
}

static void queue_cloud_payload(const char *topic, const char *payload, sl_mqtt_qos_t qos, bool retained)
{
  cloud_tx_msg_t message = { 0 };

  message.topic = topic;
  message.qos = qos;
  message.retained = retained;
  copy_text(message.payload, sizeof(message.payload), payload);

  if (osMessageQueuePut(cloud_tx_q, &message, 0U, 0U) != osOK) {
    printf("[m2m] cloud queue full for topic %s\r\n", topic);
  }
}

static void queue_audio_prompt_internal(audio_prompt_type_t type, const char *text, bool use_cloud_speech)
{
  audio_prompt_msg_t prompt = { 0 };

  prompt.type = type;
  prompt.use_cloud_speech = use_cloud_speech;
  copy_text(prompt.text, sizeof(prompt.text), text);

  if (osMessageQueuePut(audio_q, &prompt, 0U, 0U) != osOK) {
    printf("[m2m] audio queue full\r\n");
  }
}

static void queue_audio_prompt(audio_prompt_type_t type, const char *text)
{
  queue_audio_prompt_internal(type, text, false);
}

static void queue_spoken_audio_prompt(audio_prompt_type_t type, const char *text)
{
  queue_audio_prompt_internal(type, text, true);
}

static bool queue_voice_event(voice_event_type_t type)
{
  voice_event_msg_t event = { .type = type };
  return osMessageQueuePut(voice_event_q, &event, 0U, 0U) == osOK;
}

static void request_status_publish(void)
{
  status_publish_pending = true;
}

static void queue_ota_status_publish(const char *phase,
                                     const char *detail,
                                     const char *url,
                                     const char *target_version)
{
  char payload[M2M_MQTT_PAYLOAD_MAX];

  (void)snprintf(payload,
                 sizeof(payload),
                 "{\"device_id\":\"%s\",\"phase\":\"%s\",\"detail\":\"%s\","
                 "\"target_version\":\"%s\",\"url\":\"%s\","
                 "\"fw_version\":\"%s\",\"uptime_ms\":%" PRIu32 "}",
                 M2M_DEVICE_ID,
                 (phase == NULL) ? "" : phase,
                 (detail == NULL) ? "" : detail,
                 (target_version == NULL) ? "" : target_version,
                 (url == NULL) ? "" : url,
                 M2M_FIRMWARE_VERSION,
                 now_ms());
  queue_cloud_payload(M2M_TOPIC_DEVICE_OTA_STATUS, payload, SL_MQTT_QOS_LEVEL_1, false);
}

static void publish_button_state(bool pressed)
{
  char payload[96];

  (void)snprintf(payload,
                 sizeof(payload),
                 "{\"pressed\":%s,\"device_id\":\"%s\",\"fw_version\":\"%s\"}",
                 pressed ? "true" : "false",
                 M2M_DEVICE_ID,
                 M2M_FIRMWARE_VERSION);
  queue_cloud_payload(M2M_TOPIC_DEVICE_DEBUG_BUTTON, payload, SL_MQTT_QOS_LEVEL_0, false);
}

static void publish_fall_event(const char *phase, const m2m_imu_sample_t *sample)
{
  char payload[M2M_MQTT_PAYLOAD_MAX];
  m2m_imu_sample_t empty_sample = { 0 };

  if (sample == NULL) {
    sample = &empty_sample;
  }

  (void)snprintf(payload,
                 sizeof(payload),
                 "{\"device_id\":\"%s\",\"phase\":\"%s\",\"confirmed\":%s,"
                 "\"ax_mg\":%d,\"ay_mg\":%d,\"az_mg\":%d,"
                 "\"fw_version\":\"%s\",\"uptime_ms\":%" PRIu32 "}",
                 M2M_DEVICE_ID,
                 phase,
                 (strcmp(phase, "alert") == 0) ? "true" : "false",
                 (int)sample->accel_mg[0],
                 (int)sample->accel_mg[1],
                 (int)sample->accel_mg[2],
                 M2M_FIRMWARE_VERSION,
                 now_ms());
  queue_cloud_payload(M2M_TOPIC_DEVICE_FALL, payload, SL_MQTT_QOS_LEVEL_1, false);
}

static void publish_fall_ack(const char *result)
{
  char payload[160];

  (void)snprintf(payload,
                 sizeof(payload),
                 "{\"device_id\":\"%s\",\"type\":\"fall\",\"result\":\"%s\","
                 "\"uptime_ms\":%" PRIu32 "}",
                 M2M_DEVICE_ID,
                 result,
                 now_ms());
  queue_cloud_payload(M2M_TOPIC_DEVICE_REMINDER_ACK, payload, SL_MQTT_QOS_LEVEL_1, false);
}

static void mqtt_client_event_handler(void *client,
                                      sl_mqtt_client_event_t event,
                                      void *event_data,
                                      void *context)
{
  (void)client;
  (void)context;

  if (event != SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT) {
    printf("[mqtt-event] event=%s(%d) current_thread=%p stack_free=%lu\r\n",
           mqtt_client_event_name(event),
           (int)event,
           (void *)osThreadGetId(),
           (unsigned long)osThreadGetStackSpace(osThreadGetId()));
  }

  switch (event) {
    case SL_MQTT_CLIENT_CONNECTED_EVENT:
      mqtt_connected = true;
      mqtt_subscribed = false;
      state_set_connectivity(true, true);
      printf("[mqtt] connected\r\n");
      request_status_publish();
      break;

    case SL_MQTT_CLIENT_DISCONNECTED_EVENT:
      mqtt_connected = false;
      mqtt_subscribed = false;
      state_set_connectivity(true, false);
      printf("[mqtt] disconnected\r\n");
      break;

    case SL_MQTT_CLIENT_SUBSCRIBED_EVENT:
      printf("[mqtt] subscribed\r\n");
      break;

    case SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT:
      break;

    case SL_MQTT_CLIENT_ERROR_EVENT:
      printf("[mqtt] error event %d\r\n", *((sl_mqtt_client_error_status_t *)event_data));
      break;

    default:
      break;
  }
}

static void mqtt_message_handler(void *client, sl_mqtt_client_message_t *message, void *context)
{
  osThreadId_t callback_thread;

  (void)client;
  (void)context;
  callback_thread = osThreadGetId();
  copy_mqtt_topic(mqtt_handler_topic, sizeof(mqtt_handler_topic), message);
  copy_mqtt_payload(mqtt_handler_payload, sizeof(mqtt_handler_payload), message);
  build_log_preview((message == NULL) ? NULL : message->content,
                    (message == NULL) ? 0U : message->content_length,
                    mqtt_handler_payload_preview,
                    sizeof(mqtt_handler_payload_preview));
  printf("[mqtt] rx topic=%s topic_len=%u payload_len=%u payload_preview=%s\r\n",
         mqtt_handler_topic,
         (unsigned int)((message == NULL) ? 0U : message->topic_length),
         (unsigned int)((message == NULL) ? 0U : message->content_length),
         mqtt_handler_payload_preview);
  printf("[mqtt-callback] thread=%p stack_free=%lu voice_ai_id=%p voice_ai_stack_free=%lu\r\n",
         (void *)callback_thread,
         (unsigned long)osThreadGetStackSpace(callback_thread),
         (void *)voice_ai_task_thread_id,
         (unsigned long)((voice_ai_task_thread_id == NULL) ? 0U : osThreadGetStackSpace(voice_ai_task_thread_id)));

  if (strcmp(mqtt_handler_topic, M2M_TOPIC_CLOUD_DEBUG_LED_SET) == 0) {
    bool led_on = payload_is_truthy(mqtt_handler_payload);
    state_set_debug_led(led_on);
    request_status_publish();
  } else if (strcmp(mqtt_handler_topic, M2M_TOPIC_CLOUD_REMINDER_SET) == 0) {
    state_store_reminder(mqtt_handler_payload);
    queue_audio_prompt(AUDIO_PROMPT_REMINDER, mqtt_handler_payload);
    request_status_publish();
  } else if (strcmp(mqtt_handler_topic, M2M_TOPIC_CLOUD_VOICE_REPLY) == 0) {
    osStatus_t queue_status;
    bool has_request_id;

    memset(&mqtt_handler_voice_reply, 0, sizeof(mqtt_handler_voice_reply));
    has_request_id = extract_json_string_value(mqtt_handler_payload,
                                               "request_id",
                                               mqtt_handler_voice_reply.request_id,
                                               sizeof(mqtt_handler_voice_reply.request_id));

    (void)extract_json_string_value(mqtt_handler_payload,
                                    "audio_url",
                                    mqtt_handler_voice_reply.audio_url,
                                    sizeof(mqtt_handler_voice_reply.audio_url));
    (void)extract_json_string_value(mqtt_handler_payload,
                                    "text",
                                    mqtt_handler_voice_reply.text,
                                    sizeof(mqtt_handler_voice_reply.text));
    (void)extract_json_string_value(mqtt_handler_payload,
                                    "format",
                                    mqtt_handler_voice_reply.format,
                                    sizeof(mqtt_handler_voice_reply.format));
    (void)extract_json_uint32_value(mqtt_handler_payload,
                                    "sample_rate_hz",
                                    &mqtt_handler_voice_reply.sample_rate_hz);

    if (has_request_id) {
      queue_status = osMessageQueuePut(voice_reply_q, &mqtt_handler_voice_reply, 0U, 0U);

      if (queue_status == osOK) {
        printf("[voice] queued reply request_id=%s audio_url=%s queue_count=%lu voice_ai_stack_free=%lu\r\n",
               mqtt_handler_voice_reply.request_id,
               (mqtt_handler_voice_reply.audio_url[0] == '\0') ? "<missing>" : mqtt_handler_voice_reply.audio_url,
               (unsigned long)((voice_reply_q != NULL) ? osMessageQueueGetCount(voice_reply_q) : 0U),
               (unsigned long)((voice_ai_task_thread_id == NULL) ? 0U : osThreadGetStackSpace(voice_ai_task_thread_id)));
      } else {
        printf("[voice] reply queue put failed request_id=%s status=%d\r\n",
               mqtt_handler_voice_reply.request_id,
               (int)queue_status);
        state_set_voice_busy(false);
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_reply_invalid");
        request_status_publish();
      }
    } else {
      printf("[voice] reply queue failed request_id=%s\r\n",
             has_request_id ? mqtt_handler_voice_reply.request_id : "<missing>");
      state_set_voice_busy(false);
      queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_reply_invalid");
      request_status_publish();
    }
  } else if (strcmp(mqtt_handler_topic, M2M_TOPIC_CLOUD_OTA_UPDATE) == 0) {
    if (queue_ota_update_from_payload(mqtt_handler_payload)) {
      queue_audio_prompt(AUDIO_PROMPT_OTA_STATUS, "ota_request_received");
    } else {
      queue_audio_prompt(AUDIO_PROMPT_NOTICE, "ota_request_invalid");
      queue_ota_status_publish("rejected", "invalid_payload", NULL, NULL);
    }
  } else if (strcmp(mqtt_handler_topic, M2M_TOPIC_CLOUD_SYSTEM_NOTICE) == 0) {
    device_state_t snapshot = { 0 };

    if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
      snapshot = device_state;
      (void)osMutexRelease(device_state_mutex);
    }

    if (payload_contains(mqtt_handler_payload, "simulate_fall")) {
      sensor_event_t sensor_event = { 0 };
      sensor_event.type = SENSOR_EVENT_FALL_CANDIDATE;
      sensor_event.tick_ms = now_ms();
      sensor_event.sample.accel_mg[2] = 3200;
      (void)osMessageQueuePut(sensor_event_q, &sensor_event, 0U, 0U);
    }
    if (payload_contains(mqtt_handler_payload, "start_vision")) {
      if (snapshot.voice_busy) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_busy");
      } else if (!queue_voice_event(VOICE_EVENT_START_VISION)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "vision_queue_full");
      }
    } else if (payload_contains(mqtt_handler_payload, "start_qa")) {
      if (snapshot.voice_busy) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_busy");
      } else if (!queue_voice_event(VOICE_EVENT_START_QA)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_queue_full");
      }
    }
    queue_audio_prompt(AUDIO_PROMPT_NOTICE, mqtt_handler_payload);
  } else {
    printf("[mqtt] unhandled topic %s\r\n", mqtt_handler_topic);
  }
}

static sl_status_t wifi_start(void)
{
  sl_status_t status;

  wifi_start_phase = WIFI_START_PHASE_SETTLE;

  print_wifi_profile_summary();
  if (M2M_WIFI_STARTUP_SETTLE_MS > 0U) {
    printf("[wifi] startup settle wait=%u ms before net init\r\n",
           (unsigned int)M2M_WIFI_STARTUP_SETTLE_MS);
    osDelay(M2M_WIFI_STARTUP_SETTLE_MS);
  }

  wifi_init_watchdog_start();
  wifi_start_phase = WIFI_START_PHASE_NET_INIT_BEGIN;
  printf("[wifi] net init begin\r\n");
  status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &wifi_client_configuration, NULL, NULL);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_ALREADY_INITIALIZED)) {
    wifi_start_phase = WIFI_START_PHASE_FAILED;
    wifi_init_watchdog_stop();
    printf("[wifi] init failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }
  wifi_start_phase = WIFI_START_PHASE_NET_INIT_DONE;
  wifi_init_watchdog_stop();
  printf("[wifi] net init done status=0x%lx\r\n", (unsigned long)status);

#if M2M_WIFI_RUN_STARTUP_SCAN_DIAGNOSTIC
  wifi_start_phase = WIFI_START_PHASE_SCAN;
  run_wifi_scan_diagnostic();
#else
  printf("[wifi] startup scan diagnostic disabled\r\n");
#endif
  print_wifi_mac_address();
  wifi_start_phase = WIFI_START_PHASE_CONNECTING;
  printf("[wifi] connecting to %s\r\n", DEFAULT_WIFI_CLIENT_PROFILE_SSID);
  status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID);
  if (status != SL_STATUS_OK) {
    wifi_start_phase = WIFI_START_PHASE_FAILED;
    printf("[wifi] interface up failed: 0x%lx (%s)\r\n",
           (unsigned long)status,
           wifi_status_name(status));
    print_wifi_failure_hint(status);
    return status;
  }

  state_set_connectivity(true, false);
  wifi_start_phase = WIFI_START_PHASE_CONNECTED;
  printf("[wifi] connected\r\n");
  return SL_STATUS_OK;
}

static sl_status_t mqtt_start(void)
{
  sl_status_t status;

#ifdef SLI_SI91X_ENABLE_IPV6
  uint8_t hex_addr[SL_IPV6_ADDRESS_LENGTH] = { 0 };
  status = sl_inet_pton6(M2M_MQTT_BROKER_IP,
                         M2M_MQTT_BROKER_IP + strlen(M2M_MQTT_BROKER_IP),
                         hex_addr,
                         (unsigned int *)mqtt_broker_configuration.ip.ip.v6.value);
  if (status != 0x1) {
    printf("[mqtt] IPv6 address conversion failed\r\n");
    return status;
  }
  mqtt_broker_configuration.ip.type = SL_IPV6;
#else
  status = sl_net_inet_addr(M2M_MQTT_BROKER_IP, &mqtt_broker_configuration.ip.ip.v4.value);
  if (status != SL_STATUS_OK) {
    printf("[mqtt] IPv4 address conversion failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }
  mqtt_broker_configuration.ip.type = SL_IPV4;
#endif

  status = sl_mqtt_client_init(&mqtt_client, mqtt_client_event_handler);
  if (status != SL_STATUS_OK) {
    printf("[mqtt] client init failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  status = sl_mqtt_client_connect(&mqtt_client,
                                  &mqtt_broker_configuration,
                                  &last_will_message,
                                  &mqtt_client_configuration,
                                  0U);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
    printf("[mqtt] connect failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  printf("[mqtt] connect requested\r\n");
  return SL_STATUS_OK;
}

static sl_status_t mqtt_stop(void)
{
  sl_status_t status = sl_mqtt_client_disconnect(&mqtt_client, M2M_MQTT_DISCONNECT_TIMEOUT_MS);

  if ((status == SL_STATUS_OK) || (status == SL_STATUS_IN_PROGRESS) || (status == SL_STATUS_INVALID_STATE)) {
    mqtt_connected = false;
    mqtt_subscribed = false;
    state_set_connectivity(true, false);
    return SL_STATUS_OK;
  }

  printf("[mqtt] disconnect failed: 0x%lx\r\n", (unsigned long)status);
  return status;
}

static sl_status_t mqtt_subscribe_all(void)
{
  static const char *topics[] = {
    M2M_TOPIC_CLOUD_DEBUG_LED_SET,
    M2M_TOPIC_CLOUD_REMINDER_SET,
    M2M_TOPIC_CLOUD_VOICE_REPLY,
    M2M_TOPIC_CLOUD_OTA_UPDATE,
    M2M_TOPIC_CLOUD_SYSTEM_NOTICE,
  };
  sl_status_t status = SL_STATUS_OK;

  for (size_t i = 0; i < (sizeof(topics) / sizeof(topics[0])); i++) {
    status = sl_mqtt_client_subscribe(&mqtt_client,
                                      (const uint8_t *)topics[i],
                                      (uint16_t)strlen(topics[i]),
                                      SL_MQTT_QOS_LEVEL_1,
                                      1000U,
                                      mqtt_message_handler,
                                      (void *)topics[i]);
    if (status != SL_STATUS_OK) {
      printf("[mqtt] subscribe failed topic=%s status=0x%lx\r\n",
             topics[i],
             (unsigned long)status);
      return status;
    }
  }

  mqtt_subscribed = true;
  printf("[mqtt] all cloud topics subscribed\r\n");
  queue_audio_prompt(AUDIO_PROMPT_NOTICE, "system_ready");
  return SL_STATUS_OK;
}

static sl_status_t mqtt_publish_message(const cloud_tx_msg_t *queued_message)
{
  sl_mqtt_client_message_t mqtt_message = { 0 };
  sl_status_t status = SL_STATUS_FAIL;

  mqtt_message.qos_level = queued_message->qos;
  mqtt_message.is_retained = queued_message->retained;
  mqtt_message.is_duplicate_message = MQTT_DUPLICATE_MESSAGE;
  mqtt_message.topic = (uint8_t *)queued_message->topic;
  mqtt_message.topic_length = (uint16_t)strlen(queued_message->topic);
  mqtt_message.content = (uint8_t *)queued_message->payload;
  mqtt_message.content_length = strlen(queued_message->payload);

  for (uint32_t attempt = 1U; attempt <= M2M_MQTT_PUBLISH_RETRY_COUNT; ++attempt) {
    status = sl_mqtt_client_publish(&mqtt_client, &mqtt_message, M2M_MQTT_PUBLISH_TIMEOUT_MS, NULL);
    if ((status == SL_STATUS_OK) || (status == SL_STATUS_IN_PROGRESS)) {
      return status;
    }

    if (!is_transient_mqtt_publish_status(status) || (attempt == M2M_MQTT_PUBLISH_RETRY_COUNT)) {
      break;
    }

    printf("[mqtt] publish retry topic=%s attempt=%lu/%u status=0x%lx (%s)\r\n",
           queued_message->topic,
           (unsigned long)attempt,
           (unsigned int)M2M_MQTT_PUBLISH_RETRY_COUNT,
           (unsigned long)status,
           sl_status_name(status));
    osDelay(M2M_MQTT_PUBLISH_RETRY_DELAY_MS);
  }

  return status;
}

static bool payload_is_truthy(const char *payload)
{
  return payload_contains(payload, "true") || payload_contains(payload, "\"on\"")
         || payload_contains(payload, ":1") || (strcmp(payload, "1") == 0);
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
  size_t copy_len;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  if (src == NULL) {
    dest[0] = '\0';
    return;
  }

  copy_len = strlen(src);
  if (copy_len >= dest_size) {
    copy_len = dest_size - 1U;
  }

  memcpy(dest, src, copy_len);
  dest[copy_len] = '\0';
}

static bool append_text(char *dest, size_t dest_size, size_t *offset, const char *src)
{
  size_t write_offset;
  size_t src_len;
  size_t copy_len;
  bool complete = true;

  if ((dest == NULL) || (dest_size == 0U) || (offset == NULL)) {
    return false;
  }

  write_offset = *offset;
  if (write_offset >= dest_size) {
    dest[dest_size - 1U] = '\0';
    return false;
  }

  if (src == NULL) {
    src = "";
  }

  src_len = strlen(src);
  copy_len = src_len;
  if ((write_offset + copy_len) >= dest_size) {
    copy_len = (dest_size - write_offset) - 1U;
    complete = false;
  }

  if (copy_len > 0U) {
    memcpy(&dest[write_offset], src, copy_len);
    write_offset += copy_len;
  }

  dest[write_offset] = '\0';
  *offset = write_offset;
  return complete;
}

static bool append_uint32_text(char *dest, size_t dest_size, size_t *offset, uint32_t value)
{
  char value_text[16];
  int written = snprintf(value_text, sizeof(value_text), "%" PRIu32, value);

  if ((written <= 0) || ((size_t)written >= sizeof(value_text))) {
    return false;
  }

  return append_text(dest, dest_size, offset, value_text);
}

static void copy_mqtt_payload(char *dest, size_t dest_size, const sl_mqtt_client_message_t *message)
{
  size_t copy_len;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  if ((message == NULL) || (message->content == NULL)) {
    dest[0] = '\0';
    return;
  }

  copy_len = message->content_length;
  if (copy_len >= dest_size) {
    copy_len = dest_size - 1U;
  }

  memcpy(dest, message->content, copy_len);
  dest[copy_len] = '\0';
}

static void copy_mqtt_topic(char *dest, size_t dest_size, const sl_mqtt_client_message_t *message)
{
  size_t copy_len;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  if ((message == NULL) || (message->topic == NULL)) {
    dest[0] = '\0';
    return;
  }

  copy_len = message->topic_length;
  if (copy_len >= dest_size) {
    copy_len = dest_size - 1U;
  }

  memcpy(dest, message->topic, copy_len);
  dest[copy_len] = '\0';
}

static void build_log_preview(const uint8_t *data, size_t data_length, char *dest, size_t dest_size)
{
  size_t read_index = 0U;
  size_t write_index = 0U;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  dest[0] = '\0';

  if ((data == NULL) || (data_length == 0U)) {
    copy_text(dest, dest_size, "<empty>");
    return;
  }

  while ((read_index < data_length) && (write_index + 1U < dest_size)) {
    uint8_t ch = data[read_index++];

    if ((ch >= 32U) && (ch <= 126U)) {
      dest[write_index++] = (char)ch;
    } else if ((ch == '\r') || (ch == '\n') || (ch == '\t')) {
      dest[write_index++] = ' ';
    } else {
      dest[write_index++] = '.';
    }
  }

  if ((read_index < data_length) && (write_index + 4U < dest_size)) {
    dest[write_index++] = '.';
    dest[write_index++] = '.';
    dest[write_index++] = '.';
  }

  dest[write_index] = '\0';
}

static bool payload_contains(const char *payload, const char *needle)
{
  return (payload != NULL) && (needle != NULL) && (strstr(payload, needle) != NULL);
}

static void sanitize_upload_name(const char *src, const char *fallback, char *dest, size_t dest_size)
{
  size_t write_index = 0U;

  if ((dest == NULL) || (dest_size == 0U)) {
    return;
  }

  dest[0] = '\0';

  if (src != NULL) {
    for (size_t read_index = 0U; (src[read_index] != '\0') && (write_index + 1U < dest_size); ++read_index) {
      char ch = src[read_index];
      bool keep = ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) || ((ch >= '0') && (ch <= '9'))
                  || (ch == '.') || (ch == '_') || (ch == '-');

      dest[write_index++] = keep ? ch : '_';
    }
  }

  while ((write_index > 0U) && ((dest[write_index - 1U] == '.') || (dest[write_index - 1U] == '_'))) {
    write_index--;
  }

  size_t start_index = 0U;
  while ((start_index < write_index) && ((dest[start_index] == '.') || (dest[start_index] == '_'))) {
    start_index++;
  }

  if ((start_index > 0U) && (start_index < write_index)) {
    memmove(dest, dest + start_index, write_index - start_index);
    write_index -= start_index;
  } else if (start_index >= write_index) {
    write_index = 0U;
  }

  dest[write_index] = '\0';
  if ((write_index == 0U) && (fallback != NULL)) {
    copy_text(dest, dest_size, fallback);
  }
}

static bool build_upload_fallback_url(const char *request_id,
                                      const char *kind,
                                      const char *filename,
                                      char *uploaded_url,
                                      size_t uploaded_url_size)
{
  char safe_request[40];
  char safe_kind[24];
  const char *suffix = ".bin";
  const char *dot;
  int written;

  if ((request_id == NULL) || (kind == NULL) || (filename == NULL) || (uploaded_url == NULL) || (uploaded_url_size == 0U)) {
    return false;
  }

  sanitize_upload_name(request_id, "upload", safe_request, sizeof(safe_request));
  sanitize_upload_name(kind, "devkit", safe_kind, sizeof(safe_kind));

  dot = strrchr(filename, '.');
  if ((dot != NULL) && (dot[1] != '\0')) {
    suffix = dot;
  }

  written = snprintf(uploaded_url,
                     uploaded_url_size,
                     "%s/%s_%s%s",
                     M2M_DEVBOARD_UPLOAD_BASE_URL,
                     safe_request,
                     safe_kind,
                     suffix);
  return (written > 0) && ((size_t)written < uploaded_url_size);
}

static bool extract_json_string_value(const char *payload,
                                      const char *key,
                                      char *dest,
                                      size_t dest_size)
{
  char pattern[40];
  const char *key_pos;
  const char *value_pos;
  size_t value_len = 0;

  if ((payload == NULL) || (key == NULL) || (dest == NULL) || (dest_size == 0U)) {
    return false;
  }

  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  key_pos = strstr(payload, pattern);
  if (key_pos == NULL) {
    return false;
  }

  value_pos = strchr(key_pos + strlen(pattern), ':');
  if (value_pos == NULL) {
    return false;
  }

  value_pos++;
  while ((*value_pos == ' ') || (*value_pos == '\t')) {
    value_pos++;
  }

  if (*value_pos != '"') {
    return false;
  }

  value_pos++;
  while ((value_pos[value_len] != '\0') && (value_pos[value_len] != '"')) {
    value_len++;
  }

  if (value_pos[value_len] != '"') {
    return false;
  }

  if (value_len >= dest_size) {
    value_len = dest_size - 1U;
  }

  memcpy(dest, value_pos, value_len);
  dest[value_len] = '\0';
  return true;
}

static bool extract_json_uint32_value(const char *payload, const char *key, uint32_t *value)
{
  char pattern[40];
  const char *key_pos;
  const char *value_pos;
  uint32_t parsed_value = 0U;
  bool found_digit = false;

  if ((payload == NULL) || (key == NULL) || (value == NULL)) {
    return false;
  }

  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  key_pos = strstr(payload, pattern);
  if (key_pos == NULL) {
    return false;
  }

  value_pos = strchr(key_pos + strlen(pattern), ':');
  if (value_pos == NULL) {
    return false;
  }

  value_pos++;
  while ((*value_pos == ' ') || (*value_pos == '\t')) {
    value_pos++;
  }

  while ((*value_pos >= '0') && (*value_pos <= '9')) {
    found_digit = true;
    parsed_value = (parsed_value * 10U) + (uint32_t)(*value_pos - '0');
    value_pos++;
  }

  if (!found_digit) {
    return false;
  }

  *value = parsed_value;
  return true;
}

static size_t escape_json_string(char *dest, size_t dest_size, const char *src, size_t src_limit)
{
  size_t written = 0U;
  size_t consumed = 0U;

  if ((dest == NULL) || (dest_size == 0U)) {
    return 0U;
  }

  dest[0] = '\0';
  if (src == NULL) {
    return 0U;
  }

  while ((src[consumed] != '\0') && (consumed < src_limit)) {
    const char *replacement = NULL;
    char ch = src[consumed];
    size_t replacement_length = 0U;

    switch (ch) {
      case '\\':
        replacement = "\\\\";
        replacement_length = 2U;
        break;
      case '"':
        replacement = "\\\"";
        replacement_length = 2U;
        break;
      case '\n':
        replacement = "\\n";
        replacement_length = 2U;
        break;
      case '\r':
        replacement = "\\r";
        replacement_length = 2U;
        break;
      case '\t':
        replacement = "\\t";
        replacement_length = 2U;
        break;
      default:
        break;
    }

    if (replacement != NULL) {
      if ((written + replacement_length) >= dest_size) {
        break;
      }

      memcpy(&dest[written], replacement, replacement_length);
      written += replacement_length;
    } else {
      if ((unsigned char)ch < 0x20U) {
        ch = ' ';
      }

      if ((written + 1U) >= dest_size) {
        break;
      }

      dest[written++] = ch;
    }

    consumed++;
  }

  dest[written] = '\0';
  return written;
}

static bool parse_http_url(const char *url,
                           bool *use_tls,
                           char *host,
                           size_t host_size,
                           uint16_t *port,
                           char *resource,
                           size_t resource_size)
{
  const char *cursor;
  const char *path_start;
  const char *host_end;
  const char *port_start = NULL;
  size_t host_len;

  if ((url == NULL) || (use_tls == NULL) || (host == NULL) || (port == NULL) || (resource == NULL)
      || (host_size == 0U) || (resource_size == 0U)) {
    return false;
  }

  if (strncmp(url, "http://", 7) == 0) {
    *use_tls = false;
    *port = 80U;
    cursor = url + 7;
  } else if (strncmp(url, "https://", 8) == 0) {
    *use_tls = true;
    *port = 443U;
    cursor = url + 8;
  } else {
    return false;
  }

  path_start = strchr(cursor, '/');
  if ((path_start == NULL) || (path_start[1] == '\0')) {
    return false;
  }

  host_end = path_start;
  for (const char *scan = cursor; scan < path_start; scan++) {
    if (*scan == ':') {
      host_end = scan;
      port_start = scan + 1;
      break;
    }
  }

  host_len = (size_t)(host_end - cursor);
  if ((host_len == 0U) || (host_len >= host_size)) {
    return false;
  }

  memcpy(host, cursor, host_len);
  host[host_len] = '\0';

  if (port_start != NULL) {
    uint32_t parsed_port = 0U;

    for (const char *scan = port_start; scan < path_start; scan++) {
      if ((*scan < '0') || (*scan > '9')) {
        return false;
      }
      parsed_port = (parsed_port * 10U) + (uint32_t)(*scan - '0');
    }

    if ((parsed_port == 0U) || (parsed_port > 65535U)) {
      return false;
    }
    *port = (uint16_t)parsed_port;
  }

  copy_text(resource, resource_size, path_start + 1);
  return resource[0] != '\0';
}

static bool parse_http_request_url(const char *url,
                                   bool *use_tls,
                                   char *host,
                                   size_t host_size,
                                   uint16_t *port,
                                   char *resource,
                                   size_t resource_size)
{
  const char *cursor;
  const char *path_start;
  const char *host_end;
  const char *port_start = NULL;
  size_t host_len;

  if ((url == NULL) || (use_tls == NULL) || (host == NULL) || (port == NULL) || (resource == NULL)
      || (host_size == 0U) || (resource_size == 0U)) {
    return false;
  }

  if (strncmp(url, "http://", 7) == 0) {
    *use_tls = false;
    *port = 80U;
    cursor = url + 7;
  } else if (strncmp(url, "https://", 8) == 0) {
    *use_tls = true;
    *port = 443U;
    cursor = url + 8;
  } else {
    return false;
  }

  path_start = strchr(cursor, '/');
  if (path_start == NULL) {
    return false;
  }

  host_end = path_start;
  for (const char *scan = cursor; scan < path_start; scan++) {
    if (*scan == ':') {
      host_end = scan;
      port_start = scan + 1;
      break;
    }
  }

  host_len = (size_t)(host_end - cursor);
  if ((host_len == 0U) || (host_len >= host_size)) {
    return false;
  }

  memcpy(host, cursor, host_len);
  host[host_len] = '\0';

  if (port_start != NULL) {
    uint32_t parsed_port = 0U;

    for (const char *scan = port_start; scan < path_start; scan++) {
      if ((*scan < '0') || (*scan > '9')) {
        return false;
      }
      parsed_port = (parsed_port * 10U) + (uint32_t)(*scan - '0');
    }

    if ((parsed_port == 0U) || (parsed_port > 65535U)) {
      return false;
    }
    *port = (uint16_t)parsed_port;
  }

  copy_text(resource, resource_size, path_start);
  return resource[0] != '\0';
}

static sl_status_t ensure_http_client_credentials(void)
{
  static bool credentials_ready = false;
  sl_http_client_credentials_t *client_credentials;
  sl_status_t status;

  if (credentials_ready) {
    return SL_STATUS_OK;
  }

  client_credentials = (sl_http_client_credentials_t *)malloc(sizeof(sl_http_client_credentials_t));
  if (client_credentials == NULL) {
    return SL_STATUS_ALLOCATION_FAILED;
  }

  memset(client_credentials, 0, sizeof(sl_http_client_credentials_t));
  client_credentials->username_length = 0U;
  client_credentials->password_length = 0U;

  status = sl_net_set_credential(SL_NET_HTTP_CLIENT_CREDENTIAL_ID(0),
                                 SL_NET_HTTP_CLIENT_CREDENTIAL,
                                 client_credentials,
                                 sizeof(sl_http_client_credentials_t));
  free(client_credentials);

  if (status == SL_STATUS_OK) {
    credentials_ready = true;
  }

  return status;
}

static sl_status_t http_client_response_handler(const sl_http_client_t *client,
                                                sl_http_client_event_t event,
                                                void *data,
                                                void *request_context)
{
  http_request_context_t *context = (http_request_context_t *)request_context;
  sl_http_client_response_t *response = (sl_http_client_response_t *)data;

  (void)client;
  (void)event;

  if ((context == NULL) || (response == NULL)) {
    return SL_STATUS_NULL_POINTER;
  }

  context->callback_status = response->status;
  context->http_response_code = response->http_response_code;

  if ((response->status != SL_STATUS_OK)
      || ((response->http_response_code >= 400U) && (response->http_response_code <= 599U)
          && (response->http_response_code != 0U))) {
    context->callback_status = SL_STATUS_FAIL;
    context->completed = true;
  }

  if (!context->completed && (response->data_length > 0U)) {
    if ((context->buffer == NULL)
        || ((context->response_length + (size_t)response->data_length) > context->buffer_capacity)) {
      context->callback_status = SL_STATUS_HAS_OVERFLOWED;
      context->completed = true;
    } else {
      memcpy(context->buffer + context->response_length, response->data_buffer, response->data_length);
      context->response_length += response->data_length;
    }
  }

  if (response->end_of_data != 0U) {
    context->completed = true;
  }

  if (context->owner_thread != NULL) {
    (void)osThreadFlagsSet(context->owner_thread, HTTP_FLAG_EVENT);
  }

  return response->status;
}

static sl_status_t http_execute_request(sl_http_client_method_type_t method,
                                        const char *url,
                                        const http_request_header_t *headers,
                                        size_t header_count,
                                        const uint8_t *body,
                                        size_t body_length,
                                        uint8_t *response,
                                        size_t response_capacity,
                                        size_t *response_length,
                                        uint32_t timeout_ms)
{
  sl_http_client_configuration_t client_configuration = { 0 };
  sl_http_client_request_t request = { 0 };
  sl_http_client_t client = 0U;
  http_request_context_t context = { 0 };
  bool use_tls = false;
  char host[M2M_HTTP_HOST_MAX];
  char resource[M2M_HTTP_RESOURCE_MAX];
  uint16_t port = 80U;
  sl_status_t status;

  if ((url == NULL) || ((body_length > 0U) && (body == NULL))) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  if (!parse_http_request_url(url, &use_tls, host, sizeof(host), &port, resource, sizeof(resource))) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  status = ensure_http_client_credentials();
  if (status != SL_STATUS_OK) {
    printf("[http] credential setup failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  client_configuration.network_interface = SL_NET_WIFI_CLIENT_INTERFACE;
  client_configuration.ip_version = SL_IPV4;
  client_configuration.http_version = SL_HTTP_V_1_1;
  client_configuration.https_enable = use_tls;
  client_configuration.https_use_sni = use_tls;
  client_configuration.tls_version = SL_TLS_V_1_2;

  status = sl_http_client_init(&client_configuration, &client);
  if (status != SL_STATUS_OK) {
    printf("[http] client init failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  request.http_method_type = method;
  request.ip_address = (uint8_t *)host;
  request.host_name = (uint8_t *)host;
  request.port = port;
  request.resource = (uint8_t *)resource;
  request.body = (uint8_t *)body;
  request.body_length = (uint32_t)body_length;

  context.owner_thread = osThreadGetId();
  context.buffer = response;
  context.buffer_capacity = response_capacity;
  context.response_length = 0U;
  context.callback_status = SL_STATUS_FAIL;
  context.http_response_code = 0U;
  context.completed = false;

  status = sl_http_client_request_init(&request, http_client_response_handler, &context);
  if (status != SL_STATUS_OK) {
    printf("[http] request init failed: 0x%lx\r\n", (unsigned long)status);
    (void)sl_http_client_deinit(&client);
    return status;
  }

  for (size_t i = 0; i < header_count; i++) {
    status = sl_http_client_add_header(&request, headers[i].key, headers[i].value);
    if (status != SL_STATUS_OK) {
      printf("[http] add header failed: %s status=0x%lx\r\n",
             headers[i].key,
             (unsigned long)status);
      (void)sl_http_client_deinit(&client);
      return status;
    }
  }

  (void)osThreadFlagsClear(HTTP_FLAG_EVENT);
  status = sl_http_client_send_request(&client, &request);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
    printf("[http] send request failed url=%s status=0x%lx\r\n", url, (unsigned long)status);
    (void)sl_http_client_deinit(&client);
    return status;
  }

  while (!context.completed) {
    status = http_wait_for_event(&context, url, timeout_ms);
    if (status != SL_STATUS_OK) {
      (void)sl_http_client_deinit(&client);
      return status;
    }
  }

  if ((response != NULL) && (context.response_length < response_capacity)) {
    response[context.response_length] = '\0';
  }

  if (response_length != NULL) {
    *response_length = context.response_length;
  }

  status = context.callback_status;
  (void)sl_http_client_deinit(&client);
  return status;
}

static sl_status_t http_wait_for_event(http_request_context_t *context, const char *url, uint32_t timeout_ms)
{
  uint32_t flags;

  if (context == NULL) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  flags = osThreadFlagsWait(HTTP_FLAG_EVENT, osFlagsWaitAny, timeout_ms);
  if (flags == (uint32_t)osErrorTimeout) {
    printf("[http] timeout waiting for %s response_len=%lu http_code=%u completed=%s\r\n",
           (url == NULL) ? "<unknown>" : url,
           (unsigned long)context->response_length,
           (unsigned int)context->http_response_code,
           context->completed ? "yes" : "no");
    return SL_STATUS_TIMEOUT;
  }

  if ((flags & osFlagsError) != 0U) {
    return SL_STATUS_FAIL;
  }

  if (context->completed) {
    return context->callback_status;
  }

  return SL_STATUS_OK;
}

static sl_status_t http_execute_chunked_request(sl_http_client_method_type_t method,
                                                const char *url,
                                                const http_request_header_t *headers,
                                                size_t header_count,
                                                const http_body_segment_t *segments,
                                                size_t segment_count,
                                                size_t body_length,
                                                uint8_t *response,
                                                size_t response_capacity,
                                                size_t *response_length,
                                                uint32_t timeout_ms)
{
  sl_http_client_configuration_t client_configuration = { 0 };
  sl_http_client_request_t request = { 0 };
  sl_http_client_t client = 0U;
  http_request_context_t context = { 0 };
  bool use_tls = false;
  char host[M2M_HTTP_HOST_MAX];
  char resource[M2M_HTTP_RESOURCE_MAX];
  uint16_t port = 80U;
  sl_status_t status;

  if ((url == NULL) || (segments == NULL) || (segment_count == 0U)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  if (!parse_http_request_url(url, &use_tls, host, sizeof(host), &port, resource, sizeof(resource))) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  status = ensure_http_client_credentials();
  if (status != SL_STATUS_OK) {
    printf("[http] credential setup failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  client_configuration.network_interface = SL_NET_WIFI_CLIENT_INTERFACE;
  client_configuration.ip_version = SL_IPV4;
  client_configuration.http_version = SL_HTTP_V_1_1;
  client_configuration.https_enable = use_tls;
  client_configuration.https_use_sni = use_tls;
  client_configuration.tls_version = SL_TLS_V_1_2;

  status = sl_http_client_init(&client_configuration, &client);
  if (status != SL_STATUS_OK) {
    printf("[http] client init failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  request.http_method_type = method;
  request.ip_address = (uint8_t *)host;
  request.host_name = (uint8_t *)host;
  request.port = port;
  request.resource = (uint8_t *)resource;
  request.body = NULL;
  request.body_length = (uint32_t)body_length;

  context.owner_thread = osThreadGetId();
  context.buffer = response;
  context.buffer_capacity = response_capacity;
  context.response_length = 0U;
  context.callback_status = SL_STATUS_FAIL;
  context.http_response_code = 0U;
  context.completed = false;

  status = sl_http_client_request_init(&request, http_client_response_handler, &context);
  if (status != SL_STATUS_OK) {
    printf("[http] request init failed: 0x%lx\r\n", (unsigned long)status);
    (void)sl_http_client_deinit(&client);
    return status;
  }

  for (size_t i = 0; i < header_count; i++) {
    status = sl_http_client_add_header(&request, headers[i].key, headers[i].value);
    if (status != SL_STATUS_OK) {
      printf("[http] add header failed: %s status=0x%lx\r\n",
             headers[i].key,
             (unsigned long)status);
      (void)sl_http_client_deinit(&client);
      return status;
    }
  }

  (void)osThreadFlagsClear(HTTP_FLAG_EVENT);
  status = sl_http_client_send_request(&client, &request);
  if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
    printf("[http] send chunked request failed url=%s status=0x%lx\r\n", url, (unsigned long)status);
    (void)sl_http_client_deinit(&client);
    return status;
  }

  if (status == SL_STATUS_IN_PROGRESS) {
    status = http_wait_for_event(&context, url, timeout_ms);
    if ((status != SL_STATUS_OK) && context.completed) {
      (void)sl_http_client_deinit(&client);
      return status;
    }
  }

  for (size_t segment_index = 0; segment_index < segment_count; segment_index++) {
    size_t offset = 0U;
    const http_body_segment_t *segment = &segments[segment_index];

    while (offset < segment->length) {
      size_t chunk_length = segment->length - offset;

      if (chunk_length > SL_HTTP_CLIENT_MAX_WRITE_BUFFER_LENGTH) {
        chunk_length = SL_HTTP_CLIENT_MAX_WRITE_BUFFER_LENGTH;
      }

      (void)osThreadFlagsClear(HTTP_FLAG_EVENT);
      status = sl_http_client_write_chunked_data(&client, segment->data + offset, (uint32_t)chunk_length, false);
      if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
        printf("[http] chunk write failed url=%s status=0x%lx\r\n", url, (unsigned long)status);
        (void)sl_http_client_deinit(&client);
        return status;
      }

      if (status == SL_STATUS_IN_PROGRESS) {
        status = http_wait_for_event(&context, url, timeout_ms);
        if ((status != SL_STATUS_OK) && context.completed) {
          (void)sl_http_client_deinit(&client);
          return status;
        }
      }

      offset += chunk_length;
    }
  }

  while (!context.completed) {
    status = http_wait_for_event(&context, url, timeout_ms);
    if (status != SL_STATUS_OK) {
      (void)sl_http_client_deinit(&client);
      return status;
    }
  }

  if ((response != NULL) && (context.response_length < response_capacity)) {
    response[context.response_length] = '\0';
  }

  if (response_length != NULL) {
    *response_length = context.response_length;
  }

  printf("[http] chunked request complete url=%s status=0x%lx http_code=%u response_len=%lu\r\n",
         url,
         (unsigned long)context.callback_status,
         (unsigned int)context.http_response_code,
         (unsigned long)context.response_length);

  status = context.callback_status;
  (void)sl_http_client_deinit(&client);
  return status;
}

static sl_status_t http_upload_binary_chunked(const char *request_id,
                                              const char *kind,
                                              const char *filename,
                                              const char *content_type,
                                              const uint8_t *data,
                                              size_t data_length,
                                              char *uploaded_url,
                                              size_t uploaded_url_size)
{
  static const char boundary[] = "----m2m-upload-boundary";
  http_request_header_t headers[2];
  http_body_segment_t segments[3];
  uint8_t response[M2M_HTTP_RESPONSE_JSON_MAX];
  char multipart_prefix[384];
  char multipart_suffix[64];
  char content_header[96];
  size_t response_length = 0U;
  size_t body_size;
  int prefix_length;
  int suffix_length;
  sl_status_t status;

  if ((request_id == NULL) || (kind == NULL) || (filename == NULL) || (content_type == NULL) || (data == NULL)
      || (uploaded_url == NULL) || (uploaded_url_size == 0U)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  prefix_length = snprintf(multipart_prefix,
                           sizeof(multipart_prefix),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"kind\"\r\n\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"request_id\"\r\n\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                           "Content-Type: %s\r\n\r\n",
                           boundary,
                           kind,
                           boundary,
                           request_id,
                           boundary,
                           filename,
                           content_type);
  suffix_length = snprintf(multipart_suffix, sizeof(multipart_suffix), "\r\n--%s--\r\n", boundary);

  if ((prefix_length <= 0) || ((size_t)prefix_length >= sizeof(multipart_prefix)) || (suffix_length <= 0)
      || ((size_t)suffix_length >= sizeof(multipart_suffix))) {
    return SL_STATUS_ALLOCATION_FAILED;
  }

  body_size = (size_t)prefix_length + data_length + (size_t)suffix_length;
  (void)snprintf(content_header, sizeof(content_header), "multipart/form-data; boundary=%s", boundary);
  headers[0].key = "Content-Type";
  headers[0].value = content_header;
  headers[1].key = "Accept";
  headers[1].value = "application/json";

  segments[0].data = (const uint8_t *)multipart_prefix;
  segments[0].length = (size_t)prefix_length;
  segments[1].data = data;
  segments[1].length = data_length;
  segments[2].data = (const uint8_t *)multipart_suffix;
  segments[2].length = (size_t)suffix_length;

  status = http_execute_chunked_request(SL_HTTP_POST,
                                        M2M_HTTP_UPLOAD_API_URL,
                                        headers,
                                        2U,
                                        segments,
                                        3U,
                                        body_size,
                                        response,
                                        sizeof(response),
                                        &response_length,
                                        M2M_VOICE_REPLY_WAIT_MS);

  if (status != SL_STATUS_OK) {
    if ((status == SL_STATUS_TIMEOUT)
        && build_upload_fallback_url(request_id, kind, filename, uploaded_url, uploaded_url_size)) {
      printf("[http] upload timeout, using fallback=%s\r\n", uploaded_url);
      return SL_STATUS_OK;
    }
    return status;
  }

  if (!extract_json_string_value((const char *)response, "url", uploaded_url, uploaded_url_size)) {
    char response_preview[128];

    build_log_preview(response, response_length, response_preview, sizeof(response_preview));
    printf("[http] upload response missing url len=%lu body=%s\r\n",
           (unsigned long)response_length,
           response_preview);
    if (build_upload_fallback_url(request_id, kind, filename, uploaded_url, uploaded_url_size)) {
      printf("[http] upload url fallback=%s\r\n", uploaded_url);
      return SL_STATUS_OK;
    }
    return SL_STATUS_FAIL;
  }

  return SL_STATUS_OK;
}

static sl_status_t http_upload_audio_recording(const char *request_id,
                                               const int16_t *pcm_data,
                                               size_t sample_count,
                                               char *uploaded_url,
                                               size_t uploaded_url_size)
{
  static const char boundary[] = "----m2m-upload-boundary";
  http_request_header_t headers[2];
  http_body_segment_t segments[4];
  uint8_t response[M2M_HTTP_RESPONSE_JSON_MAX];
  uint8_t wav_header[44];
  char multipart_prefix[384];
  char multipart_suffix[64];
  char content_header[96];
  size_t response_length = 0U;
  size_t pcm_bytes = sample_count * sizeof(int16_t);
  size_t body_size;
  int prefix_length;
  int suffix_length;
  sl_status_t status;

  if ((request_id == NULL) || (pcm_data == NULL) || (uploaded_url == NULL) || (uploaded_url_size == 0U)) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  prefix_length = snprintf(multipart_prefix,
                           sizeof(multipart_prefix),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"kind\"\r\n\r\n"
                           "audio\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"request_id\"\r\n\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"capture.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           boundary,
                           boundary,
                           request_id,
                           boundary);
  suffix_length = snprintf(multipart_suffix, sizeof(multipart_suffix), "\r\n--%s--\r\n", boundary);

  if ((prefix_length <= 0) || ((size_t)prefix_length >= sizeof(multipart_prefix)) || (suffix_length <= 0)
      || ((size_t)suffix_length >= sizeof(multipart_suffix))) {
    return SL_STATUS_ALLOCATION_FAILED;
  }

  build_wav_header(wav_header, pcm_bytes, M2M_AUDIO_CAPTURE_SAMPLE_RATE_HZ);
  body_size = (size_t)prefix_length + sizeof(wav_header) + pcm_bytes + (size_t)suffix_length;

  (void)snprintf(content_header, sizeof(content_header), "multipart/form-data; boundary=%s", boundary);
  headers[0].key = "Content-Type";
  headers[0].value = content_header;
  headers[1].key = "Accept";
  headers[1].value = "application/json";

  segments[0].data = (const uint8_t *)multipart_prefix;
  segments[0].length = (size_t)prefix_length;
  segments[1].data = wav_header;
  segments[1].length = sizeof(wav_header);
  segments[2].data = (const uint8_t *)pcm_data;
  segments[2].length = pcm_bytes;
  segments[3].data = (const uint8_t *)multipart_suffix;
  segments[3].length = (size_t)suffix_length;

  status = http_execute_chunked_request(SL_HTTP_POST,
                                        M2M_HTTP_UPLOAD_API_URL,
                                        headers,
                                        2U,
                                        segments,
                                        4U,
                                        body_size,
                                        response,
                                        sizeof(response),
                                        &response_length,
                                        M2M_VOICE_REPLY_WAIT_MS);
  if (status != SL_STATUS_OK) {
    if ((status == SL_STATUS_TIMEOUT)
        && build_upload_fallback_url(request_id, "audio", "capture.wav", uploaded_url, uploaded_url_size)) {
      printf("[http] upload timeout, using fallback=%s\r\n", uploaded_url);
      return SL_STATUS_OK;
    }
    return status;
  }

  if (!extract_json_string_value((const char *)response, "url", uploaded_url, uploaded_url_size)) {
    char response_preview[128];

    build_log_preview(response, response_length, response_preview, sizeof(response_preview));
    printf("[http] upload response missing url len=%lu body=%s\r\n",
           (unsigned long)response_length,
           response_preview);
    if (build_upload_fallback_url(request_id, "audio", "capture.wav", uploaded_url, uploaded_url_size)) {
      printf("[http] upload url fallback=%s\r\n", uploaded_url);
      return SL_STATUS_OK;
    }
    return SL_STATUS_FAIL;
  }

  return SL_STATUS_OK;
}

static sl_status_t http_download_binary(const char *url,
                                        uint8_t *buffer,
                                        size_t buffer_capacity,
                                        size_t *response_length,
                                        uint32_t timeout_ms)
{
  return http_execute_request(SL_HTTP_GET,
                              url,
                              NULL,
                              0U,
                              NULL,
                              0U,
                              buffer,
                              buffer_capacity,
                              response_length,
                              timeout_ms);
}

static bool queue_ota_update_from_payload(const char *payload)
{
  ota_event_msg_t ota_event = { 0 };
  char resource[M2M_OTA_RESOURCE_MAX];
  const char *resource_start = NULL;

  if ((payload == NULL) || (payload[0] == '\0')) {
    return false;
  }

  if (extract_json_string_value(payload, "url", ota_event.url, sizeof(ota_event.url))) {
  } else if ((strncmp(payload, "http://", 7) == 0) || (strncmp(payload, "https://", 8) == 0)) {
    copy_text(ota_event.url, sizeof(ota_event.url), payload);
  } else if (extract_json_string_value(payload, "resource", resource, sizeof(resource))) {
    resource_start = resource;
    while (*resource_start == '/') {
      resource_start++;
    }
    (void)snprintf(ota_event.url,
                   sizeof(ota_event.url),
                   "http://%s/%s",
                   M2M_HTTP_FILE_HOST_IP,
                   (*resource_start == '\0') ? M2M_DEFAULT_OTA_RESOURCE : resource_start);
  } else {
    return false;
  }

  (void)extract_json_string_value(payload,
                                  "target_version",
                                  ota_event.target_version,
                                  sizeof(ota_event.target_version));

  return osMessageQueuePut(ota_event_q, &ota_event, 0U, 0U) == osOK;
}

static bool wait_for_mqtt_connection(uint32_t timeout_ms)
{
  uint32_t start_ms = now_ms();

  while ((now_ms() - start_ms) < timeout_ms) {
    if (mqtt_connected) {
      return true;
    }
    osDelay(50U);
  }

  return mqtt_connected;
}

#if M2M_STARTUP_WELCOME_ENABLED
static bool wait_for_mqtt_ready(uint32_t timeout_ms)
{
  uint32_t start_ms = now_ms();

  while ((now_ms() - start_ms) < timeout_ms) {
    if (mqtt_connected && mqtt_subscribed) {
      return true;
    }
    osDelay(50U);
  }

  return mqtt_connected && mqtt_subscribed;
}
#endif

static void clear_voice_reply_queue(void)
{
  voice_reply_msg_t stale_reply = { 0 };
  uint32_t cleared = 0U;

  if (voice_reply_q == NULL) {
    return;
  }

  while (osMessageQueueGet(voice_reply_q, &stale_reply, NULL, 0U) == osOK) {
    cleared++;
  }

  if (cleared > 0U) {
    printf("[voice] cleared stale reply queue count=%lu\r\n", (unsigned long)cleared);
  }
}

static bool wait_for_voice_reply(const char *request_id, voice_reply_msg_t *reply, uint32_t timeout_ms)
{
  uint32_t start_ms = now_ms();
  uint32_t last_wait_log_ms = start_ms;

  if ((request_id == NULL) || (reply == NULL)) {
    return false;
  }

  while ((now_ms() - start_ms) < timeout_ms) {
    voice_reply_msg_t queued_reply = { 0 };
    uint32_t elapsed_ms = now_ms() - start_ms;
    uint32_t remaining_ms = (elapsed_ms < timeout_ms) ? (timeout_ms - elapsed_ms) : 0U;
    uint32_t wait_slice_ms = (remaining_ms > 250U) ? 250U : remaining_ms;
    osStatus_t queue_status;

    if (remaining_ms == 0U) {
      break;
    }

    queue_status = osMessageQueueGet(voice_reply_q, &queued_reply, NULL, wait_slice_ms);
    if (queue_status == osOK) {
      printf("[voice] dequeued reply request_id=%s expected=%s audio_url=%s\r\n",
             queued_reply.request_id,
             request_id,
             (queued_reply.audio_url[0] == '\0') ? "<missing>" : queued_reply.audio_url);

      if (strcmp(queued_reply.request_id, request_id) == 0) {
        *reply = queued_reply;
        return true;
      }

      printf("[voice] ignoring reply for stale request_id=%s while waiting for %s\r\n",
             queued_reply.request_id,
             request_id);
      continue;
    }

    if (queue_status == osErrorTimeout) {
      uint32_t now = now_ms();

      if ((now - last_wait_log_ms) >= 1000U) {
        last_wait_log_ms = now;
        printf("[voice] still waiting request_id=%s elapsed_ms=%lu queue_count=%lu stack_free=%lu\r\n",
               request_id,
               (unsigned long)(now - start_ms),
               (unsigned long)((voice_reply_q != NULL) ? osMessageQueueGetCount(voice_reply_q) : 0U),
               (unsigned long)osThreadGetStackSpace(osThreadGetId()));
      }
      continue;
    }

    printf("[voice] reply queue get failed request_id=%s status=%d\r\n",
           request_id,
           (int)queue_status);
    break;
  }

  return false;
}

static bool play_cloud_text_prompt(const char *prompt_tag, const char *speech_text)
{
  tts_prompt_workspace_t *workspace = &tts_prompt_workspace;
  size_t reply_audio_size = 0U;
  size_t payload_offset = 0U;
  uint32_t request_number = 0U;
  bool played = false;
  bool payload_complete = true;
  sl_status_t status;

  if ((speech_text == NULL) || (speech_text[0] == '\0')) {
    return false;
  }

  if (ensure_psram_media_ready() != SL_STATUS_OK) {
    printf("[voice-tts] skipped tag=%s reason=psram_unavailable\r\n",
           (prompt_tag == NULL) ? "unknown" : prompt_tag);
    return false;
  }

  memset(workspace, 0, sizeof(*workspace));

  if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
    workspace->snapshot = device_state;
    (void)osMutexRelease(device_state_mutex);
  }

  if (!workspace->snapshot.wifi_connected || !workspace->snapshot.mqtt_connected) {
    printf("[voice-tts] skipped tag=%s reason=cloud_offline wifi=%s mqtt=%s\r\n",
           (prompt_tag == NULL) ? "unknown" : prompt_tag,
           workspace->snapshot.wifi_connected ? "yes" : "no",
           workspace->snapshot.mqtt_connected ? "yes" : "no");
    return false;
  }

  if (!try_reserve_voice_session(&request_number)) {
    printf("[voice-tts] skipped tag=%s reason=voice_busy\r\n",
           (prompt_tag == NULL) ? "unknown" : prompt_tag);
    return false;
  }

  state_set_local_activity(LOCAL_ACTIVITY_WAITING_REPLY);
  build_log_preview((const uint8_t *)speech_text,
                    strlen(speech_text),
                    workspace->text_preview,
                    sizeof(workspace->text_preview));
  (void)escape_json_string(workspace->escaped_speech_text,
                           sizeof(workspace->escaped_speech_text),
                           speech_text,
                           M2M_AUDIO_PROMPT_TEXT_MAX - 1U);
  format_request_id("tts", request_number, workspace->request_id, sizeof(workspace->request_id));
  clear_voice_reply_queue();
  queue_status_publish();

  workspace->payload[0] = '\0';
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "{\"request_id\":\"");
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 workspace->request_id) && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "\",\"device_id\":\"") && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 M2M_DEVICE_ID) && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "\",\"mode\":\"notice\",\"audio_url\":null,"
                                 "\"image_url\":null,\"text_prompt\":\"") && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 workspace->escaped_speech_text) && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "\",\"dev_board_stub\":false,\"fw_version\":\"") && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 M2M_FIRMWARE_VERSION) && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "\",\"uptime_ms\":") && payload_complete;
  payload_complete = append_uint32_text(workspace->payload,
                                        sizeof(workspace->payload),
                                        &payload_offset,
                                        now_ms()) && payload_complete;
  payload_complete = append_text(workspace->payload,
                                 sizeof(workspace->payload),
                                 &payload_offset,
                                 "}") && payload_complete;

  if (!payload_complete) {
    printf("[voice-tts] payload truncated request_id=%s tag=%s\r\n",
           workspace->request_id,
           (prompt_tag == NULL) ? "unknown" : prompt_tag);
  }

  queue_cloud_payload(M2M_TOPIC_DEVICE_VOICE_REQUEST, workspace->payload, SL_MQTT_QOS_LEVEL_1, false);
  printf("[voice-tts] request queued request_id=%s tag=%s text=%s\r\n",
         workspace->request_id,
         (prompt_tag == NULL) ? "unknown" : prompt_tag,
         workspace->text_preview);

  if (!wait_for_voice_reply(workspace->request_id, &workspace->reply, M2M_TTS_PROMPT_REPLY_WAIT_MS)) {
    printf("[voice-tts] reply timeout request_id=%s tag=%s\r\n",
           workspace->request_id,
           (prompt_tag == NULL) ? "unknown" : prompt_tag);
    goto voice_tts_cleanup;
  }

  if (workspace->reply.audio_url[0] == '\0') {
    printf("[voice-tts] missing audio_url request_id=%s tag=%s\r\n",
           workspace->request_id,
           (prompt_tag == NULL) ? "unknown" : prompt_tag);
    goto voice_tts_cleanup;
  }

  state_set_local_activity(LOCAL_ACTIVITY_DOWNLOADING);
  status = http_download_binary(workspace->reply.audio_url,
                                psram_media.reply_audio_buffer,
                                psram_media.reply_audio_capacity,
                                &reply_audio_size,
                                M2M_VOICE_REPLY_AUDIO_DOWNLOAD_TIMEOUT_MS);
  if (status != SL_STATUS_OK) {
    printf("[voice-tts] download failed request_id=%s tag=%s status=0x%lx\r\n",
           workspace->request_id,
           (prompt_tag == NULL) ? "unknown" : prompt_tag,
           (unsigned long)status);
    goto voice_tts_cleanup;
  }

  printf("[voice-tts] playback begin request_id=%s tag=%s bytes=%lu\r\n",
         workspace->request_id,
         (prompt_tag == NULL) ? "unknown" : prompt_tag,
         (unsigned long)reply_audio_size);
  state_set_local_activity(LOCAL_ACTIVITY_PLAYING_AUDIO);
  played = m2m_dev_board_play_wav(psram_media.reply_audio_buffer, reply_audio_size);
  printf("[voice-tts] playback %s request_id=%s tag=%s\r\n",
         played ? "done" : "failed",
         workspace->request_id,
         (prompt_tag == NULL) ? "unknown" : prompt_tag);

voice_tts_cleanup:
  state_set_voice_busy(false);
  state_set_local_activity(LOCAL_ACTIVITY_IDLE);
  queue_status_publish();
  return played;
}

static void play_boot_led_pattern(void)
{
  printf("[m2m] boot pattern count=%u fw=%s\r\n",
         (unsigned int)M2M_DEVBOARD_BOOT_LED_PATTERN_COUNT,
         M2M_FIRMWARE_VERSION);

  for (uint32_t i = 0; i < M2M_DEVBOARD_BOOT_LED_PATTERN_COUNT; i++) {
    state_set_debug_led(true);
    osDelay(M2M_DEVBOARD_BOOT_LED_PATTERN_ON_MS);
    state_set_debug_led(false);
    osDelay(M2M_DEVBOARD_BOOT_LED_PATTERN_OFF_MS);
  }
}

static sl_status_t ota_fw_update_response_handler(sl_wifi_event_t event,
                                                  uint16_t *data,
                                                  uint32_t data_length,
                                                  void *arg)
{
  (void)data;
  (void)data_length;
  (void)arg;

  ota_response_complete = true;
  ota_callback_status = SL_WIFI_CHECK_IF_EVENT_FAILED(event) ? SL_STATUS_FAIL : SL_STATUS_OK;
  return ota_callback_status;
}

static sl_status_t perform_http_ota_update(const ota_event_msg_t *ota_event)
{
  sl_si91x_http_otaf_params_t http_params = { 0 };
  bool use_tls = false;
  uint16_t port = 80U;
  uint16_t flags = 0U;
  char host[48];
  char resource[M2M_OTA_RESOURCE_MAX];
  sl_status_t status;

  if ((ota_event == NULL) || !parse_http_url(ota_event->url,
                                             &use_tls,
                                             host,
                                             sizeof(host),
                                             &port,
                                             resource,
                                             sizeof(resource))) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  if (use_tls) {
    flags |= BIT(0);
  }

  ota_response_complete = false;
  ota_callback_status = SL_STATUS_FAIL;

  status = sl_wifi_set_callback(SL_WIFI_HTTP_OTA_FW_UPDATE_EVENTS,
                                (sl_wifi_callback_function_t)ota_fw_update_response_handler,
                                NULL);
  if (status != SL_STATUS_OK) {
    printf("[ota] failed to register OTA callback: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  http_params.flags = flags;
  http_params.ip_address = (uint8_t *)host;
  http_params.port = port;
  http_params.resource = (uint8_t *)resource;
  http_params.host_name = (uint8_t *)host;
  http_params.extended_header = NULL;
  http_params.user_name = (uint8_t *)"";
  http_params.password = (uint8_t *)"";

  printf("[ota] downloading host=%s port=%u resource=%s\r\n",
         host,
         (unsigned int)port,
         resource);
  status = sl_si91x_http_otaf_v2(&http_params);
  if (status != SL_STATUS_OK) {
    printf("[ota] sl_si91x_http_otaf_v2 failed: 0x%lx\r\n", (unsigned long)status);
    return status;
  }

  if (ota_response_complete && (ota_callback_status != SL_STATUS_OK)) {
    printf("[ota] OTA callback reported failure: 0x%lx\r\n", (unsigned long)ota_callback_status);
    return ota_callback_status;
  }

  return SL_STATUS_OK;
}

static void status_indicator_task(void *argument)
{
  led_status_t last_status = LED_STATUS_BOOT;
  uint32_t status_start_ms = now_ms();
  bool last_led_on = false;

  (void)argument;
  printf("[indicator] local LED status task start\r\n");

  while (1) {
    led_status_t status = indicator_select_led_status();
    uint32_t current_ms = now_ms();
    bool led_on;

    if (status != last_status) {
      last_status = status;
      status_start_ms = current_ms;
      printf("[indicator] led_status=%s\r\n", led_status_name(status));
    }

    led_on = led_status_is_on(status, current_ms - status_start_ms);
    if (led_on != last_led_on) {
      m2m_dev_board_set_debug_led(led_on);
      last_led_on = led_on;
    }

    osDelay(50U);
  }
}

static void mqtt_task(void *argument)
{
  sl_status_t status;
  uint32_t last_status_ms = 0;

  (void)argument;
  mqtt_task_thread_id = osThreadGetId();
  log_thread_snapshot("mqtt-task-start", mqtt_task_thread_id);
  log_heap_snapshot("mqtt-task-start-heap");

  status = wifi_start();
  (void)osEventFlagsSet(system_flags, SYSTEM_FLAG_RUNTIME_READY);
  if (status != SL_STATUS_OK) {
    state_set_local_activity(LOCAL_ACTIVITY_ERROR);
    queue_audio_prompt(AUDIO_PROMPT_NOTICE, "wifi_failed");
    return;
  }

  status = mqtt_start();
  if (status != SL_STATUS_OK) {
    queue_audio_prompt(AUDIO_PROMPT_NOTICE, "mqtt_failed");
  }

  while (1) {
    cloud_tx_msg_t queued_message;

    if (status_publish_pending) {
      status_publish_pending = false;
      queue_status_publish();
    }

    if (mqtt_connected && !mqtt_subscribed && !ota_exclusive_mode) {
      (void)mqtt_subscribe_all();
    }

    if (osMessageQueueGet(cloud_tx_q, &queued_message, NULL, 100U) == osOK) {
      if (mqtt_connected && !ota_exclusive_mode) {
        status = mqtt_publish_message(&queued_message);
        if ((status != SL_STATUS_OK) && (status != SL_STATUS_IN_PROGRESS)) {
          printf("[mqtt] publish failed topic=%s status=0x%lx (%s)\r\n",
                 queued_message.topic,
                 (unsigned long)status,
                 sl_status_name(status));
        }
      } else {
        char payload_preview[128];

        build_log_preview((const uint8_t *)queued_message.payload,
                          strlen(queued_message.payload),
                          payload_preview,
                          sizeof(payload_preview));
        printf("[mqtt] drop while offline topic=%s payload_preview=%s\r\n",
               queued_message.topic,
               payload_preview);
      }
    }

    if ((now_ms() - last_status_ms) >= M2M_STATUS_PERIOD_MS) {
      last_status_ms = now_ms();
      queue_status_publish();
    }
  }
}

static void system_control_task(void *argument)
{
  uint32_t last_check_ms = now_ms();
  m2m_imu_sample_t pending_fall_sample = { 0 };
  bool button_press_consumed = false;

  (void)argument;
  wait_for_runtime_ready();
  play_boot_led_pattern();

  while (1) {
    button_event_t button_event;
    sensor_event_t sensor_event;
    device_state_t snapshot;

    while (osMessageQueueGet(button_event_q, &button_event, NULL, 0U) == osOK) {
      if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
        snapshot = device_state;
        (void)osMutexRelease(device_state_mutex);
      } else {
        memset(&snapshot, 0, sizeof(snapshot));
      }

      printf("[button] event=%s tick=%lu voice_busy=%s voice_recording=%s ota=%s fall_pending=%s\r\n",
             button_event_name(button_event.type),
             (unsigned long)button_event.tick_ms,
             snapshot.voice_busy ? "yes" : "no",
             snapshot.voice_recording ? "yes" : "no",
             snapshot.ota_in_progress ? "yes" : "no",
             snapshot.fall_pending ? "yes" : "no");

      if (button_event.type == BUTTON_EVENT_PRESSED) {
        button_press_consumed = false;
        state_set_button_pressed(true);
        publish_button_state(true);

        if (snapshot.ota_in_progress) {
          button_press_consumed = true;
          queue_audio_prompt(AUDIO_PROMPT_OTA_STATUS, "ota_busy");
          continue;
        }

        if ((osEventFlagsGet(system_flags) & SYSTEM_FLAG_FALL_PENDING) != 0U) {
          button_press_consumed = true;
          state_set_fall_pending(false, 0U);
          publish_fall_ack("cancelled_by_button");
          queue_spoken_audio_prompt(AUDIO_PROMPT_NOTICE, "Fall cancelled.");
          continue;
        }

        if (snapshot.voice_recording) {
          voice_stop_requested = true;
          button_press_consumed = true;
          printf("[button] stop requested for active recording\r\n");
        }
      } else if (button_event.type == BUTTON_EVENT_RELEASED) {
        state_set_button_pressed(false);
        publish_button_state(false);
        if (button_press_consumed) {
          button_press_consumed = false;
          continue;
        }

        if (snapshot.ota_in_progress) {
          queue_audio_prompt(AUDIO_PROMPT_OTA_STATUS, "ota_busy");
        } else if (snapshot.voice_busy) {
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_busy");
        } else if (!queue_voice_event(VOICE_EVENT_START_QA)) {
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_queue_full");
          printf("[button] failed to queue short-press QA request\r\n");
        } else {
          printf("[button] queued short-press QA request\r\n");
        }
        button_press_consumed = false;
      } else {
        if (button_press_consumed) {
          continue;
        }

        button_press_consumed = true;
        if (snapshot.ota_in_progress) {
          queue_audio_prompt(AUDIO_PROMPT_OTA_STATUS, "ota_busy");
        } else if (snapshot.voice_busy) {
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_busy");
        } else if (!queue_voice_event(VOICE_EVENT_START_VISION)) {
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "vision_queue_full");
          printf("[button] failed to queue long-press vision request\r\n");
        } else {
          printf("[button] queued long-press vision request\r\n");
        }
      }
    }

    while (osMessageQueueGet(sensor_event_q, &sensor_event, NULL, 0U) == osOK) {
      if (sensor_event.type == SENSOR_EVENT_FALL_CANDIDATE) {
        pending_fall_sample = sensor_event.sample;
        state_set_fall_pending(true, now_ms() + M2M_FALL_CONFIRM_TIMEOUT_MS);
        queue_spoken_audio_prompt(AUDIO_PROMPT_FALL_CONFIRM,
                                  "Possible fall detected. Are you okay? Press the button within thirty seconds to cancel.");
        publish_fall_event("candidate", &pending_fall_sample);
      }
    }

    if ((now_ms() - last_check_ms) >= 100U) {
      last_check_ms = now_ms();

      if (osMutexAcquire(device_state_mutex, osWaitForever) == osOK) {
        snapshot = device_state;
        (void)osMutexRelease(device_state_mutex);
      } else {
        memset(&snapshot, 0, sizeof(snapshot));
      }

      if (snapshot.fall_pending && ((int32_t)(now_ms() - snapshot.fall_deadline_ms) >= 0)) {
        state_set_fall_pending(false, 0U);
        publish_fall_event("alert", &pending_fall_sample);
        queue_spoken_audio_prompt(AUDIO_PROMPT_NOTICE, "Fall alert sent.");
      }
    }

    osDelay(20U);
  }
}

static void imu_task(void *argument)
{
  (void)argument;
  wait_for_runtime_ready();
  printf("[imu] task start sample_period_ms=%u\r\n", (unsigned int)M2M_IMU_SAMPLE_PERIOD_MS);

  while (1) {
    m2m_imu_sample_t sample = { 0 };

    m2m_dev_board_read_imu(&sample);
    if (m2m_dev_board_is_fall_candidate(&sample)) {
      sensor_event_t event = { 0 };
      osStatus_t queue_status;

      event.type = SENSOR_EVENT_FALL_CANDIDATE;
      event.sample = sample;
      event.tick_ms = now_ms();
      queue_status = osMessageQueuePut(sensor_event_q, &event, 0U, 0U);
      printf("[imu] fall candidate queued status=%ld accel_mg=[%d,%d,%d]\r\n",
             (long)queue_status,
             sample.accel_mg[0],
             sample.accel_mg[1],
             sample.accel_mg[2]);
    }

    osDelay(M2M_IMU_SAMPLE_PERIOD_MS);
  }
}

static void voice_ai_task(void *argument)
{
  (void)argument;
  wait_for_runtime_ready();
  voice_ai_task_thread_id = osThreadGetId();
  log_thread_snapshot("voice-ai-start", voice_ai_task_thread_id);
  log_heap_snapshot("voice-ai-start-heap");

  while (1) {
    voice_event_msg_t voice_event;

    if (osMessageQueueGet(voice_event_q, &voice_event, NULL, osWaitForever) == osOK) {
      char payload[M2M_MQTT_PAYLOAD_MAX];
      char request_id[24];
      char audio_url[M2M_OTA_URL_MAX] = { 0 };
      char image_url[M2M_OTA_URL_MAX] = { 0 };
      char image_url_json[M2M_OTA_URL_MAX + 2U] = "null";
      const bool capture_image = (voice_event.type == VOICE_EVENT_START_VISION);
      const char *mode = capture_image ? "vision" : "qa";
      const char *start_prompt = capture_image ? "vision_start" : "voice_start";
      uint32_t request_number;
      uint32_t capture_start_ms;
      bool stopped_by_button = false;
      size_t audio_sample_count = 0U;
      size_t image_size = 0U;
      size_t reply_audio_size = 0U;
      uint32_t image_capture_attempt = 0U;
      sl_status_t status = SL_STATUS_OK;
      voice_reply_msg_t reply = { 0 };

      if (ensure_psram_media_ready() != SL_STATUS_OK) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "psram_unavailable");
        continue;
      }

      if (!try_reserve_voice_session(&request_number)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_busy");
        continue;
      }

      format_request_id("req", request_number, request_id, sizeof(request_id));
      clear_voice_reply_queue();
      voice_stop_requested = false;
      state_set_local_activity(LOCAL_ACTIVITY_VOICE_RECORDING);
      state_set_voice_recording(true);
      queue_status_publish();

      m2m_dev_board_play_prompt(start_prompt, request_id);
      capture_start_ms = now_ms();
      if (!m2m_dev_board_capture_audio_pcm(psram_media.recording_pcm,
                                           psram_media.recording_pcm_capacity_samples,
                                           M2M_VOICE_CAPTURE_AUTO_STOP_MS,
                                           &voice_stop_requested,
                                           &stopped_by_button,
                                           &audio_sample_count)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "audio_capture_failed");
        goto voice_request_cleanup;
      }

      if ((now_ms() - capture_start_ms) > (M2M_VOICE_CAPTURE_AUTO_STOP_MS + M2M_VOICE_CAPTURE_START_BUDGET_MS)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_capture_late");
      }

      if (audio_sample_count == 0U) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "audio_capture_empty");
        goto voice_request_cleanup;
      }

      printf("[voice] capture stats request_id=%s mode=%s stopped_by_button=%s\r\n",
             request_id,
             mode,
             stopped_by_button ? "yes" : "no");
      print_audio_capture_stats(psram_media.recording_pcm, audio_sample_count);

      if (capture_image) {
        bool image_captured = false;

        for (image_capture_attempt = 1U; image_capture_attempt <= 2U; ++image_capture_attempt) {
          image_captured = m2m_dev_board_capture_image(psram_media.image_buffer,
                                                       psram_media.image_buffer_capacity,
                                                       &image_size);
          if (image_captured) {
            if (image_capture_attempt > 1U) {
              printf("[voice] image capture recovered request_id=%s attempt=%lu bytes=%lu\r\n",
                     request_id,
                     (unsigned long)image_capture_attempt,
                     (unsigned long)image_size);
            }
            break;
          }

          if (image_capture_attempt < 2U) {
            printf("[voice] image capture retry request_id=%s next_attempt=%lu\r\n",
                   request_id,
                   (unsigned long)(image_capture_attempt + 1U));
            osDelay(200U);
          }
        }

        if (!image_captured) {
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "image_capture_failed");
          goto voice_request_cleanup;
        }
      }

      state_set_voice_recording(false);
      state_set_local_activity(LOCAL_ACTIVITY_UPLOADING);
      queue_status_publish();
      m2m_dev_board_play_prompt("record_stop_upload", stopped_by_button ? "button" : "timeout");

      status = http_upload_audio_recording(request_id,
                                           psram_media.recording_pcm,
                                           audio_sample_count,
                                           audio_url,
                                           sizeof(audio_url));
      if (status != SL_STATUS_OK) {
        printf("[voice] audio upload failed request_id=%s status=0x%lx\r\n",
               request_id,
               (unsigned long)status);
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "audio_upload_failed");
        goto voice_request_cleanup;
      }

      if (capture_image) {
        status = http_upload_binary_chunked(request_id,
                                            "vision",
                                            "capture.jpg",
                                            "image/jpeg",
                                            psram_media.image_buffer,
                                            image_size,
                                            image_url,
                                            sizeof(image_url));
        if (status != SL_STATUS_OK) {
          printf("[voice] image upload failed request_id=%s status=0x%lx\r\n",
                 request_id,
                 (unsigned long)status);
          queue_audio_prompt(AUDIO_PROMPT_NOTICE, "image_upload_failed");
          goto voice_request_cleanup;
        }

        (void)snprintf(image_url_json, sizeof(image_url_json), "\"%s\"", image_url);
      }

      (void)snprintf(payload,
                     sizeof(payload),
                     "{\"request_id\":\"%s\",\"device_id\":\"%s\",\"mode\":\"%s\","
                     "\"audio_url\":\"%s\",\"image_url\":%s,"
                     "\"dev_board_stub\":false,\"fw_version\":\"%s\","
                     "\"uptime_ms\":%" PRIu32 "}",
                     request_id,
                     M2M_DEVICE_ID,
                     mode,
                     audio_url,
                     image_url_json,
                     M2M_FIRMWARE_VERSION,
                     now_ms());
      queue_cloud_payload(M2M_TOPIC_DEVICE_VOICE_REQUEST, payload, SL_MQTT_QOS_LEVEL_1, false);
      queue_audio_prompt(AUDIO_PROMPT_VOICE_REQUEST_SENT, request_id);
      state_set_local_activity(LOCAL_ACTIVITY_WAITING_REPLY);

      printf("[voice] wait reply begin request_id=%s timeout_ms=%u stack_free=%lu\r\n",
             request_id,
             (unsigned int)M2M_VOICE_REPLY_WAIT_MS,
             (unsigned long)osThreadGetStackSpace(osThreadGetId()));
      if (!wait_for_voice_reply(request_id, &reply, M2M_VOICE_REPLY_WAIT_MS)) {
        printf("[voice] wait reply timeout request_id=%s stack_free=%lu\r\n",
               request_id,
               (unsigned long)osThreadGetStackSpace(osThreadGetId()));
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_reply_timeout");
        goto voice_request_cleanup;
      }
      printf("[voice] wait reply done request_id=%s audio_url=%s stack_free=%lu\r\n",
             request_id,
             (reply.audio_url[0] == '\0') ? "<missing>" : reply.audio_url,
             (unsigned long)osThreadGetStackSpace(osThreadGetId()));

      if (reply.audio_url[0] == '\0') {
        char text_preview[128];

        build_log_preview((const uint8_t *)reply.text, strlen(reply.text), text_preview, sizeof(text_preview));
        printf("[voice] cloud reply missing audio_url request_id=%s text=%s\r\n",
               request_id,
               text_preview);
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "voice_reply_invalid");
        goto voice_request_cleanup;
      }

      printf("[voice] reply audio download begin request_id=%s url=%s timeout_ms=%u\r\n",
             request_id,
             reply.audio_url,
             (unsigned int)M2M_VOICE_REPLY_AUDIO_DOWNLOAD_TIMEOUT_MS);
      state_set_local_activity(LOCAL_ACTIVITY_DOWNLOADING);
      status = http_download_binary(reply.audio_url,
                                    psram_media.reply_audio_buffer,
                                    psram_media.reply_audio_capacity,
                                    &reply_audio_size,
                                    M2M_VOICE_REPLY_AUDIO_DOWNLOAD_TIMEOUT_MS);
      if (status != SL_STATUS_OK) {
        printf("[voice] reply audio download failed request_id=%s url=%s status=0x%lx\r\n",
               request_id,
               reply.audio_url,
               (unsigned long)status);
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "reply_audio_download_failed");
        goto voice_request_cleanup;
      }

      printf("[voice] reply audio ready request_id=%s bytes=%lu first4=%02X %02X %02X %02X\r\n",
             request_id,
             (unsigned long)reply_audio_size,
             (reply_audio_size > 0U) ? psram_media.reply_audio_buffer[0] : 0U,
             (reply_audio_size > 1U) ? psram_media.reply_audio_buffer[1] : 0U,
             (reply_audio_size > 2U) ? psram_media.reply_audio_buffer[2] : 0U,
             (reply_audio_size > 3U) ? psram_media.reply_audio_buffer[3] : 0U);
      printf("[voice] reply audio playback begin request_id=%s\r\n", request_id);

      state_set_local_activity(LOCAL_ACTIVITY_PLAYING_AUDIO);
      if (!m2m_dev_board_play_wav(psram_media.reply_audio_buffer, reply_audio_size)) {
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "reply_audio_play_failed");
        goto voice_request_cleanup;
      }
      printf("[voice] reply audio playback end request_id=%s\r\n", request_id);

      {
        char text_preview[128];

        build_log_preview((const uint8_t *)reply.text, strlen(reply.text), text_preview, sizeof(text_preview));
        printf("[voice] completed request_id=%s mode=%s text=%s\r\n",
               request_id,
               mode,
               (reply.text[0] == '\0') ? "<none>" : text_preview);
      }

voice_request_cleanup:
      printf("[voice] cleanup request_id=%s voice_busy->no recording->no\r\n", request_id);
      voice_stop_requested = false;
      state_set_voice_recording(false);
      state_set_voice_busy(false);
      state_set_local_activity(LOCAL_ACTIVITY_IDLE);
      queue_status_publish();
    }
  }
}

static void ota_task(void *argument)
{
  (void)argument;
  wait_for_runtime_ready();

  while (1) {
    ota_event_msg_t ota_event;

    if (osMessageQueueGet(ota_event_q, &ota_event, NULL, osWaitForever) == osOK) {
      sl_status_t status;

      state_set_local_activity(LOCAL_ACTIVITY_OTA);
      state_set_ota(true, "starting", ota_event.target_version);
      queue_ota_status_publish("requested", "dashboard_request", ota_event.url, ota_event.target_version);
      queue_audio_prompt(AUDIO_PROMPT_OTA_STATUS, "ota_request_received");
      queue_status_publish();

      osDelay(M2M_OTA_STATUS_FLUSH_MS);
      ota_exclusive_mode = true;

      status = mqtt_stop();
      if (status != SL_STATUS_OK) {
        ota_exclusive_mode = false;
        state_set_ota(false, "disconnect_failed", ota_event.target_version);
        queue_audio_prompt(AUDIO_PROMPT_NOTICE, "ota_disconnect_failed");
        state_set_local_activity(LOCAL_ACTIVITY_IDLE);
        queue_status_publish();
        continue;
      }

      status = perform_http_ota_update(&ota_event);
      if (status == SL_STATUS_OK) {
        printf("[ota] firmware download complete, rebooting into new image\r\n");
        m2m_dev_board_set_debug_led(true);
        printf("[ota] reboot settle wait=%u ms before soc reset\r\n",
               (unsigned int)M2M_OTA_REBOOT_DELAY_MS);
        osDelay(M2M_OTA_REBOOT_DELAY_MS);
        sl_si91x_soc_nvic_reset();
      }

      printf("[ota] OTA failed with status=0x%lx\r\n", (unsigned long)status);
      ota_exclusive_mode = false;

      status = mqtt_start();
      if (status == SL_STATUS_OK) {
        (void)wait_for_mqtt_connection(M2M_OTA_RECONNECT_WAIT_MS);
      }

      state_set_ota(false, "failed", ota_event.target_version);
      queue_ota_status_publish("failed", "download_or_flash_failed", ota_event.url, ota_event.target_version);
      queue_audio_prompt(AUDIO_PROMPT_NOTICE, "ota_failed");
      state_set_local_activity(LOCAL_ACTIVITY_IDLE);
      queue_status_publish();
    }
  }
}

static void reminder_audio_task(void *argument)
{
  (void)argument;
  wait_for_runtime_ready();

  log_thread_snapshot("reminder-audio-start", osThreadGetId());
  log_heap_snapshot("reminder-audio-start-heap");

  while (1) {
    audio_prompt_msg_t prompt;

    if (osMessageQueueGet(audio_q, &prompt, NULL, osWaitForever) == osOK) {
      const char *tag = "notice";

      switch (prompt.type) {
        case AUDIO_PROMPT_VOICE_REQUEST_SENT:
          tag = "voice_request_sent";
          break;
        case AUDIO_PROMPT_VOICE_REPLY:
          tag = "voice_reply";
          break;
        case AUDIO_PROMPT_FALL_CONFIRM:
          tag = "fall_confirm";
          break;
        case AUDIO_PROMPT_REMINDER:
          tag = "reminder";
          break;
        case AUDIO_PROMPT_OTA_STATUS:
          tag = "ota_status";
          break;
        case AUDIO_PROMPT_NOTICE:
        default:
          tag = "notice";
          break;
      }

      if (prompt.use_cloud_speech) {
        if (play_cloud_text_prompt(tag, prompt.text)) {
          continue;
        }

        printf("[voice-tts] fallback tag=%s detail=%s\r\n",
               tag,
               prompt.text);
      }

      m2m_dev_board_play_prompt(tag, prompt.text);
    }
  }
}

static void startup_diag_task(void *argument)
{
  uint32_t flags;

  (void)argument;
  wait_for_runtime_ready();

  flags = osEventFlagsWait(system_flags, SYSTEM_FLAG_WIFI_CONNECTED, osFlagsWaitAny, 5000U);
  if ((flags & osFlagsError) == 0U) {
    printf("[diag] startup self-test waiting complete: wifi_connected=yes\r\n");
  } else {
    printf("[diag] startup self-test waiting complete: wifi_connected=no\r\n");
  }

  osDelay(500U);
  run_dev_board_diagnostic();
#if M2M_STARTUP_WELCOME_ENABLED
  if (wait_for_mqtt_ready(M2M_STARTUP_WELCOME_WAIT_MS)) {
    queue_spoken_audio_prompt(AUDIO_PROMPT_NOTICE, M2M_STARTUP_WELCOME_TEXT);
    printf("[startup] queued welcome prompt fw=%s\r\n", M2M_FIRMWARE_VERSION);
  } else {
    printf("[startup] welcome prompt skipped: mqtt_not_ready fw=%s\r\n",
           M2M_FIRMWARE_VERSION);
  }
#endif
  (void)osEventFlagsClear(system_flags, SYSTEM_FLAG_STARTUP_ACTIVE);
  printf("[startup] startup sequence complete: button input enabled\r\n");
  osThreadExit();
}

static void dev_board_input_task(void *argument)
{
  bool last_raw_pressed = false;
  bool stable_pressed = false;
  uint32_t last_change_ms = now_ms();
  uint32_t press_start_ms = 0;
  bool long_press_sent = false;
  bool suppress_until_release = false;

  (void)argument;
  wait_for_runtime_ready();

  while (1) {
    bool raw_pressed = m2m_dev_board_read_ptt_button();
    uint32_t current_ms = now_ms();

    if ((osEventFlagsGet(system_flags) & SYSTEM_FLAG_STARTUP_ACTIVE) != 0U) {
      if (raw_pressed) {
        suppress_until_release = true;
      }
      last_raw_pressed = raw_pressed;
      stable_pressed = raw_pressed;
      last_change_ms = current_ms;
      press_start_ms = current_ms;
      long_press_sent = false;
      osDelay(M2M_BUTTON_POLL_PERIOD_MS);
      continue;
    }

    if (raw_pressed != last_raw_pressed) {
      last_raw_pressed = raw_pressed;
      last_change_ms = current_ms;
      printf("[button] raw=%s tick=%lu\r\n",
             raw_pressed ? "pressed" : "released",
             (unsigned long)current_ms);
    }

    if (((current_ms - last_change_ms) >= M2M_BUTTON_DEBOUNCE_MS) && (stable_pressed != raw_pressed)) {
      button_event_t event = { 0 };
      osStatus_t put_status;
      stable_pressed = raw_pressed;

      if (suppress_until_release) {
        if (!stable_pressed) {
          suppress_until_release = false;
          printf("[button] startup-held press cleared tick=%lu\r\n",
                 (unsigned long)current_ms);
        }
        press_start_ms = current_ms;
        long_press_sent = false;
        osDelay(M2M_BUTTON_POLL_PERIOD_MS);
        continue;
      }

      event.type = stable_pressed ? BUTTON_EVENT_PRESSED : BUTTON_EVENT_RELEASED;
      event.tick_ms = current_ms;
      put_status = osMessageQueuePut(button_event_q, &event, 0U, 0U);
      printf("[button] debounced=%s tick=%lu queue=%s\r\n",
             stable_pressed ? "pressed" : "released",
             (unsigned long)current_ms,
             (put_status == osOK) ? "ok" : "fail");

      if (stable_pressed) {
        press_start_ms = current_ms;
        long_press_sent = false;
      }
    }

    if (stable_pressed && !long_press_sent && ((current_ms - press_start_ms) >= M2M_BUTTON_LONG_PRESS_MS)) {
      button_event_t event = {
        .type = BUTTON_EVENT_LONG_PRESS,
        .tick_ms = current_ms,
      };
      osStatus_t put_status = osMessageQueuePut(button_event_q, &event, 0U, 0U);
      printf("[button] long_press tick=%lu hold_ms=%lu queue=%s\r\n",
             (unsigned long)current_ms,
             (unsigned long)(current_ms - press_start_ms),
             (put_status == osOK) ? "ok" : "fail");
      long_press_sent = true;
    }

    osDelay(M2M_BUTTON_POLL_PERIOD_MS);
  }
}
