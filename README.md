# Wi-Fi Audio Tone Test

Evaluate UDP audio transport on the Nordic nRF7002DK with a turnkey tone generator, latency-friendly scheduler, and desktop tools for capture and analysis.

## Hardware & Firmware
- **Target board:** nRF7002 DK (nrf7002dk/nrf5340/cpuapp)
- **Wi-Fi device:** integrated nRF7002 (2.4/5 GHz)
- **Buttons:** BTN1 decreases tone amplitude, BTN2 increases in 5% steps
- **Default amplitude:** 50%; stored in firmware and adjustable at runtime
- **Firmware overlays:** `overlay-tone.conf` configures dedicated streaming workqueue and UDP tone features

## What’s Included
- **Firmware** (`shell_with_tone`): dedicated workqueue pacing, 0–100% amplitude control, and BTN1/BTN2 volume adjustment in 5% steps (default 50%).
- **Receiver** (`scripts/tone_udp_rx.py`): jitter-buffered playback, underflow telemetry, optional WAV capture.
- **Transmitter** (`scripts/tone_udp_tx.py`): host-based tone source that mirrors the firmware packet format.

## Build & Flash (nRF7002DK)
```bash
cd shell_with_tone
west build -p -b nrf7002dk/nrf5340/cpuapp -- -DEXTRA_CONF=overlay-tone.conf
west flash
```

## Configure Wi-Fi
```bash
uart:~$ wifi cred add -s YOUR_SSID -k 1 -p YOUR_PASSWORD
uart:~$ wifi cred auto_connect
uart:~$ wifi status
```

## Start Streaming
```bash
uart:~$ tone start 192.168.1.100 50005
```
> BTN1 lowers volume, BTN2 raises it; log output prints the new amplitude.

Key commands:
- `tone stop`
- `tone status`
- `tone config freq=<Hz> amp=<0-100> rate=<Hz> packet=<ms>`

## Host Receiver
```bash
pip install sounddevice numpy
cd scripts
python tone_udp_rx.py            # defaults to port 50005
python tone_udp_rx.py --save-wav out.wav --log-level DEBUG
python tone_udp_rx.py --no-audio --jitter-buffer-ms 150
```
The script reports `Playback queue depth` and `underflows`; non-zero underflows indicate host starvation.

## Host Transmitter (Optional)
```bash
python tone_udp_tx.py --ip 192.168.1.100
```

## Releases
Firmware artifacts are published via GitHub Actions. On each merge to `main` (and tags starting with `v`) the workflow in `.github/workflows/build.yml` builds the `overlay-tone.conf` image and uploads `merged.hex` as an artifact. To download:
- Open the repository’s **Actions** tab on GitHub
- Select the latest successful **Build and Test Wi-Fi Tone Test** run
- Download the `build-Wi-Fi Tone Shell (tone overlay)` artifact to get the compiled firmware

## Network Notes
- Keep devices on the same subnet; UDP port 50005 must be open.
- Disabling Wi-Fi power save on the DUT helps maintain consistent spacing for the tone scheduler.
