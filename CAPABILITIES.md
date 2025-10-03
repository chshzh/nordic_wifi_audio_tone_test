# Wi-Fi Audio Tone Test - System Capabilities

This document describes what the Wi-Fi Audio Tone Test system can do and its key features.

## Overview

The Wi-Fi Audio Tone Test is a comprehensive system for evaluating UDP audio transport on the Nordic nRF7002DK. It provides a complete solution for generating, transmitting, receiving, and analyzing audio tone streams over Wi-Fi networks.

## Hardware Capabilities

### Supported Boards
- **nRF7002 DK** (nrf7002dk/nrf5340/cpuapp)
  - Integrated nRF7002 Wi-Fi chip (2.4 GHz and 5 GHz support)
  - Dual-core nRF5340 processor

### Physical Controls
- **BTN1 (Button 1)**: Decreases tone amplitude by 5% per press
- **BTN2 (Button 2)**: Increases tone amplitude by 5% per press
- Real-time volume control while streaming is active
- Visual feedback through console logging

## Firmware Capabilities

### Audio Generation
- **Sine wave tone generation** with configurable parameters
- **Frequency range**: Configurable (default: 1000 Hz)
- **Sample rates**: Configurable (default: 44100 Hz)
- **Amplitude control**: 0-100% in 5% steps (default: 50%)
- **Packet duration**: Configurable in milliseconds (default: 10 ms)
- Efficient waveform generation using pre-computed lookup tables

### Streaming Features
- **UDP-based audio streaming** for low-latency transmission
- **Dedicated workqueue** for precise timing and minimal jitter
- **Real-time packet scheduling** with microsecond precision
- **Sequence numbering** for packet loss detection
- **Timestamp embedding** for latency analysis
- **Automatic retry logic** for transient network failures
- **Buffer management** with 64 KB socket send buffer

### Shell Commands
The firmware provides a complete command-line interface via UART:

#### Tone Control Commands
- `tone start [<ipv4> <port>]` - Start tone streaming to specified destination
- `tone stop` - Stop active tone streaming
- `tone status` - Display current streaming status and configuration
- `tone config freq=<Hz> amp=<0-100> rate=<Hz> packet=<ms>` - Configure tone parameters

#### Wi-Fi Commands
- `wifi cred add -s <SSID> -k <key_mgmt> -p <password>` - Add Wi-Fi credentials
- `wifi cred auto_connect` - Enable automatic Wi-Fi connection
- `wifi status` - Display Wi-Fi connection status
- Full Wi-Fi shell with additional network management commands

### Configuration Options
- Adjustable tone frequency (Hz)
- Adjustable amplitude (0-100%)
- Adjustable sample rate (Hz)
- Adjustable packet duration (ms)
- Persistent amplitude setting (survives restarts)

## Python Script Capabilities

### Receiver Script (`tone_udp_rx.py`)

#### Core Features
- **UDP packet reception** on configurable port (default: 50005)
- **Jitter buffer** for smooth playback (configurable buffer size)
- **Real-time audio playback** using sounddevice library
- **WAV file recording** for offline analysis
- **Statistics reporting** with detailed metrics
- **Packet loss detection** via sequence number monitoring
- **Underflow detection** and reporting

#### Audio Processing
- Support for mono (1 channel) audio
- 16-bit signed integer PCM format
- Configurable sample rate (default: 44100 Hz)
- Automatic jitter compensation
- Queue depth monitoring

#### Operating Modes
- **Normal mode**: Full audio playback with statistics
- **No-audio mode**: Reception and analysis without playback
- **Recording mode**: Save received audio to WAV file
- **Debug mode**: Detailed logging for troubleshooting

#### Statistics and Monitoring
- Total packets received
- Packet loss rate and count
- Sequence gap detection
- Bytes received per second
- Playback queue depth
- Underflow count (audio starvation events)
- Periodic reporting at configurable intervals

#### Command-Line Options
```bash
--port <port>              # UDP listening port (default: 50005)
--sample-rate <rate>       # Sample rate in Hz (default: 44100)
--channels <n>             # Number of audio channels (default: 1)
--jitter-buffer-ms <ms>    # Jitter buffer size (default: 100 ms)
--save-wav <file>          # Save received audio to WAV file
--no-audio                 # Disable audio playback
--log-level <level>        # Set logging level (DEBUG, INFO, etc.)
--device <id>              # Select audio output device
```

### Transmitter Script (`tone_udp_tx.py`)

#### Core Features
- **Host-based tone generation** for testing without hardware
- **UDP packet transmission** with firmware-compatible format
- **Precise timing control** for consistent packet intervals
- **Sequence numbering** matching firmware format
- **Timestamp generation** for latency measurements

