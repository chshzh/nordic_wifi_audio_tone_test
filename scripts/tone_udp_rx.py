#!/usr/bin/env python3
from __future__ import annotations
"""Wi-Fi audio tone UDP receiver and player.

Listens for UDP packets in the wifi_audio_tone_test format, buffers audio to
absorb jitter, plays back the PCM stream, and reports statistics.
"""

import argparse
import collections
import logging
import queue
import signal
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
        self.total_received = 0
        self.received_since_last_report = 0
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
        self.total_received += 1
        self.received_since_last_report += 1
        self.total_bytes += payload_len

    def report_needed(self, period_s: float) -> bool:
        return time.monotonic() - self.last_report >= period_s

    def report(self, jitter_buffer_samples: int, sample_rate: int):
        now = time.monotonic()
        elapsed = now - self.start_time
        bitrate_kbps = (self.total_bytes * 8 / elapsed) / 1000 if elapsed > 0 else 0.0
        packet_rate = self.total_received / elapsed if elapsed > 0 else 0.0
        LOGGER.info(
            "Stats: received=%d total_received=%d lost=%d bitrate=%.1f kbps packet_rate=%.1f/s buffer=%.1f ms",
            self.received_since_last_report,
            self.total_received,
            self.lost_packets,
            bitrate_kbps,
            packet_rate,
            jitter_buffer_samples / sample_rate * 1000.0,
        )
        self.received_since_last_report = 0  # Reset counter for next report period
        self.last_report = now


def discover_local_ips() -> list[str]:
    ips: set[str] = set()

    try:
        hostname = socket.gethostname()
        _, _, host_ips = socket.gethostbyname_ex(hostname)
        ips.update(host_ips)
    except (socket.gaierror, OSError):
        pass

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe_sock:
            probe_sock.connect(("8.8.8.8", 80))
            ips.add(probe_sock.getsockname()[0])
    except OSError:
        pass

    if not ips:
        return []

    sorted_ips = sorted(ips)
    non_loopback = [ip for ip in sorted_ips if not ip.startswith("127.")]
    return non_loopback or sorted_ips


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Receive and play UDP sine tone stream")
    parser.add_argument("--listen-port", type=int, default=50005, help="UDP port to bind")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="Expected sample rate")
    parser.add_argument("--channels", type=int, default=DEFAULT_CHANNELS, help="Channel count (mono=1)")
    parser.add_argument("--jitter-buffer-ms", type=float, default=60.0, help="Jitter buffer depth in ms")
    parser.add_argument("--device", type=int, help="Sound device index for playback")
    parser.add_argument("--log-level", default="INFO", help="Logging level")
    parser.add_argument("--log-file", type=Path, help="Optional log file path")
    parser.add_argument("--save-wav", type=Path, help="Optional WAV output path")
    parser.add_argument("--no-audio", action="store_true", help="Disable audio playback (stats only)")
    return parser


class PlaybackStats:
    def __init__(self):
        self._lock = threading.Lock()
        self._underflows = 0
        self._max_depth = 0

    def record_underflow(self):
        with self._lock:
            self._underflows += 1

    def observe_depth(self, depth: int):
        with self._lock:
            if depth > self._max_depth:
                self._max_depth = depth

    def snapshot(self):
        with self._lock:
            underflows = self._underflows
            max_depth = self._max_depth
            self._underflows = 0
            self._max_depth = 0
        return underflows, max_depth


def audio_thread(
    play_queue: "queue.Queue[bytes]",
    sample_rate: int,
    channels: int,
    device: int | None,
    stop_event: threading.Event,
    bytes_per_frame: int,
    playback_stats: PlaybackStats,
):
    if sd is None:
        LOGGER.error("sounddevice module not available; cannot play audio")
        return

    dtype = "int16"
    blocksize = 0  # let sounddevice decide
    pending = bytearray()

    def callback(outdata, frames, time_info, status):  # pragma: no cover - realtime callback
        needed_bytes = frames * bytes_per_frame
        playback_stats.observe_depth(play_queue.qsize())
        while len(pending) < needed_bytes:
            try:
                pending.extend(play_queue.get_nowait())
            except queue.Empty:
                missing = needed_bytes - len(pending)
                if missing > 0:
                    playback_stats.record_underflow()
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

    def handle_sigTSTP(signum, frame):  # pragma: no cover - signal handling
        raise KeyboardInterrupt

    signal.signal(signal.SIGTSTP, handle_sigTSTP)

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

    bound_ip, bound_port = sock.getsockname()
    LOGGER.info("Listening on %s:%d", bound_ip, bound_port)

    if bound_ip == "0.0.0.0":
        local_ips = discover_local_ips()
        if local_ips:
            endpoints = ", ".join(f"{ip}:{bound_port}" for ip in local_ips)
            LOGGER.info("Reachable on local interfaces: %s", endpoints)
            LOGGER.info("nRF7002DK tone start command: tone start %s %d", local_ips[0], bound_port)
        else:
            LOGGER.info("Reachable on all interfaces; local IP discovery unavailable")
            LOGGER.info("nRF7002DK tone start command: tone start <receiver_ip> %d", bound_port)
    else:
        LOGGER.info("Reachable on %s:%d", bound_ip, bound_port)
        LOGGER.info("nRF7002DK tone start command: tone start %s %d", bound_ip, bound_port)

    # Remind user about port configuration when using default
    if args.listen_port == 50005:
        LOGGER.info("Using default port 50005. To use a different port, specify --listen-port <port_number>")

    LOGGER.info("Use Ctrl+Z to exit the receiver cleanly")

    playback_queue: queue.Queue[bytes] | None
    playback_stats = None
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
        playback_queue = queue.Queue(maxsize=64)
        playback_stats = PlaybackStats()
        if target_buffer_bytes > 0 and zero_chunk:
            buffered_bytes = 0
            while buffered_bytes < target_buffer_bytes:
                chunk_len = min(len(zero_chunk), target_buffer_bytes - buffered_bytes)
                playback_queue.put(b"\x00" * chunk_len)
                buffered_bytes += chunk_len
        audio_thread_obj = threading.Thread(
            target=audio_thread,
            args=(
                playback_queue,
                args.sample_rate,
                args.channels,
                args.device,
                stop_event,
                bytes_per_frame,
                playback_stats,
            ),
            daemon=True,
        )
        audio_thread_obj.start()
    elif args.no_audio:
        LOGGER.info("Audio playback disabled (--no-audio)")
        playback_queue = None
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

            if playback_queue is not None:
                try:
                    playback_queue.put(payload, timeout=0.05)
                    buffered_bytes = min(buffered_bytes + len(payload), target_buffer_bytes)
                except queue.Full:
                    LOGGER.warning("Playback queue full; dropping audio chunk")

                if buffered_bytes >= target_buffer_bytes:
                    buffered_bytes = target_buffer_bytes

            if stats.report_needed(5.0):
                stats.report(jitter_buffer_samples, args.sample_rate)
                if playback_queue is not None and playback_stats is not None:
                    underflows, max_depth = playback_stats.snapshot()
                    depth = playback_queue.qsize()
                    if underflows:
                        LOGGER.warning(
                            "Playback queue depth=%d/%d underflows=%d max_depth=%d",
                            depth,
                            playback_queue.maxsize,
                            underflows,
                            max_depth,
                        )
                    else:
                        LOGGER.info(
                            "Playback queue depth=%d/%d max_depth=%d",
                            depth,
                            playback_queue.maxsize,
                            max_depth,
                        )
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
