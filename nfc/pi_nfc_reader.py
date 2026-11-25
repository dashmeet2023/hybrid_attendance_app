#!/usr/bin/env python3
"""
Raspberry Pi side of the classroom RFID/NFC reader (ESP32/ESP8266 + RC522 or PN532 over USB serial).

Every card tap is sent to the attendance server, which decides the result:
  VERIFIED      student marked present in the open session for this room
  PENDING       session needs the CSI room check and the sensor is not sure yet: tap again
  REJECTED      no open session for this student here, card revoked, room empty (CSI) ...
  UNKNOWN_CARD  new card: the admin assigns it in Administration > NFC cards
  SESSION_OPENED / SESSION_CLOSED   teacher card: toggles the session for the teacher's class in this room
  ADMIN         administrator card: opens the live website on the Pi's screen
The result is sent back to the reader (LED pattern) and shown on the touchscreen kiosk.

Settings come from csi/.env (same room token as the CSI sensor on this Pi):
  BACKEND_URL=https://hybrid-attendance-app.vercel.app
  CSI_SERVICE_TOKEN=...
  NFC_PORT=/dev/serial/by-id/...   (optional; otherwise the first argument, then auto-detect)
  ROOM_NAME=Room 102               (optional; shown on the kiosk)

Run:  ~/hybrid_attendance_app/csi/venv/bin/python nfc/pi_nfc_reader.py [/dev/ttyUSB0]
Kiosk page: http://127.0.0.1:8090  (nfc/kiosk.sh opens it full-screen on the touchscreen)

Every tap and its result is also appended to nfc/taps.csv as a local log.
"""

import collections
import csv
import glob
import json
import os
import subprocess
import sys
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import requests
import serial

BAUD = 115200
HERE = Path(__file__).resolve().parent
ENV_FILE = HERE.parent / "csi" / ".env"
CSI_STATUS_FILE = HERE.parent / "csi" / "logs" / "latest.json"
KIOSK_HTML = HERE / "kiosk.html"
KIOSK_PORT = 8090
LOG_FILE = HERE / "taps.csv"
REPEAT_IGNORE_S = 3  # the same card held on the reader is reported only once
WEBSITE_PROFILE = "hybrid-website-window"  # marks the browser window opened by the Website button


class State:
    """What the kiosk shows; shared between the serial loop and the web server."""

    def __init__(self):
        self.lock = threading.Lock()
        self.reader_connected = False
        self.reader_info = ""
        self.port = ""
        self.taps = collections.deque(maxlen=5)
        self.room = ""
        self.backend = ""

    def snapshot(self):
        with self.lock:
            data = {
                "reader_connected": self.reader_connected,
                "reader_info": self.reader_info,
                "port": self.port,
                "room": self.room,
                "backend": self.backend,
                "taps": list(self.taps),
                "now": time.time(),
            }
        data["csi"] = read_csi_status()
        return data


STATE = State()


def read_csi_status():
    try:
        data = json.loads(CSI_STATUS_FILE.read_text())
        data["age_s"] = round(time.time() - data.get("written_at", 0), 1)
        return data
    except (OSError, ValueError):
        return None


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
    if os.getenv("NFC_PORT"):
        return os.environ["NFC_PORT"]
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 1
    # Keep DTR/RTS low so opening the port doesn't reset the board
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
        """Return (outcome, message, name of the card's owner)."""
        try:
            r = self.http.post(self.url, json={"uid": uid}, timeout=10)
        except requests.RequestException as e:
            return "OFFLINE", f"Server not reachable ({e.__class__.__name__}). Tap again.", None
        try:
            body = r.json()
        except ValueError:
            body = {}
        if r.status_code != 200:
            return "ERROR", f"HTTP {r.status_code}: {body.get('error', r.text[:120])}", None
        return body.get("outcome", "ERROR"), body.get("message", ""), body.get("student")


