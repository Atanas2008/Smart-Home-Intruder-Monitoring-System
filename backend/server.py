"""
Intruder Detection and Tracking System – Python Backend
========================================================
Reads structured serial data from the Arduino, parses it, and exposes a
REST API consumed by the web frontend.

Serial line format produced by the Arduino:
    STATE:<state>|POS:<position>|LEFT:<cm>|CENTER:<cm>|RIGHT:<cm>

API endpoints
-------------
GET /status  → JSON snapshot of the latest reading
GET /history → JSON list of the last N readings (newest first)
GET /        → serves frontend/index.html
GET /<path>  → serves any other file under frontend/

Usage
-----
    pip install -r requirements.txt
    python server.py [--port PORT] [--serial PORT] [--baud BAUD] [--simulate]
"""

from __future__ import annotations

import argparse
import json
import logging
import random
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, send_from_directory

# ── Logging ───────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)

# ── Paths ─────────────────────────────────────────────────────────────────────
BASE_DIR = Path(__file__).resolve().parent
FRONTEND_DIR = BASE_DIR.parent / "frontend"

# ── History buffer ────────────────────────────────────────────────────────────
HISTORY_MAXLEN = 100
_history: deque[dict[str, Any]] = deque(maxlen=HISTORY_MAXLEN)
_lock = threading.Lock()

# ── Default / sentinel state ─────────────────────────────────────────────────
_EMPTY_STATUS: dict[str, Any] = {
    "state": "IDLE",
    "position": "NONE",
    "distances": {"left": None, "center": None, "right": None},
    "timestamp": None,
}


# ─────────────────────────────────────────────────────────────────────────────
# Serial-line parser
# ─────────────────────────────────────────────────────────────────────────────

def parse_serial_line(line: str) -> dict[str, Any] | None:
    """
    Parse a single Arduino serial line into a status dictionary.

    Expected format:
        STATE:TRACKING|POS:LEFT|LEFT:45.2|CENTER:120.0|RIGHT:350.0

    Returns ``None`` when the line does not match the expected format.
    """
    line = line.strip()
    if not line:
        return None

    record: dict[str, Any] = {}
    try:
        for token in line.split("|"):
            key, _, value = token.partition(":")
            key = key.strip().upper()
            value = value.strip()

            if key == "STATE":
                if value not in {"IDLE", "DETECTED", "TRACKING", "ALERT"}:
                    return None
                record["state"] = value

            elif key == "POS":
                if value not in {"NONE", "LEFT", "CENTER", "RIGHT"}:
                    return None
                record["position"] = value

            elif key in {"LEFT", "CENTER", "RIGHT"}:
                record.setdefault("distances", {})[key.lower()] = float(value)

        # Require all mandatory fields
        if not all(k in record for k in ("state", "position", "distances")):
            return None
        if not all(k in record["distances"] for k in ("left", "center", "right")):
            return None

        record["timestamp"] = datetime.now(timezone.utc).isoformat()
        return record

    except (ValueError, AttributeError):
        return None


def _push(record: dict[str, Any]) -> None:
    """Thread-safe insert into the history deque."""
    with _lock:
        _history.appendleft(record)


def latest_status() -> dict[str, Any]:
    """Return the most recent status record, or the empty sentinel."""
    with _lock:
        return dict(_history[0]) if _history else dict(_EMPTY_STATUS)


def get_history(n: int = HISTORY_MAXLEN) -> list[dict[str, Any]]:
    """Return up to *n* most-recent status records (newest first)."""
    with _lock:
        return list(_history)[:n]


# ─────────────────────────────────────────────────────────────────────────────
# Serial reader thread
# ─────────────────────────────────────────────────────────────────────────────

