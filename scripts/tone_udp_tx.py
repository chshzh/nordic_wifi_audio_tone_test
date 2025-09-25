#!/usr/bin/env python3
"""Wi-Fi audio tone UDP sender prototype.

Generates a continuous PCM sine tone and transmits it over UDP with the
wifi_audio_tone_test packet format so the receiver or firmware can validate
transport quality.
"""

import argparse
import logging
import math
import socket
import struct
import sys
import time

LOGGER = logging.getLogger("tone_udp_tx")
DEFAULT_SAMPLE_RATE = 44_100
DEFAULT_PACKET_MS = 20
HEADER_FMT = ">III"  # sequence, cumulative samples, timestamp (us)
HEADER_LEN = struct.calcsize(HEADER_FMT)
INT16_MAX = 2 ** 15 - 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stream a PCM sine tone over UDP using wifi_audio_tone_test format",
    )
    parser.add_argument("--ip", required=True, help="Destination IPv4 address")
    parser.add_argument("--port", type=int, required=True, help="Destination UDP port")
    parser.add_argument("--freq", type=float, default=1000.0, help="Tone frequency in Hz")
    parser.add_argument(
        "--amplitude",
        type=float,
        default=0.8,
        help="Amplitude as fraction of full scale (0.0-1.0)",
    )
    parser.add_argument(
        "--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="PCM sample rate"
    )
    parser.add_argument(
        "--packet-ms",
        type=float,
        default=DEFAULT_PACKET_MS,
        help="Packet duration in milliseconds",
    )
    parser.add_argument("--log-level", default="INFO", help="Logging level")
    return parser


def generate_sine_packet(samples_per_packet: int, freq_hz: float, amplitude: float, sample_rate: int) -> bytes:
    """Generate one PCM packet worth of samples as signed 16-bit little-endian."""
    step = 2.0 * math.pi * freq_hz / sample_rate
    # Using list comprehension for deterministic table
    table = [int(math.sin(step * i) * amplitude) for i in range(samples_per_packet)]
    return struct.pack("<" + "h" * samples_per_packet, *table)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO), format="%(asctime)s %(levelname)s %(message)s")

    if args.sample_rate <= 0:
        LOGGER.error("Sample rate must be positive")
        return 1
    if args.packet_ms <= 0:
        LOGGER.error("Packet duration must be positive")
        return 1
    amplitude = max(0.0, min(1.0, args.amplitude)) * INT16_MAX

    samples_per_packet = int(round(args.sample_rate * (args.packet_ms / 1000.0)))
    if samples_per_packet <= 0:
        LOGGER.error("Computed samples per packet <= 0. Check sample rate / packet duration")
        return 1

    pcm_payload = generate_sine_packet(samples_per_packet, args.freq, amplitude, args.sample_rate)
    payload_len = len(pcm_payload)
    LOGGER.info(
        "Starting tone stream: dest=%s:%d freq=%.1fHz amp=%.1f sample_rate=%d packet_ms=%.1f payload=%d bytes",
        args.ip,
        args.port,
        args.freq,
        args.amplitude,
        args.sample_rate,
        args.packet_ms,
        payload_len,
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, max(payload_len * 4, 65536))
    dest = (args.ip, args.port)

    seq = 0
    sample_counter = 0
    interval_s = args.packet_ms / 1000.0
    next_send = time.perf_counter()

    try:
        while True:
            now = time.perf_counter()
            if now < next_send:
                time.sleep(next_send - now)
            timestamp_us = int(time.time_ns() // 1000)
            header = struct.pack(HEADER_FMT, seq, sample_counter, timestamp_us)
            sent = sock.sendto(header + pcm_payload, dest)
            if sent != HEADER_LEN + payload_len:
                LOGGER.warning("Short send: %d bytes", sent)
            seq = (seq + 1) & 0xFFFFFFFF
            sample_counter = (sample_counter + samples_per_packet) & 0xFFFFFFFF
            next_send += interval_s
    except KeyboardInterrupt:
        LOGGER.info("Stopping tone stream")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
