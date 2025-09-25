# Wi-Fi Audio Tone Test Scripts

## Requirements
- Python 3.10 or newer
- Install dependencies: `pip install sounddevice numpy`
  - Optional: `pip install pyaudio` if you prefer that backend.

## Usage
1. Start the receiver (listens on UDP port 5000 by default):
   ```bash
   python tone_udp_rx.py --listen-port 5000
   ```
   Add `--device <index>` to choose an audio output (run `python -m sounddevice` to list devices).

2. In another terminal, start the tone sender and point it at the receiver:
   ```bash
   python tone_udp_tx.py --ip 127.0.0.1 --port 5000
   ```
   Adjust options like `--freq`, `--amplitude`, or `--packet-ms` as needed.

Stop either script with `Ctrl+C`.
