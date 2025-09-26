/*
 * Wi-Fi Audio Tone Test - tone streaming implementation
 */

#include "tone_stream.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <tone.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(tone_stream, CONFIG_LOG_DEFAULT_LEVEL);

#define LUT_POINTS 1024U

K_THREAD_STACK_DEFINE(tone_stream_work_stack, CONFIG_TONE_STREAM_WORKQUEUE_STACK_SIZE);
static struct k_work_q tone_stream_work_q;
static bool tone_stream_work_q_started;

struct tone_packet_header {
	uint32_t seq;
	uint32_t sample_count;
	uint32_t timestamp_us;
} __packed;

/* Buffer for one period of tone (max for 100Hz at 48kHz = 480 samples) */
#define TONE_PERIOD_MAX_SAMPLES 480

struct tone_stream_context {
	struct tone_stream_settings settings;
	int sock_fd;
	bool streaming;
	uint32_t seq_num;
	uint32_t sample_counter;
	uint16_t samples_per_packet;
	uint32_t interval_us;
	uint64_t next_deadline_us;
	int16_t tone_period[TONE_PERIOD_MAX_SAMPLES];
	size_t tone_period_samples;
	uint32_t tone_position;
	struct k_work_delayable work;
	struct k_mutex lock;
} static ctx;

static uint8_t tx_buffer[sizeof(struct tone_packet_header) + TONE_MAX_PAYLOAD_BYTES];

static inline uint64_t micros_now(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static int generate_tone_period(void)
{
	size_t tone_size_bytes;
	float amplitude = ctx.settings.amplitude_pct / 100.0f;

	int ret = tone_gen(ctx.tone_period, &tone_size_bytes, ctx.settings.frequency_hz,
			   ctx.settings.sample_rate_hz, amplitude);
	if (ret != 0) {
		LOG_ERR("tone_gen failed: %d", ret);
		return ret;
	}

	ctx.tone_period_samples = tone_size_bytes / sizeof(int16_t);
	ctx.tone_position = 0;

	LOG_INF("Generated tone period: %u samples for %u Hz", ctx.tone_period_samples,
		ctx.settings.frequency_hz);

	return 0;
}

static void fill_pcm_samples(int16_t *pcm, uint32_t samples)
{
	if (ctx.tone_period_samples == 0) {
		/* No tone generated yet, fill with silence */
		memset(pcm, 0, samples * sizeof(int16_t));
		return;
	}

	for (uint32_t i = 0; i < samples; i++) {
		pcm[i] = ctx.tone_period[ctx.tone_position];
		ctx.tone_position++;
		if (ctx.tone_position >= ctx.tone_period_samples) {
			ctx.tone_position = 0; /* Loop back to start of period */
		}
	}
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

static void reschedule_next_packet(void)
{
	const uint32_t interval_us = ctx.interval_us;
	if (interval_us == 0U) {
		k_work_reschedule_for_queue(&tone_stream_work_q, &ctx.work, K_NO_WAIT);
		return;
	}

	uint64_t now = micros_now();
	if (ctx.next_deadline_us == 0U) {
		ctx.next_deadline_us = now + interval_us;
	} else {
		ctx.next_deadline_us += interval_us;
	}

	uint64_t delay_us = (ctx.next_deadline_us > now) ? (ctx.next_deadline_us - now) : interval_us;
	uint32_t delay_clamped = (uint32_t)CLAMP(delay_us, 1U, (uint64_t)UINT32_MAX);

	k_work_reschedule_for_queue(&tone_stream_work_q, &ctx.work,
				  K_USEC(delay_clamped));
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

	uint32_t seq = ctx.seq_num++;
	uint32_t sample_count = ctx.sample_counter;
	uint32_t timestamp = (uint32_t)(micros_now() & 0xFFFFFFFFU);

	struct tone_packet_header header = {
		.seq = sys_cpu_to_be32(seq),
		.sample_count = sys_cpu_to_be32(sample_count),
		.timestamp_us = sys_cpu_to_be32(timestamp),
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

	reschedule_next_packet();
	k_mutex_unlock(&ctx.lock);
}

static void stop_locked(void)
{
	if (ctx.streaming) {
		ctx.streaming = false;
		ctx.next_deadline_us = 0U;
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
	ctx.tone_period_samples = 0;

	if (!tone_stream_work_q_started) {
		k_work_queue_init(&tone_stream_work_q);
		k_work_queue_start(&tone_stream_work_q, tone_stream_work_stack,
		      K_THREAD_STACK_SIZEOF(tone_stream_work_stack),
		      K_PRIO_PREEMPT(CONFIG_TONE_STREAM_WORKQUEUE_PRIORITY), NULL);
		if (IS_ENABLED(CONFIG_THREAD_NAME)) {
			k_thread_name_set(&tone_stream_work_q.thread, "tone_stream");
		}
		tone_stream_work_q_started = true;
	}

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
	ctx.interval_us = (uint32_t)(((uint64_t)samples * 1000000U) /
				 ctx.settings.sample_rate_hz);

	/* Generate new tone period with updated parameters */
	int ret = generate_tone_period();
	if (ret != 0) {
		k_mutex_unlock(&ctx.lock);
		return ret;
	}

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

	if (ctx.samples_per_packet == 0U) {
		ctx.samples_per_packet = DIV_ROUND_CLOSEST(
			ctx.settings.sample_rate_hz * ctx.settings.packet_duration_ms, 1000U);
		if (ctx.samples_per_packet == 0U ||
		    ctx.samples_per_packet > TONE_MAX_SAMPLES_PER_PACKET) {
			k_mutex_unlock(&ctx.lock);
			return -ERANGE;
		}
	}

	if (ctx.interval_us == 0U) {
		ctx.interval_us = (uint32_t)(((uint64_t)ctx.samples_per_packet * 1000000U) /
				  ctx.settings.sample_rate_hz);
	}

	int ret = configure_destination_socket();
	if (ret < 0) {
		k_mutex_unlock(&ctx.lock);
		return ret;
	}

	ctx.seq_num = 0;
	ctx.sample_counter = 0;
	ctx.tone_position = 0;
	ctx.next_deadline_us = 0U;

	/* Generate tone period if not already done */
	if (ctx.tone_period_samples == 0) {
		ret = generate_tone_period();
		if (ret != 0) {
			k_mutex_unlock(&ctx.lock);
			return ret;
		}
	}

	ctx.streaming = true;
	ctx.next_deadline_us = micros_now();

	k_mutex_unlock(&ctx.lock);

	k_work_schedule_for_queue(&tone_stream_work_q, &ctx.work, K_NO_WAIT);

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
