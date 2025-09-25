# Wi-Fi Audio Tone Test

A comprehensive Wi-Fi audio streaming test system using Nordic nRF7002DK to evaluate Wi-Fi transport quality through continuous sine-tone PCM streaming.

## Overview

This project provides:
- **nRF7002DK firmware** that generates and streams sine tone audio over UDP
- **Python receiver/transmitter tools** for testing and validation
- **Real-time audio playback** with jitter buffering and statistics
- **Configurable parameters** for tone frequency, packet size, and network settings

## Quick Start

### 1. Firmware Setup (nRF7002DK)

Build and flash the firmware:
```bash
cd shell_with_tone
west build -p -b nrf7002dk/nrf5340/cpuapp -- -DEXTRA_CONF=overlay-tone.conf
west flash
```

### Wi-Fi Setup
Connect to the device serial terminal and configure Wi-Fi credentials:
```bash
# Add Wi-Fi network credentials (WPA2/WPA3)
uart:~$ wifi cred add -s YOUR_SSID -k 1 -p YOUR_PASSWORD

# Enable auto-connect on boot
uart:~$ wifi cred auto_connect

# Check connection status
uart:~$ wifi status
```

### Start Audio Streaming
```bash
# Start streaming directly to receiver
uart:~$ tone start 192.168.1.100 50005
```

### 2. Python Receiver Setup

Install dependencies:
```bash
pip install sounddevice numpy
```

Start the receiver:
```bash
cd scripts
python tone_udp_rx.py
```

The receiver will listen on port 50005 by default and play received audio through your default sound device.

### 3. Testing with Python Transmitter

For testing without hardware:
```bash
python tone_udp_tx.py --ip 127.0.0.1
```

## Firmware Commands

### Basic Commands
- `tone start [<ip> <port>]` - Start streaming (optionally set target)
- `tone stop` - Stop streaming
- `tone status` - Show streaming status and statistics

### Configuration
- `tone config freq=<Hz>` - Set tone frequency (default: 1000 Hz)
- `tone config amp=<0-100>` - Set amplitude percentage (default: 80%)
- `tone config rate=<Hz>` - Set sample rate (default: 44100 Hz)
- `tone config packet=<ms>` - Set packet duration (default: 10 ms)

### Example Usage
```bash
# Add Wi-Fi credentials
uart:~$ wifi cred add -s MY_NETWORK -k 1 -p MyPassword123
uart:~$ wifi cred auto_connect

# Configure tone parameters
uart:~$ tone config packet=10 freq=500

# Start streaming
uart:~$ tone start 192.168.1.100 50005

# Check status
uart:~$ tone status
```

## Python Tools

### Receiver (`tone_udp_rx.py`)
Receives UDP audio stream and plays it back with statistics.

**Options:**
- `--listen-port <port>` - UDP port to bind (default: 50005)
- `--device <index>` - Sound device for playback
- `--jitter-buffer-ms <ms>` - Jitter buffer size (default: 60ms)
- `--save-wav <file>` - Save audio to WAV file
- `--no-audio` - Statistics only mode

**Example:**
```bash
# Basic usage
python tone_udp_rx.py

# Custom port and save to file
python tone_udp_rx.py --listen-port 5005 --save-wav output.wav

# List available audio devices
python -m sounddevice
```

### Transmitter (`tone_udp_tx.py`)
Generates and transmits sine tone for testing.

**Options:**
- `--ip <address>` - Destination IP (required)
- `--port <port>` - Destination port (default: 50005)
- `--freq <Hz>` - Tone frequency (default: 1000 Hz)
- `--amplitude <0.0-1.0>` - Amplitude (default: 0.8)
- `--packet-ms <ms>` - Packet duration (default: 10 ms)

**Example:**
```bash
# Basic usage
python tone_udp_tx.py --ip 192.168.1.100

# Custom settings
python tone_udp_tx.py --ip 192.168.1.100 --freq 500 --amplitude 0.5
```

## Statistics Output

The receiver displays real-time statistics:
```
Stats: received=500 total_received=15038 lost=0 bitrate=706.0 kbps packet_rate=100.0/s buffer=60.0 ms
```

- `received` - Packets received in last 5 seconds
- `total_received` - Total packets received since start
- `lost` - Number of lost packets detected
- `bitrate` - Current bitrate in kbps
- `packet_rate` - Packets per second
- `buffer` - Jitter buffer depth in milliseconds

### Network Requirements
- Both devices must be on the same network
- Default port 50005 should be open
- Recommended: disable Wi-Fi power saving for consistent streaming