def close_website_window():
    """A tap brings the kiosk back to the front."""
    subprocess.run(["pkill", "-f", WEBSITE_PROFILE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def open_website_window():
    """Open the live website in its own window (maximized, under the taskbar). Returns (ok, message)."""
    import shutil
    url = STATE.backend
    profile = Path.home() / ".config" / WEBSITE_PROFILE
    profile.mkdir(parents=True, exist_ok=True)
    if not (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")):
        print("[website] no desktop session in this process (started over SSH?) - cannot open a window", flush=True)
        return False, "Start the kiosk from the desktop (reboot)"
    close_website_window()
    for browser in ("chromium", "chromium-browser", "firefox"):
        path = shutil.which(browser)
        if not path:
            continue
        if browser == "firefox":
            # Separate Firefox instance with its own profile, so a card tap can close just this window.
            cmd = [path, "--no-remote", "--new-instance", "--profile", str(profile), url]
        else:
            cmd = [path, f"--user-data-dir={profile}", "--start-maximized", "--noerrdialogs",
                   "--disable-session-crashed-bubble", f"--app={url}"]
        subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        print(f"[website] opened {url} with {browser}", flush=True)
        # Return to the kiosk automatically after 5 minutes.
        timer = threading.Timer(300, close_website_window)
        timer.daemon = True
        timer.start()
        return True, f"Opening website ({browser})"
    print("[website] no browser found (install chromium or firefox)", flush=True)
    return False, "No web browser installed"


def on_card(ser, server, uid):
    close_website_window()   # any tap brings the kiosk back to the front
    outcome, message, student = server.tap(uid)
    print(f"[{outcome}] {uid}  {message}", flush=True)
    log_tap(uid, outcome, message)
    with STATE.lock:
        STATE.taps.appendleft({"uid": uid, "outcome": outcome, "message": message,
                               "student": student, "at": time.time()})
    if outcome == "ADMIN":
        open_website_window()   # administrator card: open the live website on the Pi's screen
    # Show the decision on the reader's LED: VERIFIED = long flash, PENDING = 2 slow,
    # UNKNOWN_CARD = 3 medium, REJECTED = 6 fast, OFFLINE/ERROR = 10 very fast.
    # Teacher/admin card actions that succeeded are shown like VERIFIED.
    led = "VERIFIED" if outcome in ("SESSION_OPENED", "SESSION_CLOSED", "ADMIN") else outcome
    send(ser, "RESULT " + led)


def run(port, server):
    ser = open_port(port)
    time.sleep(2)
    ser.reset_input_buffer()
    send(ser, "INFO")
    send(ser, "START")
    with STATE.lock:
        STATE.reader_connected, STATE.port = True, port
    print(f"Listening on {port}, posting to {server.url}. Tap a card... (Ctrl+C to quit)", flush=True)
    last_uid, last_at = None, 0.0

    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print("[reader]", line, flush=True)
            continue

        event = msg.get("event")
        if event == "card":
            uid = str(msg.get("uid", "")).upper()
            if uid == last_uid and time.time() - last_at < REPEAT_IGNORE_S:
                continue
            last_uid, last_at = uid, time.time()
            on_card(ser, server, uid)
        elif event in ("ready", "info"):
            info = f"firmware {msg.get('fw')}, chip {msg.get('chip') or msg.get('pn532')}"
            with STATE.lock:
                STATE.reader_info = info
            print(f"[reader {event}] {info}", flush=True)
        elif event == "ok" or event == "pong":
            continue
        elif event == "error":
            print("[reader error]", msg.get("msg"), flush=True)
        else:
            print("[reader]", msg, flush=True)


class KioskHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body, ctype):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/status":
            return self.reply(200, json.dumps(STATE.snapshot()), "application/json")
        if self.path in ("/", "/index.html"):
            try:
                return self.reply(200, KIOSK_HTML.read_bytes(), "text/html; charset=utf-8")
            except OSError:
                return self.reply(500, "nfc/kiosk.html is missing", "text/plain")
        self.reply(404, "not found", "text/plain")

    def do_POST(self):
        if self.path == "/open-website":
            ok, message = open_website_window()
            return self.reply(200 if ok else 500, json.dumps({"opened": ok, "message": message}), "application/json")
        self.reply(404, "not found", "text/plain")


def start_kiosk_server():
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", KIOSK_PORT), KioskHandler)
    except OSError as e:
        print(f"Kiosk page not started ({e}); is another copy running?", flush=True)
        return
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print(f"Kiosk page: http://127.0.0.1:{KIOSK_PORT}", flush=True)


def main():
    load_env(ENV_FILE)
    url, token = os.getenv("BACKEND_URL"), os.getenv("CSI_SERVICE_TOKEN")
    if not url or not token:
        sys.exit(f"Set BACKEND_URL and CSI_SERVICE_TOKEN in {ENV_FILE}")
    STATE.room, STATE.backend = os.getenv("ROOM_NAME", ""), url.rstrip("/")
    server = Server(url, token)
    start_kiosk_server()
    while True:
        port = find_port()
        if not port:
            print("Reader not found (set NFC_PORT in csi/.env). Is the USB cable plugged in? Retrying...", flush=True)
            time.sleep(3)
            continue
        try:
            run(port, server)
        except serial.SerialException as e:
            with STATE.lock:
                STATE.reader_connected = False
            print(f"Serial error ({e}). Reconnecting in 3 s...", flush=True)
            time.sleep(3)
        except KeyboardInterrupt:
            print("\nBye.")
            return


if __name__ == "__main__":
    main()
