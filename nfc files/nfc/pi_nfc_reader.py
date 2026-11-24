#!/usr/bin/env python3
"""
Raspberry Pi side of the classroom NFC reader (ESP32-C3 + PN532 bridge over USB serial).

Every card tap is sent to the attendance server, which decides the result:
  VERIFIED      student marked present in the open session for this room (reader blinks)
  PENDING       session needs the CSI room check and the sensor is not sure yet: tap again
  REJECTED      no open session for this student here, card revoked, room empty (CSI) ...
  UNKNOWN_CARD  new card: the admin assigns it in Administration > NFC cards

Settings come from csi/.env (same room token as the CSI sensor on this Pi):
  BACKEND_URL=https://hybrid-attendance-app.vercel.app
  CSI_SERVICE_TOKEN=...

Run:  ~/hybrid_attendance_app/csi/venv/bin/python nfc/pi_nfc_reader.py            (auto-finds /dev/ttyACM*)
      ~/hybrid_attendance_app/csi/venv/bin/python nfc/pi_nfc_reader.py /dev/ttyACM0

Every tap and its result is also appended to nfc/taps.csv as a local log.
"""

import csv
import glob
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import requests
import serial

BAUD = 115200
HERE = Path(__file__).resolve().parent
ENV_FILE = HERE.parent / "csi" / ".env"
LOG_FILE = HERE / "taps.csv"
REPEAT_IGNORE_S = 3  # the same card held on the reader is reported only once


def load_env(path):
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    ports = sorted(glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 1
    # Keep DTR/RTS low so opening the port doesn't reset the ESP32-C3
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def send(ser, cmd):
    ser.write((cmd + "\n").encode())


def log_tap(uid, outcome, detail):
    new_file = not LOG_FILE.exists()
    now = datetime.now()
    with open(LOG_FILE, "a", newline="") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow(["date", "time", "uid", "outcome", "detail"])
        w.writerow([now.date().isoformat(), now.strftime("%H:%M:%S"), uid, outcome, detail])


class Server:
    def __init__(self, url, token):
        self.url = url.rstrip("/") + "/api/nfc/tap"
        self.http = requests.Session()
        self.http.headers.update({"Authorization": "Bearer " + token})

    def tap(self, uid):
        try:
            r = self.http.post(self.url, json={"uid": uid}, timeout=10)
        except requests.RequestException as e:
            return "OFFLINE", f"Server not reachable ({e.__class__.__name__}). Tap again."
        try:
            body = r.json()
        except ValueError:
            body = {}
        if r.status_code != 200:
            return "ERROR", f"HTTP {r.status_code}: {body.get('error', r.text[:120])}"
        return body.get("outcome", "ERROR"), body.get("message", "")


def on_card(ser, server, uid):
    outcome, message = server.tap(uid)
    print(f"[{outcome}] {uid}  {message}", flush=True)
    log_tap(uid, outcome, message)
    if outcome == "VERIFIED":
        send(ser, "BLINK")  # confirm on the reader


def run(port, server):
    ser = open_port(port)
    time.sleep(2)
    ser.reset_input_buffer()
    send(ser, "INFO")
    send(ser, "START")
    print(f"Listening on {port}, posting to {server.url}. Tap a card... (Ctrl+C to quit)", flush=True)
    last_uid, last_at = None, 0.0

    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print("[esp32]", line, flush=True)
            continue

        event = msg.get("event")
        if event == "card":
            uid = str(msg.get("uid", "")).upper()
            if uid == last_uid and time.time() - last_at < REPEAT_IGNORE_S:
                continue
            last_uid, last_at = uid, time.time()
            on_card(ser, server, uid)
        elif event == "error":
            print("[esp32 error]", msg.get("msg"), flush=True)
        else:
            print("[esp32]", msg, flush=True)


def main():
    load_env(ENV_FILE)
    url, token = os.getenv("BACKEND_URL"), os.getenv("CSI_SERVICE_TOKEN")
    if not url or not token:
        sys.exit(f"Set BACKEND_URL and CSI_SERVICE_TOKEN in {ENV_FILE}")
    server = Server(url, token)
    while True:
        port = find_port()
        if not port:
            print("ESP32-C3 not found on /dev/ttyACM*. Is the USB-C cable plugged in? Retrying...", flush=True)
            time.sleep(3)
            continue
        try:
            run(port, server)
        except serial.SerialException as e:
            print(f"Serial error ({e}). Reconnecting in 3 s...", flush=True)
            time.sleep(3)
        except KeyboardInterrupt:
            print("\nBye.")
            return


if __name__ == "__main__":
    main()
