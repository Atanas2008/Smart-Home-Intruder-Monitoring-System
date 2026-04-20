# Smart Home Intruder Monitoring System

A complete, end-to-end security system built around an **Arduino Uno** that
detects and tracks intruders with ultrasonic sensors, rotates a servo toward
them, and streams live status data to a **web dashboard** via a **Python backend**.

---

## Table of Contents

1. [System Architecture](#system-architecture)
2. [Hardware](#hardware)
3. [Repository Layout](#repository-layout)
4. [Arduino Firmware](#arduino-firmware)
5. [Python Backend](#python-backend)
6. [Web Frontend](#web-frontend)
7. [Quick Start](#quick-start)
8. [API Reference](#api-reference)
9. [Camera Integration Note](#camera-integration-note)

---

## System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Physical Layer                                                  │
│                                                                  │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐                 │
│  │ HC-SR04    │  │ HC-SR04    │  │ HC-SR04    │                 │
│  │  LEFT      │  │  CENTER    │  │  RIGHT     │                 │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘                │
│        └───────────────┴───────────────┘                        │
│                          │                                       │
│                    ┌─────▼──────┐                                │
│                    │ Arduino Uno│──── Servo motor                │
│                    └─────┬──────┘                                │
│                          │ USB Serial (9600 baud)                │
└──────────────────────────┼───────────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│  Backend (Python / Flask)                                        │
│                                                                  │
│  serial thread ──▶ parse ──▶ in-memory history deque            │
│                                                                  │
│  GET /status   ──▶ latest record  (JSON)                        │
│  GET /history  ──▶ last 50 records (JSON array)                 │
│  GET /         ──▶ serve index.html                              │
└──────────────────────────┬───────────────────────────────────────┘
                           │ HTTP
┌──────────────────────────▼───────────────────────────────────────┐
│  Frontend (HTML + CSS + JS)                                      │
│                                                                  │
│  • System state card  (IDLE / DETECTED / TRACKING / ALERT)      │
│  • Intruder position + raw sensor distances                      │
│  • Servo needle visualizer                                       │
│  • Live event-log table                                          │
│  • Camera placeholder                                            │
└──────────────────────────────────────────────────────────────────┘
```

**Data flow in one sentence:**  
Arduino → USB Serial → Python parser → Flask REST API → browser polls
`/status` every 500 ms and `/history` every 2 s → DOM updated in place.

---

## Hardware

| Component | Qty | Notes |
|---|---|---|
| Arduino Uno | 1 | Any ATmega328P board works |
| HC-SR04 Ultrasonic sensor | 3 | LEFT / CENTER / RIGHT |
| Servo motor (SG90 or similar) | 1 | Signal on pin 9 |
| USB-A to USB-B cable | 1 | Arduino ↔ PC |
| Z-IOT WiFi camera | 1 | App-only; see [camera note](#camera-integration-note) |

### Wiring

```
Sensor  | TRIG | ECHO
--------|------|-----
LEFT    |  D2  |  D3
CENTER  |  D4  |  D5
RIGHT   |  D6  |  D7

Servo signal → D9
```

---

## Repository Layout

```
.
├── arduino/
│   └── intruder_system/
│       └── intruder_system.ino   # Arduino firmware
├── backend/
│   ├── server.py                 # Flask backend + serial reader
│   └── requirements.txt
├── frontend/
│   ├── index.html                # Dashboard page
│   ├── style.css                 # Dark-theme styles
│   └── app.js                    # Polling logic & DOM updates
└── README.md
```

---

## Arduino Firmware

**File:** `arduino/intruder_system/intruder_system.ino`

### Features

| Feature | Detail |
|---|---|
| Distance measurement | Sequential HC-SR04 reads, 100 µs guard delay |
| Noise filtering | 5-sample moving average per sensor |
| Intruder detection | Any sensor < 100 cm |
| Position | Closest active sensor wins (LEFT / CENTER / RIGHT) |
| Servo tracking | Moves only on position change to avoid jitter |
| State machine | IDLE → DETECTED → TRACKING → ALERT (5 s timeout) |
| Serial output | 200 ms cadence, machine-readable pipe-delimited line |

### Serial output format

```
STATE:TRACKING|POS:LEFT|LEFT:45.2|CENTER:320.0|RIGHT:350.0
```

### State machine transitions

```
IDLE  ──(object detected)──▶  DETECTED
DETECTED  ──(object moves)──▶  TRACKING
DETECTED / TRACKING  ──(> 5 s)──▶  ALERT
DETECTED / TRACKING / ALERT  ──(no object)──▶  IDLE
```

---

## Python Backend

**File:** `backend/server.py`

### Install

```bash
cd backend
pip install -r requirements.txt
```

### Run (with hardware)

```bash
python server.py --serial /dev/ttyUSB0 --port 5000
# Windows: python server.py --serial COM3 --port 5000
```

### Run (simulation mode – no hardware required)

```bash
python server.py --simulate --port 5000
```

Simulation mode generates synthetic sensor readings so you can develop
and demo the web UI without physical hardware.

### CLI flags

| Flag | Default | Description |
|---|---|---|
| `--port` | 5000 | HTTP listen port |
| `--serial` | /dev/ttyUSB0 | Arduino serial device |
| `--baud` | 9600 | Serial baud rate |
| `--simulate` | off | Bypass hardware, generate synthetic data |

### Logging

Structured log output is written to stdout at INFO level. The serial
reader logs every parsed record at DEBUG level (`export PYTHONLOGLEVEL=DEBUG`).

---

## Web Frontend

**Files:** `frontend/index.html`, `frontend/style.css`, `frontend/app.js`

Open `http://localhost:5000` after starting the backend. No build step
required – plain HTML/CSS/JS.

### Dashboard components

| Component | Description |
|---|---|
| **System Status** | State badge + colour-coded label; pulses red on ALERT |
| **Intruder Position** | LEFT / CENTER / RIGHT cells; active cell highlighted |
| **Sensor Distances** | Real-time cm readings under each position cell |
| **Servo Direction** | Animated needle showing current servo angle |
| **Event Log** | Scrollable table of the last 30 events with timestamps |
| **Camera Feed** | Placeholder with explanation (see below) |

The page polls `/status` every **500 ms** and `/history` every **2 s**
using the `fetch` API. No WebSocket dependency required.

---

## Quick Start

```bash
# 1. Flash the Arduino sketch
#    Open arduino/intruder_system/intruder_system.ino in the Arduino IDE
#    and upload to your Uno.

# 2. Install Python dependencies
cd backend
pip install -r requirements.txt

# 3a. Run with hardware
python server.py --serial /dev/ttyUSB0

# 3b. Run in simulation mode (no Arduino needed)
python server.py --simulate

# 4. Open the dashboard
#    Navigate to http://localhost:5000 in any browser.
```

---

## API Reference

### `GET /status`

Returns the most recent sensor snapshot.

```json
{
  "state":    "TRACKING",
  "position": "LEFT",
  "distances": {
    "left":   45.2,
    "center": 320.0,
    "right":  350.0
  },
  "timestamp": "2025-01-15T10:23:45.123456+00:00"
}
```

### `GET /history`

Returns a JSON array of the last 50 snapshots, newest first.

---

## Camera Integration Note

The **Z-IOT camera** does not expose a direct RTSP or HTTP stream URL —
it is accessible only through the manufacturer's mobile application.
As a result, the dashboard shows a placeholder panel instead of a live feed.

To add a real camera feed, replace the placeholder `<div>` in
`frontend/index.html` with an `<img>` or `<video>` tag pointing to an
RTSP-capable camera's stream URL, for example:

```html
<!-- MJPEG stream from an IP camera -->
<img src="http://192.168.1.100/video.cgi" alt="Live feed" />
```