/*
 * Wi-Fi Audio Tone Test - tone streaming interface
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TONE_DEFAULT_SAMPLE_RATE_HZ     44100U
#define TONE_DEFAULT_PACKET_DURATION_MS 10U
#define TONE_DEFAULT_FREQUENCY_HZ       1000U
#define TONE_DEFAULT_AMPLITUDE_PCT      50U

#define TONE_MAX_SAMPLES_PER_PACKET CONFIG_TONE_MAX_SAMPLES_PER_PACKET
#define TONE_MAX_PAYLOAD_BYTES      (TONE_MAX_SAMPLES_PER_PACKET * sizeof(int16_t))

struct tone_stream_settings {
	uint32_t sample_rate_hz;
	uint16_t packet_duration_ms;
	uint16_t frequency_hz;
	uint8_t amplitude_pct;
	uint32_t dest_ipv4;
	uint16_t dest_port;
};

int tone_stream_init(void);
bool tone_stream_is_active(void);
int tone_stream_get_settings(struct tone_stream_settings *out);
int tone_stream_start(const struct shell *shell);
void tone_stream_stop(const struct shell *shell);
void tone_stream_status(const struct shell *shell);
int tone_stream_set_target(const char *ip_str, uint16_t port);
int tone_stream_set_params(uint16_t freq_hz, uint8_t amplitude_pct, uint32_t sample_rate_hz,
			   uint16_t packet_ms);
int tone_stream_adjust_amplitude(int delta_pct);
uint8_t tone_stream_get_current_amplitude(void);
int tone_stream_adjust_amplitude(int delta_pct);

#ifdef __cplusplus
}
#endif
