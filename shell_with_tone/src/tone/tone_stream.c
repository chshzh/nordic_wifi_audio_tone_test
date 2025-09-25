/*
 * Wi-Fi Audio Tone Test - tone streaming implementation
 */

#include "tone_stream.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(tone_stream, CONFIG_LOG_DEFAULT_LEVEL);

#define LUT_POINTS      1024U
#define PHASE_FRAC_BITS 22U
#define PHASE_FRAC_MASK ((1U << PHASE_FRAC_BITS) - 1U)

struct tone_packet_header {
	uint32_t seq;
	uint32_t sample_count;
	uint32_t timestamp_us;
} __packed;

struct tone_stream_context {
	struct tone_stream_settings settings;
	int sock_fd;
	bool streaming;
	uint32_t seq_num;
	uint32_t sample_counter;
	uint16_t samples_per_packet;
	uint32_t phase_acc;
	uint32_t phase_step;
	int16_t lut[LUT_POINTS];
	struct k_work_delayable work;
	struct k_mutex lock;
} static ctx;

static uint8_t tx_buffer[sizeof(struct tone_packet_header) + TONE_MAX_PAYLOAD_BYTES];

static inline uint32_t micros_now(void)
{
	return k_ticks_to_us_ceil32(k_uptime_ticks());
}

static int ensure_network_ready(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("No default network interface");
		return -ENODEV;
	}

	if (!net_if_is_admin_up(iface) || !net_if_is_up(iface)) {
		LOG_WRN("Default network interface is not up");
		return -ENETDOWN;
	}

	return 0;
}

static void update_phase_step(void)
{
	ctx.phase_step = ((uint64_t)ctx.settings.frequency_hz << PHASE_FRAC_BITS) * LUT_POINTS /
			 ctx.settings.sample_rate_hz;
}

static void generate_lut(void)
{
	const double amplitude = (ctx.settings.amplitude_pct / 100.0) * INT16_MAX;

	for (uint32_t i = 0; i < LUT_POINTS; i++) {
		double angle = (2.0 * M_PI * i) / LUT_POINTS;
		ctx.lut[i] = (int16_t)lrint(amplitude * sin(angle));
	}
}

static void fill_pcm_samples(int16_t *pcm, uint32_t samples)
{
	uint32_t phase = ctx.phase_acc;
	const uint32_t step = ctx.phase_step;

	for (uint32_t i = 0; i < samples; i++) {
		uint32_t index = (phase >> PHASE_FRAC_BITS) & (LUT_POINTS - 1U);
		pcm[i] = ctx.lut[index];
		phase = (phase + step) & ((LUT_POINTS << PHASE_FRAC_BITS) - 1U);
	}

	ctx.phase_acc = phase;
}

static int configure_destination_socket(void)
{
	struct sockaddr_in dest = {
		.sin_family = AF_INET,
		.sin_port = htons(ctx.settings.dest_port),
		.sin_addr.s_addr = ctx.settings.dest_ipv4,
	};

	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return -errno;
	}

	int buf = 64 * 1024;
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

	if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		int err = -errno;
		LOG_ERR("connect() failed: %d", errno);
		close(fd);
		return err;
	}

	ctx.sock_fd = fd;
	return 0;
}

static void reschedule_next_packet(uint32_t samples)
{
	const uint32_t interval_us =
		(uint32_t)(((uint64_t)samples * 1000000U) / ctx.settings.sample_rate_hz);
	k_work_reschedule(&ctx.work, K_USEC(MAX(interval_us, 1U)));
}

static void send_work_handler(struct k_work *item)
{
	ARG_UNUSED(item);

	k_mutex_lock(&ctx.lock, K_FOREVER);

	if (!ctx.streaming || ctx.sock_fd < 0) {
		k_mutex_unlock(&ctx.lock);
		return;
	}

	const uint32_t samples = ctx.samples_per_packet;

	struct tone_packet_header header = {
		.seq = sys_cpu_to_be32(ctx.seq_num++),
		.sample_count = sys_cpu_to_be32(ctx.sample_counter),
		.timestamp_us = sys_cpu_to_be32(micros_now()),
	};

	ctx.sample_counter += samples;

	size_t payload_bytes = samples * sizeof(int16_t);
	if (payload_bytes > TONE_MAX_PAYLOAD_BYTES) {
		LOG_ERR("Payload too large (%u bytes)", payload_bytes);
		ctx.streaming = false;
		k_mutex_unlock(&ctx.lock);
		return;
	}

	memcpy(tx_buffer, &header, sizeof(header));
	int16_t *pcm = (int16_t *)(tx_buffer + sizeof(header));
	fill_pcm_samples(pcm, samples);

	ssize_t sent = send(ctx.sock_fd, tx_buffer, sizeof(header) + payload_bytes, 0);
	if (sent < 0) {
		LOG_ERR("send() failed: %d", errno);
	}

	reschedule_next_packet(samples);
	k_mutex_unlock(&ctx.lock);
}

static void stop_locked(void)
{
	if (ctx.streaming) {
		ctx.streaming = false;
		k_work_cancel_delayable(&ctx.work);
	}

	if (ctx.sock_fd >= 0) {
		close(ctx.sock_fd);
		ctx.sock_fd = -1;
	}
}

