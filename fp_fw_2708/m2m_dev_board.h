#ifndef M2M_DEV_BOARD_H
#define M2M_DEV_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int16_t accel_mg[3];
  int16_t gyro_dps[3];
} m2m_imu_sample_t;

#define M2M_DEV_BOARD_CAPTURED_IMAGE_MAX_BYTES 196608U

void m2m_dev_board_init(void);
void m2m_dev_board_set_debug_led(bool on);
bool m2m_dev_board_get_debug_led(void);
bool m2m_dev_board_read_ptt_button(void);

void m2m_dev_board_read_imu(m2m_imu_sample_t *sample);
bool m2m_dev_board_run_imu_self_test(m2m_imu_sample_t *sample);
bool m2m_dev_board_is_fall_candidate(const m2m_imu_sample_t *sample);

bool m2m_dev_board_capture_audio_pcm(int16_t *pcm_buffer,
                                     size_t max_samples,
                                     uint32_t max_duration_ms,
                                     const volatile bool *stop_requested,
                                     bool *stopped_by_request,
                                     size_t *captured_samples);
bool m2m_dev_board_capture_image(uint8_t *buffer, size_t buffer_size, size_t *captured_size);
bool m2m_dev_board_run_speaker_self_test(void);
bool m2m_dev_board_play_wav(const uint8_t *wav_data, size_t wav_size);
void m2m_dev_board_play_prompt(const char *prompt_tag, const char *detail);

#endif // M2M_DEV_BOARD_H
