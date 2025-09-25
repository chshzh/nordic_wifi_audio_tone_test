# wifi_audio_tone_test Product Specification

## Overview
- **Purpose**: Evaluate Wi-Fi transport quality using a continuous sine-tone PCM stream from `nRF7002DK` to a Python UDP listener for audible monitoring and quantitative stats.
- **Reference Baseline**: Mirror project layout and build tooling from `nordic_wifi_opus_audio_demo`; reuse tone-generation concepts from `src/audio/audio_system.c`.
- **Success Criteria**:
  - End-to-end latency ≤ 100 ms with default settings.
  - Uninterrupted playback for ≥ 10 minutes at 44.1 kHz, 16-bit mono.
  - Shell command group exposes start/stop/status with packet and Wi-Fi metrics.

## System Topology
- PC connects to router via Ethernet.
- `nRF7002DK` associates with same router in Wi-Fi STA mode; streams sine tone as UDP packets.
- Python listener on PC buffers and plays received PCM audio through local sound device while logging metrics.

## Firmware (nRF7002DK)
- **Project Base**: Fork `nrf/samples/wifi/shell` within this project.
- **Shell Interface**:
  - `tone start <freq_hz> [amplitude_pct] [sample_rate_hz] [packet_ms]`
  - `tone stop`
  - `tone status` (packets sent, underruns, Wi-Fi RSSI, retries, current settings)
  - `udp target <ip> <port>` to configure destination socket and persist via Zephyr settings subsystem.
- **Audio Generation**:
  - Fixed-point sine LUT (at least 1024 samples) feeding PCM buffer at runtime.
  - Default tone: 1 kHz, 80% amplitude, 44.1 kHz sample rate, 16-bit signed, mono.
  - Configurable packet duration (default 20 ms = 882 samples) with header preceding payload.
- **Packet Format** (big endian header):
  - `uint32_t sequence_number`
  - `uint32_t sample_counter` (cumulative samples transmitted)
  - `uint32_t timestamp_us` (monotonic uptime in microseconds)
- **Networking & Diagnostics**:
  - Non-blocking UDP socket with send pacing based on packet duration.
  - Track Wi-Fi RSSI, TX retries, socket errors; expose stats via shell and logs.
  - Optional Kconfig to disable Wi-Fi power-save when streaming.
- **Build Variants**:
  - `overlay-debug.conf` enabling verbose logging, shell history.
  - Future `overlay-lowrate.conf` for reduced sample rates.

## Python Tooling
- **Receiver (`tone_udp_rx.py`)**:
  - CLI options: `--listen-port`, `--device`, `--jitter-buffer-ms`, `--log-file`, `--save-wav`, `--sample-rate`, `--channels`.
  - Jitter buffer accumulates configurable milliseconds of audio before playback; detects sequence gaps and fills with zeros.
  - Playback path uses `sounddevice` (default) with fallback to `pyaudio` if requested.
  - Periodic stats (every 5 s): packets received, lost, average inter-arrival, latency estimate, buffer depth.
  - Optional WAV capture for offline analysis.
- **Prototype Sender (`tone_udp_tx.py`)**:
  - Mirrors firmware packet format to validate receiver and playback pipeline.
  - CLI options align with firmware defaults (`--ip`, `--port`, `--freq`, `--amplitude`, `--packet-ms`).
  - Timing loop maintains packet cadence and logs drift.

## Logging & Metrics
- Firmware `tone status` shows totals, underruns, send interval average, Wi-Fi signal metrics.
- Receiver logs to console and optional file with structured summaries; align timestamps with device logs.
- Document methods to export raw stats for analysis.

## Configurability & Files
- `prj.conf` baseline + overlays for alternate sample rates or Wi-Fi settings.
- `boards/` directory for board-specific configs if needed.
- `scripts/` hosts Python tools and helper utilities.
- `docs/` to include README, ASCII topology diagrams, troubleshooting, and this spec (symlink or reference).

## Testing Strategy
1. **Python Loopback**: Use `tone_udp_tx.py` → `tone_udp_rx.py` locally/LAN to validate pipeline and logging.
2. **Firmware Bring-Up**: Flash device, confirm Wi-Fi connection, verify shell commands without streaming.
3. **End-to-End Audio**: Stream from device to receiver, record stats, capture audio snippet.
4. **Stress**: ≥30 min run, vary router distance/interference, note jitter buffer performance.
5. **Regression**: Automated script to verify basic CLI commands and packet integrity post-changes.

## Risks & Mitigations
- **Wi-Fi Jitter**: Provide adjustable jitter buffer, monitor latency, optionally adapt packet size.
- **Clock Drift**: Receiver adjusts playback rate or inserts/removes samples based on buffer depth.
- **Audio Device Issues**: Allow selecting output device, document driver setup, provide file logging fallback.
- **Router QoS Variance**: Recommend disabling power-save, selecting least congested channel, using dedicated SSID when possible.

## Documentation Deliverables
- `README.md` with setup, build, flash, and usage instructions.
- ASCII diagram of data flow.
- Troubleshooting section covering firewall, dependency installation, typical Wi-Fi pitfalls.
- Version history and change log referencing this specification for ongoing alignment.