def _serial_reader_loop(port: str, baud: int) -> None:
    """
    Continuously read lines from the Arduino over a serial port.
    Reconnects automatically after errors.
    """
    try:
        import serial  # type: ignore[import]
    except ImportError:
        log.error(
            "pyserial is not installed. Run: pip install pyserial  "
            "(or use --simulate to run without hardware)"
        )
        return

    while True:
        try:
            log.info("Opening serial port %s at %d baud …", port, baud)
            with serial.Serial(port, baud, timeout=2) as ser:
                log.info("Serial port %s open.", port)
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    try:
                        line = raw.decode("utf-8", errors="replace")
                    except Exception:
                        continue
                    record = parse_serial_line(line)
                    if record:
                        _push(record)
                        log.debug("Parsed: %s", json.dumps(record))
                    else:
                        log.debug("Ignored line: %r", line.strip())
        except Exception as exc:  # serial.SerialException, OSError, …
            log.warning("Serial error (%s). Retrying in 3 s …", exc)
            time.sleep(3)


# ─────────────────────────────────────────────────────────────────────────────
# Simulator thread  (--simulate flag)
# ─────────────────────────────────────────────────────────────────────────────

def _simulator_loop() -> None:
    """
    Generate synthetic serial data for demonstration / development without
    physical hardware.
    """
    states    = ["IDLE", "DETECTED", "TRACKING", "ALERT"]
    positions = ["NONE", "LEFT", "CENTER", "RIGHT"]
    weights_s = [0.4, 0.2, 0.25, 0.15]
    weights_p = [0.3, 0.25, 0.2, 0.25]

    log.info("Simulator started – generating synthetic data every 0.5 s.")
    while True:
        state = random.choices(states, weights=weights_s)[0]
        pos   = "NONE" if state == "IDLE" else random.choices(
            positions[1:], weights=weights_p[1:]
        )[0]

        def _dist(near: bool) -> float:
            return round(random.uniform(20, 95) if near else random.uniform(101, 380), 1)

        d_left   = _dist(pos == "LEFT")
        d_center = _dist(pos == "CENTER")
        d_right  = _dist(pos == "RIGHT")

        line = (
            f"STATE:{state}|POS:{pos}"
            f"|LEFT:{d_left}|CENTER:{d_center}|RIGHT:{d_right}"
        )
        record = parse_serial_line(line)
        if record:
            _push(record)
        time.sleep(0.5)


# ─────────────────────────────────────────────────────────────────────────────
# Flask application
# ─────────────────────────────────────────────────────────────────────────────

app = Flask(__name__, static_folder=None)


@app.route("/status")
def route_status():
    """Return the most recent system status as JSON."""
    return jsonify(latest_status())


@app.route("/history")
def route_history():
    """Return the last 50 status records as a JSON array."""
    return jsonify(get_history(50))


@app.route("/")
def route_index():
    """Serve the frontend dashboard."""
    return send_from_directory(str(FRONTEND_DIR), "index.html")


@app.route("/<path:filename>")
def route_static(filename: str):
    """Serve any file from the frontend directory."""
    return send_from_directory(str(FRONTEND_DIR), filename)


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Intruder Detection System – Python backend"
    )
    parser.add_argument(
        "--port", type=int, default=5000,
        help="HTTP port for the web server (default: 5000)"
    )
    parser.add_argument(
        "--serial", default="/dev/ttyUSB0",
        help="Serial port connected to Arduino (default: /dev/ttyUSB0)"
    )
    parser.add_argument(
        "--baud", type=int, default=9600,
        help="Serial baud rate (default: 9600)"
    )
    parser.add_argument(
        "--simulate", action="store_true",
        help="Run without hardware: generate synthetic data"
    )
    args = parser.parse_args()

    # Start background data-source thread
    if args.simulate:
        t = threading.Thread(target=_simulator_loop, daemon=True)
    else:
        t = threading.Thread(
            target=_serial_reader_loop,
            args=(args.serial, args.baud),
            daemon=True,
        )
    t.start()

    log.info(
        "Starting web server on http://0.0.0.0:%d  (simulate=%s)",
        args.port, args.simulate,
    )
    app.run(host="0.0.0.0", port=args.port, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
