#!/bin/bash
# Starts the RFID reader service and opens the kiosk full-screen on the touchscreen.
# Launched at login by ~/.config/autostart/hybrid-kiosk.desktop (see nfc/README).
APP="$HOME/hybrid_attendance_app"
PY="$APP/csi/venv/bin/python"
LOG="$APP/nfc/reader.log"

# Reader + kiosk web server, restarted if it ever exits.
if ! pgrep -f "nfc/pi_nfc_reader.py" >/dev/null; then
  ( while true; do
      "$PY" "$APP/nfc/pi_nfc_reader.py" >>"$LOG" 2>&1
      echo "$(date) reader exited, restarting in 5 s" >>"$LOG"
      sleep 5
    done ) &
fi

# Wait for the kiosk page, then open it full-screen.
for i in $(seq 1 30); do
  curl -fs http://127.0.0.1:8090/status >/dev/null && break
  sleep 1
done
xset s off -dpms 2>/dev/null   # keep the screen on (X11); harmless on Wayland

# Rotate the 3.5" LCD (SPI-1) to landscape and mirror it on the HDMI monitor: the monitor runs at
# 720x480 with scale 1.5 (= 480x320, same shape) at the same position, so both show the same picture.
# Change SCREEN_TRANSFORM in csi/.env (90 or 270) if the picture is upside down.
T=$(grep -E '^SCREEN_TRANSFORM=' "$APP/csi/.env" 2>/dev/null | cut -d= -f2); T=${T:-90}
if command -v wlr-randr >/dev/null; then
  sleep 3   # let the desktop finish applying its own screen layout first
  wlr-randr --output SPI-1 --transform "$T" --pos 0,0 --output HDMI-A-1 --mode 720x480 --scale 1.5 --pos 0,0 2>/dev/null \
    || wlr-randr --output SPI-1 --transform "$T" --pos 0,0 --output HDMI-A-1 --custom-mode 720x480 --scale 1.5 --pos 0,0 2>/dev/null \
    || wlr-randr --output SPI-1 --transform "$T" --pos 0,0 2>/dev/null   # no monitor connected
fi
for browser in chromium chromium-browser; do
  if command -v "$browser" >/dev/null; then
    exec "$browser" --user-data-dir="$HOME/.config/hybrid-kiosk" --kiosk --noerrdialogs \
      --disable-session-crashed-bubble --disable-infobars --check-for-update-interval=31536000 \
      --app=http://127.0.0.1:8090
  fi
done
exec firefox --kiosk http://127.0.0.1:8090
