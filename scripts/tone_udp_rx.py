#!/usr/bin/env python3
"""Wi-Fi audio tone UDP receiver and player.

Listens for UDP packets in the wifi_audio_tone_test format, buffers audio to
absorb jitter, plays back the PCM stream, and reports statistics.
"""

import argparse
import collections
import logging
import queue
import socket
import struct
import sys
import threading
import time
from pathlib import Path

try:
    import sounddevice as sd
except ImportError:  # pragma: no cover - fallback handled at runtime
    sd = None

try:
    import numpy as np
except ImportError:
    np = None

try:
    import wave
except ImportError:  # standard library; safety net
    wave = None

LOGGER = logging.getLogger("tone_udp_rx")
HEADER_FMT = ">III"
HEADER_LEN = struct.calcsize(HEADER_FMT)
DEFAULT_SAMPLE_RATE = 44_100
DEFAULT_CHANNELS = 1
INT16_BYTES = 2


class Stats:
    def __init__(self):
        self.start_time = time.monotonic()
        self.last_report = self.start_time
        self.received_packets = 0
        self.lost_packets = 0
        self.last_seq = None
        self.total_bytes = 0
        self.intervals = collections.deque(maxlen=1000)

    def update(self, seq: int, payload_len: int):
        now = time.monotonic()
        if self.last_seq is not None:
            expected = (self.last_seq + 1) & 0xFFFFFFFF
            if seq != expected:
                gap = (seq - expected) & 0xFFFFFFFF
                self.lost_packets += gap
                LOGGER.warning("Sequence gap: expected %d got %d (lost %d)", expected, seq, gap)
            self.intervals.append(now)
        self.last_seq = seq
        self.received_packets += 1
        self.total_bytes += payload_len

    def report_needed(self, period_s: float) -> bool:
        return time.monotonic() - self.last_report >= period_s

    def report(self, jitter_buffer_samples: int, sample_rate: int):
        now = time.monotonic()
        elapsed = now - self.start_time
        bitrate_kbps = (self.total_bytes * 8 / elapsed) / 1000 if elapsed > 0 else 0.0
        packet_rate = self.received_packets / elapsed if elapsed > 0 else 0.0
        LOGGER.info(
            "Stats: received=%d lost=%d bitrate=%.1f kbps packet_rate=%.1f/s buffer=%.1f ms",
            self.received_packets,
            self.lost_packets,
            bitrate_kbps,
            packet_rate,
            jitter_buffer_samples / sample_rate * 1000.0,
        )
        self.last_report = now


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Receive and play UDP sine tone stream")
    parser.add_argument("--listen-port", type=int, default=5000, help="UDP port to bind")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="Expected sample rate")
    parser.add_argument("--channels", type=int, default=DEFAULT_CHANNELS, help="Channel count (mono=1)")
    parser.add_argument("--jitter-buffer-ms", type=float, default=60.0, help="Jitter buffer depth in ms")
    parser.add_argument("--device", type=int, help="Sound device index for playback")
    parser.add_argument("--log-level", default="INFO", help="Logging level")
    parser.add_argument("--log-file", type=Path, help="Optional log file path")
    parser.add_argument("--save-wav", type=Path, help="Optional WAV output path")
    parser.add_argument("--no-audio", action="store_true", help="Disable audio playback (stats only)")
    return parser


