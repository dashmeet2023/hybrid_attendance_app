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

# Screen layout: the kiosk runs on the 3.5" LCD (SPI-1, rotated to landscape) and the HDMI monitor shows a
# live full-screen copy of it (wl-mirror; install once: sudo apt install -y wl-mirror). Without a monitor
# the kiosk simply runs on the LCD. SCREEN_TRANSFORM in csi/.env: 90 or 270 if the LCD is upside down.
T=$(grep -E '^SCREEN_TRANSFORM=' "$APP/csi/.env" 2>/dev/null | cut -d= -f2); T=${T:-90}
HDMI=$(wlr-randr 2>/dev/null | awk '/^HDMI-A-[0-9]/{print $1; exit}')
sleep 3   # let the desktop finish applying its own screen layout first
wlr-randr --output SPI-1 --transform "$T" --pos 0,0 2>/dev/null
# Open the kiosk while the LCD is the only screen, so it is placed there.
[ -n "$HDMI" ] && wlr-randr --output "$HDMI" --off 2>/dev/null
sleep 1

BROWSER=firefox
for b in chromium chromium-browser; do command -v "$b" >/dev/null && BROWSER=$b && break; done
if [ "$BROWSER" = firefox ]; then
  firefox --kiosk http://127.0.0.1:8090 &
else
  "$BROWSER" --user-data-dir="$HOME/.config/hybrid-kiosk" --kiosk --noerrdialogs \
    --disable-session-crashed-bubble --disable-infobars --check-for-update-interval=31536000 \
    --app=http://127.0.0.1:8090 &
fi
KIOSK=$!
sleep 8

if [ -n "$HDMI" ]; then
  wlr-randr --output "$HDMI" --on --preferred --scale 1 --pos 480,0 2>/dev/null
  sleep 2
  if command -v wl-mirror >/dev/null; then
    # Mirror the LCD onto the monitor, full screen, keeping the shape (black side bars on 16:9).
    ( while kill -0 $KIOSK 2>/dev/null; do wl-mirror --fullscreen-output "$HDMI" SPI-1 >/dev/null 2>&1; sleep 3; done ) &
  fi
fi
wait $KIOSK
