/*
 * Wi-Fi Audio Tone Test - shell command bindings
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>

#include "tone_stream.h"

LOG_MODULE_REGISTER(tone_shell, CONFIG_LOG_DEFAULT_LEVEL);

static int cmd_tone_start(const struct shell *shell, size_t argc, char **argv)
{
	int ret = 0;

	/* If IP and port are provided, set the target first */
	if (argc == 3) {
		char *end;
		long port = strtol(argv[2], &end, 10);
		if (*end != '\0' || port <= 0 || port > UINT16_MAX) {
			shell_error(shell, "Invalid port: %s", argv[2]);
			return -EINVAL;
		}

		ret = tone_stream_set_target(argv[1], (uint16_t)port);
		if (ret) {
			shell_error(shell, "Invalid IPv4 address or port");
			return ret;
		}
		shell_print(shell, "Tone target set to %s:%ld", argv[1], port);
	} else if (argc != 1) {
		shell_error(shell, "Usage: tone start [<ipv4> <port>]");
		return -EINVAL;
	}

	ret = tone_stream_start(shell);
	if (ret == -EALREADY) {
		shell_warn(shell, "Tone already streaming");
	} else if (ret == -ENOTCONN) {
		shell_error(shell, "Destination not set. Use 'tone start <ip> <port>'");
	} else if (ret == -ERANGE) {
		shell_error(shell, "Packet configuration invalid. Adjust tone config");
	} else if (ret) {
		shell_error(shell, "Failed to start tone: %d", ret);
	}

	return ret;
}

static int cmd_tone_stop(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	tone_stream_stop(shell);
	return 0;
}

static int cmd_tone_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	tone_stream_status(shell);
	return 0;
}

static int cmd_tone_config(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(shell, "Params: freq=<Hz> amp=<0-100> rate=<Hz> packet=<ms>");
		return 0;
	}

	uint16_t freq = TONE_DEFAULT_FREQUENCY_HZ;
	uint8_t amp = TONE_DEFAULT_AMPLITUDE_PCT;
	uint32_t rate = TONE_DEFAULT_SAMPLE_RATE_HZ;
	uint16_t packet = TONE_DEFAULT_PACKET_DURATION_MS;

	for (size_t i = 1; i < argc; i++) {
		char *pair = argv[i];
		char *eq = strchr(pair, '=');
		if (!eq) {
			shell_error(shell, "Invalid param: %s", pair);
			return -EINVAL;
		}

		*eq = '\0';
		const char *key = pair;
		const char *value = eq + 1;

		long parsed = strtol(value, NULL, 10);
		if (strcmp(key, "freq") == 0) {
			if (parsed <= 0 || parsed > 20000) {
				shell_error(shell, "Frequency out of range");
				return -EINVAL;
			}
			freq = (uint16_t)parsed;
		} else if (strcmp(key, "amp") == 0) {
			if (parsed < 0 || parsed > 100) {
				shell_error(shell, "Amplitude 0-100");
				return -EINVAL;
			}
			amp = (uint8_t)parsed;
		} else if (strcmp(key, "rate") == 0) {
			if (parsed <= 0 || parsed > 192000) {
				shell_error(shell, "Sample rate out of range");
				return -EINVAL;
			}
			rate = (uint32_t)parsed;
		} else if (strcmp(key, "packet") == 0) {
			if (parsed <= 0 || parsed > 1000) {
				shell_error(shell, "Packet duration out of range");
				return -EINVAL;
			}
			packet = (uint16_t)parsed;
		} else {
			shell_error(shell, "Unknown key: %s", key);
			return -EINVAL;
		}
	}

	int ret = tone_stream_set_params(freq, amp, rate, packet);
	if (ret) {
		shell_error(shell, "Failed to apply params: %d", ret);
	} else {
		shell_print(shell, "Tone params set: %u Hz, %u%%, %u Hz sample, %u ms packet", freq,
			    amp, rate, packet);
	}

	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	tone_cmds, SHELL_CMD(start, NULL, "Start tone streaming [<ipv4> <port>]", cmd_tone_start),
	SHELL_CMD(stop, NULL, "Stop tone streaming", cmd_tone_stop),
	SHELL_CMD(status, NULL, "Display tone status", cmd_tone_status),
	SHELL_CMD(config, NULL, "Configure tone parameters", cmd_tone_config),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tone, &tone_cmds, "Tone streaming control", NULL);

static int tone_shell_init(void)
{
	return tone_stream_init();
}

SYS_INIT(tone_shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