int tone_stream_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	ctx.sock_fd = -1;
	ctx.settings.sample_rate_hz = TONE_DEFAULT_SAMPLE_RATE_HZ;
	ctx.settings.packet_duration_ms = TONE_DEFAULT_PACKET_DURATION_MS;
	ctx.settings.frequency_hz = TONE_DEFAULT_FREQUENCY_HZ;
	ctx.settings.amplitude_pct = TONE_DEFAULT_AMPLITUDE_PCT;

	k_mutex_init(&ctx.lock);
	k_work_init_delayable(&ctx.work, send_work_handler);
	generate_lut();
	update_phase_step();

	return 0;
}

bool tone_stream_is_active(void)
{
	k_mutex_lock(&ctx.lock, K_FOREVER);
	bool active = ctx.streaming;
	k_mutex_unlock(&ctx.lock);
	return active;
}

int tone_stream_get_settings(struct tone_stream_settings *out)
{
	if (!out) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);
	*out = ctx.settings;
	k_mutex_unlock(&ctx.lock);

	return 0;
}

int tone_stream_set_target(const char *ip_str, uint16_t port)
{
	if (!ip_str || port == 0) {
		return -EINVAL;
	}

	struct in_addr addr;
	if (zsock_inet_pton(AF_INET, ip_str, &addr) != 1) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);
	ctx.settings.dest_ipv4 = addr.s_addr;
	ctx.settings.dest_port = port;
	k_mutex_unlock(&ctx.lock);

	return 0;
}

int tone_stream_set_params(uint16_t freq_hz, uint8_t amplitude_pct, uint32_t sample_rate_hz,
			   uint16_t packet_ms)
{
	if (sample_rate_hz == 0U || packet_ms == 0U) {
		return -EINVAL;
	}

	uint16_t amp = MIN(amplitude_pct, 100U);
	uint32_t samples = DIV_ROUND_CLOSEST(sample_rate_hz * packet_ms, 1000U);
	if (samples == 0U || samples > TONE_MAX_SAMPLES_PER_PACKET) {
		return -ERANGE;
	}

	k_mutex_lock(&ctx.lock, K_FOREVER);
	ctx.settings.frequency_hz = freq_hz;
	ctx.settings.amplitude_pct = amp;
	ctx.settings.sample_rate_hz = sample_rate_hz;
	ctx.settings.packet_duration_ms = packet_ms;
	ctx.samples_per_packet = samples;
	ctx.phase_acc = 0;
	update_phase_step();
	generate_lut();
	k_mutex_unlock(&ctx.lock);

	return 0;
}

int tone_stream_start(const struct shell *shell)
{
	k_mutex_lock(&ctx.lock, K_FOREVER);

	if (ctx.streaming) {
		k_mutex_unlock(&ctx.lock);
		return -EALREADY;
	}

	if (ctx.settings.dest_port == 0U || ctx.settings.dest_ipv4 == 0U) {
		k_mutex_unlock(&ctx.lock);
		return -ENOTCONN;
	}

	int net_ready = ensure_network_ready();
	if (net_ready < 0) {
		k_mutex_unlock(&ctx.lock);
		return net_ready;
	}

	if (ctx.samples_per_packet == 0U) {
		ctx.samples_per_packet = DIV_ROUND_CLOSEST(
			ctx.settings.sample_rate_hz * ctx.settings.packet_duration_ms, 1000U);
		if (ctx.samples_per_packet == 0U ||
		    ctx.samples_per_packet > TONE_MAX_SAMPLES_PER_PACKET) {
			k_mutex_unlock(&ctx.lock);
			return -ERANGE;
		}
	}

	int ret = configure_destination_socket();
	if (ret < 0) {
		k_mutex_unlock(&ctx.lock);
		return ret;
	}

	ctx.seq_num = 0;
	ctx.sample_counter = 0;
	ctx.phase_acc = 0;
	update_phase_step();
	generate_lut();
	ctx.streaming = true;

	k_mutex_unlock(&ctx.lock);

	reschedule_next_packet(ctx.samples_per_packet);

	if (shell) {
		shell_print(shell, "Tone stream started: %u Hz, %u%%, %u ms packets",
			    ctx.settings.frequency_hz, ctx.settings.amplitude_pct,
			    ctx.settings.packet_duration_ms);
	}

	return 0;
}

void tone_stream_stop(const struct shell *shell)
{
	k_mutex_lock(&ctx.lock, K_FOREVER);
	stop_locked();
	k_mutex_unlock(&ctx.lock);

	if (shell) {
		shell_print(shell, "Tone stream stopped");
	}
}

void tone_stream_status(const struct shell *shell)
{
	if (!shell) {
		return;
	}

	struct tone_stream_settings settings;
	bool active;
	uint32_t packets;

	k_mutex_lock(&ctx.lock, K_FOREVER);
	settings = ctx.settings;
	active = ctx.streaming;
	packets = ctx.seq_num;
	k_mutex_unlock(&ctx.lock);

	char ip_buf[NET_IPV4_ADDR_LEN];

	if (settings.dest_ipv4 != 0U) {
		struct in_addr addr = {
			.s_addr = settings.dest_ipv4,
		};
		zsock_inet_ntop(AF_INET, &addr, ip_buf, sizeof(ip_buf));
	} else {
		strcpy(ip_buf, "unset");
	}

	shell_print(shell, "Tone state: %s", active ? "streaming" : "stopped");
	shell_print(shell, "  Destination: %s:%u", ip_buf, settings.dest_port);
	shell_print(shell, "  Tone: %u Hz @ %u%%", settings.frequency_hz, settings.amplitude_pct);
	shell_print(shell, "  Sample rate: %u Hz, packet %u ms", settings.sample_rate_hz,
		    settings.packet_duration_ms);
	shell_print(shell, "  Packets sent: %u", packets);
}