def audio_thread(
    play_queue: "queue.Queue[bytes]",
    sample_rate: int,
    channels: int,
    device: int | None,
    stop_event: threading.Event,
    bytes_per_frame: int,
):
    if sd is None:
        LOGGER.error("sounddevice module not available; cannot play audio")
        return

    dtype = "int16"
    blocksize = 0  # let sounddevice decide
    pending = bytearray()

    def callback(outdata, frames, time_info, status):  # pragma: no cover - realtime callback
        needed_bytes = frames * bytes_per_frame
        while len(pending) < needed_bytes:
            try:
                pending.extend(play_queue.get_nowait())
            except queue.Empty:
                missing = needed_bytes - len(pending)
                if missing > 0:
                    pending.extend(b"\x00" * missing)
                break
        outdata[:] = pending[:needed_bytes]
        del pending[:needed_bytes]

    with sd.RawOutputStream(
        samplerate=sample_rate,
        blocksize=blocksize,
        device=device,
        channels=channels,
        dtype=dtype,
        callback=callback,
    ):
        LOGGER.info("Audio playback started")
        while not stop_event.is_set():
            time.sleep(0.1)
    LOGGER.info("Audio playback stopped")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    handlers = [logging.StreamHandler()]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file))
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=handlers,
    )

    if args.sample_rate <= 0 or args.channels <= 0:
        LOGGER.error("Sample rate and channels must be positive")
        return 1

    jitter_buffer_samples = int(args.sample_rate * (args.jitter_buffer_ms / 1000.0))
    if jitter_buffer_samples <= 0:
        LOGGER.error("Jitter buffer must result in at least one sample")
        return 1

    bytes_per_frame = INT16_BYTES * args.channels
    target_buffer_bytes = jitter_buffer_samples * bytes_per_frame
    zero_chunk = b"\x00" * (bytes_per_frame * 256) if bytes_per_frame > 0 else b""

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    sock.bind(("0.0.0.0", args.listen_port))
    sock.settimeout(1.0)
    LOGGER.info("Listening on UDP port %d", args.listen_port)

    playback_queue: queue.Queue[bytes] = queue.Queue(maxsize=64)
    stop_event = threading.Event()
    audio_thread_obj = None

    wav_writer = None
    if args.save_wav:
        if wave is None:
            LOGGER.error("wave module not available; cannot save WAV")
            return 1
        wav_writer = wave.open(str(args.save_wav), "wb")
        wav_writer.setnchannels(args.channels)
        wav_writer.setsampwidth(INT16_BYTES)
        wav_writer.setframerate(args.sample_rate)

    if not args.no_audio and sd is not None:
        if target_buffer_bytes > 0 and zero_chunk:
            buffered_bytes = 0
            while buffered_bytes < target_buffer_bytes:
                chunk_len = min(len(zero_chunk), target_buffer_bytes - buffered_bytes)
                playback_queue.put(b"\x00" * chunk_len)
                buffered_bytes += chunk_len
        audio_thread_obj = threading.Thread(
            target=audio_thread,
            args=(playback_queue, args.sample_rate, args.channels, args.device, stop_event, bytes_per_frame),
            daemon=True,
        )
        audio_thread_obj.start()
    elif args.no_audio:
        LOGGER.info("Audio playback disabled (--no-audio)")
    else:
        LOGGER.warning("sounddevice not installed; running without audio output")

    stats = Stats()
    bytes_per_sample = INT16_BYTES * args.channels
    buffered_bytes = target_buffer_bytes

    try:
        while True:
            try:
                packet, addr = sock.recvfrom(HEADER_LEN + bytes_per_sample * 2048)
            except socket.timeout:
                if stats.report_needed(5.0):
                    stats.report(jitter_buffer_samples, args.sample_rate)
                continue

            if len(packet) <= HEADER_LEN:
                LOGGER.warning("Received undersized packet (%d bytes) from %s", len(packet), addr)
                continue

            header = packet[:HEADER_LEN]
            payload = packet[HEADER_LEN:]
            seq, sample_counter, timestamp_us = struct.unpack(HEADER_FMT, header)
            stats.update(seq, len(payload))

            if wav_writer is not None:
                wav_writer.writeframes(payload)

            try:
                playback_queue.put(payload, timeout=0.05)
                buffered_bytes = min(buffered_bytes + len(payload), target_buffer_bytes)
            except queue.Full:
                LOGGER.warning("Playback queue full; dropping audio chunk")

            if buffered_bytes >= target_buffer_bytes:
                buffered_bytes = target_buffer_bytes

            if stats.report_needed(5.0):
                stats.report(jitter_buffer_samples, args.sample_rate)
    except KeyboardInterrupt:
        LOGGER.info("Stopping receiver")
    finally:
        stop_event.set()
        if audio_thread_obj:
            audio_thread_obj.join(timeout=2.0)
        if wav_writer is not None:
            wav_writer.close()
        sock.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