#### Audio Generation
- Sine wave generation with configurable parameters
- 16-bit signed integer PCM samples
- Configurable frequency (default: 1000 Hz)
- Configurable amplitude (0.0-1.0, default: 0.5)
- Configurable sample rate (default: 44100 Hz)
- Configurable packet duration (default: 10 ms)

#### Command-Line Options
```bash
--ip <address>             # Target IP address (required)
--port <port>              # Target UDP port (default: 50005)
--freq <Hz>                # Tone frequency (default: 1000)
--amplitude <0.0-1.0>      # Amplitude (default: 0.5)
--sample-rate <rate>       # Sample rate (default: 44100)
--packet-ms <ms>           # Packet duration (default: 10)
--log-level <level>        # Set logging level
```

## Use Cases

### 1. Wi-Fi Audio Quality Assessment
- Measure packet loss and jitter over Wi-Fi networks
- Evaluate different Wi-Fi channels and frequencies
- Test network performance under various conditions
- Compare 2.4 GHz vs 5 GHz performance

### 2. Network Latency Analysis
- Measure end-to-end audio latency
- Analyze timestamp data for timing accuracy
- Identify network congestion issues
- Monitor jitter and buffer stability

### 3. Firmware Development and Testing
- Test audio streaming implementations
- Validate timing and scheduling algorithms
- Debug UDP transmission issues
- Profile CPU usage and memory consumption

### 4. Audio Transport Validation
- Verify PCM audio encoding/decoding
- Test different sample rates and packet sizes
- Validate buffer management strategies
- Check for audio artifacts and underflows

### 5. Educational and Demonstration
- Demonstrate Wi-Fi audio streaming concepts
- Show real-time network statistics
- Teach audio processing and networking
- Prototype audio streaming applications

## Technical Specifications

### Packet Format
- **Header**: 12 bytes
  - Sequence number (4 bytes, big-endian)
  - Cumulative sample count (4 bytes, big-endian)
  - Timestamp in microseconds (4 bytes, big-endian)
- **Payload**: PCM audio samples (16-bit signed integers, little-endian)

### Performance Characteristics
- **Default packet rate**: 100 packets/second (10 ms packets)
- **Default bandwidth**: ~70 kbps for 44.1 kHz mono audio
- **Latency**: Depends on jitter buffer (default ~100 ms)
- **Timing precision**: Microsecond-level scheduling
- **Maximum packet size**: Configurable up to 2048 samples per packet

### Network Requirements
- **Protocol**: UDP (connectionless, low-latency)
- **Default port**: 50005
- **Network topology**: Same subnet recommended
- **Firewall**: UDP port must be open
- **Bandwidth**: ~100 kbps minimum for default configuration

## Integration Capabilities

### Extensibility
- Modular code structure for easy customization
- Configurable parameters via Kconfig and overlays
- Well-documented APIs for extension
- Python scripts can be integrated into larger systems

### Compatibility
- Standard PCM audio format (compatible with most audio tools)
- Standard UDP networking (works with any UDP-capable network)
- Cross-platform Python scripts (Windows, Linux, macOS)
- Zephyr RTOS firmware (portable to other boards)

### Data Export
- WAV file export for offline analysis
- Console logging for real-time monitoring
- Statistics export for performance analysis
- Timestamp data for latency measurements

## Limitations and Constraints

### Hardware Limitations
- Requires nRF7002 DK or compatible hardware
- Buttons are board-specific (BTN1/BTN2)
- Wi-Fi range depends on environment and configuration

### Software Limitations
- UDP protocol has no guaranteed delivery
- Packet loss possible under poor network conditions
- Jitter buffer adds latency (configurable trade-off)
- Python receiver requires sounddevice and numpy libraries

### Network Limitations
- Works best on same subnet (though not strictly required)
- Performance depends on Wi-Fi signal quality
- May experience issues with high network congestion
- Firewall rules must allow UDP traffic

## Future Enhancement Possibilities

While not currently implemented, the architecture supports potential additions:
- Support for stereo (2-channel) audio
- Additional waveform types (square, triangle, sawtooth)
- Dynamic bitrate adaptation
- Forward error correction (FEC)
- Encrypted audio transmission
- Multi-target streaming (broadcast/multicast)
- Web-based control interface
- Real-time visualization of audio waveforms
- Automatic network quality adaptation

## Summary

The Wi-Fi Audio Tone Test system provides a complete toolkit for:
- ✅ Generating audio tones on embedded hardware
- ✅ Streaming audio over Wi-Fi networks via UDP
- ✅ Receiving and playing back audio on host computers
- ✅ Analyzing network performance and audio quality
- ✅ Testing and validating audio streaming implementations
- ✅ Educational demonstrations and prototyping

It's particularly well-suited for evaluating UDP-based audio transport, testing Wi-Fi network performance, and developing real-time audio streaming applications on Nordic hardware.
