<p align="center">
  <img src="data/hawk370_transparent.webp" alt="HAWK 370 Logo" width="180"><br>
</p>

<h1 align="center">HAWK 370 · Wireless ThrottleBlaster</h1>
<p align="center">
  A Wi‑Fi controlled Socket 370 CPU throttle for retro PC builders, running on an ESP32‑C3 SuperMini.
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-ESP32--C3-blue">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="status" src="https://img.shields.io/badge/status-pre--bring--up-orange">
</p>

---

## What is this?

**HAWK 370** turns an ESP32‑C3 SuperMini into a wireless throttle controller for Socket 370 CPUs (Intel Mendocino / Coppermine / Tualatin, and VIA Samuel / Ezra / Nehemiah). It drives the CPU's `STPCLK#` pin with a hardware‑timed pulse‑density signal generated on the ESP32's RMT peripheral, and exposes a full web dashboard for control and monitoring - no companion app, no cloud, everything served straight from the module over your local network.

> ⚠️ **Pre‑bring‑up firmware.** Carrier frequency and minimum‑pulse‑width values are placeholders until bench calibration is done per CPU family - see the in‑app calibration warning and the comments in `ESP32_C3_SuperMini.ino`.

## Features

- **Two Wi‑Fi modes, one setup wizard** - join your home/lab network (STA), or run entirely standalone as its own access point (`HAWK370-Setup`) for bench use with no router required.
- **Live throttle control** - drag the slider or click the MHz readout to type an exact value. CPU family presets carry their own carrier frequency and minimum run‑length, with a visible warning banner for families that haven't been bench‑calibrated yet.
- **Real‑time voltage & temperature telemetry** - VCORE and VTT rails streamed over Server‑Sent Events with live sparkline graphs, extensible to temperature monitoring.
- **OTA firmware & file manager** - drag‑and‑drop uploads to LittleFS storage; `.bin` files are detected automatically and flashed as firmware (device reboots after a successful flash), everything else is written to storage. Includes rename, delete, download, and a live storage‑usage bar.
- **Wi‑Fi power‑saving toggle** - switch between low‑latency (`Max`) and power‑saving (`Eco`) modem modes on the fly.

## Screenshots

| Throttle Control | Files / OTA | Setup |
|---|---|---|
| ![Throttle control screen](docs/screenshots/throttle-control.png) | ![Files and OTA screen](docs/screenshots/files.png) | ![Wi-Fi setup wizard](docs/screenshots/setup.png) |

## Hardware

Built for an **ESP32‑C3 SuperMini** driving the HAWK 370 board.

| Signal | Pin | Notes |
|---|---|---|
| `STPCLK#` drive | GPIO 7 | Via onboard 2N7002, RMT TX output |
| VCORE sense | GPIO 0 (ADC1_CH1) | 10k/10k divider + 100nF |
| VTT sense | GPIO 1 (ADC1_CH0) | 10k/10k divider + 100nF |

The STPCLK# waveform is generated once and looped entirely in RMT hardware, so Wi‑Fi/BLE activity can't introduce jitter into the timing.

## Getting started

1. Flash `ESP32_C3_SuperMini.ino` with the Arduino IDE (ESP32 core 3.x / IDF5) or PlatformIO. Required libraries:
   - `ESPAsyncWebServer`, `AsyncTCP`
   - `ArduinoJson`
   - `ESPmDNS`, `DNSServer`
   - `LittleFS`, `Preferences`
2. Upload `index.html`, `files.html`, `setup.html`, `hawk370.js`, `hawk370.css`, the logo, and the two self‑hosted fonts to LittleFS.
3. Power up the board. On first boot it starts its own hotspot, **`HAWK370-Setup`** (password `hawk370setup`) - connect to it and open `http://hawk370.local` (or `192.168.8.1`) to run the setup wizard.
4. Choose to join your Wi‑Fi network, or stay in standalone Access Point mode for bench use.
5. Once connected, reach the dashboard at `http://hawk370.local` from any device on that network.

## Web UI overview

| Page | Purpose |
|---|---|
| `setup.html` | First‑boot wizard: choose AP vs. STA, scan and join a network |
| `index.html` | Main dashboard: CPU family/base clock config, throttle slider, VCORE/VTT graphs, device controls |
| `files.html` | Drag‑and‑drop uploader for firmware OTA and general file storage on LittleFS |

## Selected API endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/api/throttle` | GET | Current family, base clock, speed %, target MHz |
| `/api/throttle/config` | POST | Set CPU family + base clock (MHz) |
| `/api/throttle/speed` | POST | Set throttle speed (0–100%) |
| `/api/telemetry` | GET | One‑shot voltage/temp/RSSI/memory snapshot |
| `/events` | SSE | Live `telemetry` push every ~500ms |
| `/api/system` | GET | Firmware version, IP, SSID, storage usage |
| `/api/system/wifi-ps` | POST | Toggle Wi‑Fi power saving |
| `/api/files/all` | GET | List files on LittleFS |
| `/api/files/rename` / `/api/files/delete` | POST | Manage stored files |
| `/ota/firmware` | POST | Upload and flash new firmware |
| `/ota/file` | POST | Upload a file to storage |
| `/api/restart` / `/api/factory-reset` | POST | Restart device / erase Wi‑Fi credentials |

## Acknowledgements

Web server, Wi‑Fi provisioning, mDNS, and OTA subsystems are adapted from my earlier [**AirMeter**](https://github.com/BitsUndBolts/airmeter) project.

## License

MIT © 2026
