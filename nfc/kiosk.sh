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

# Screen layout (true mirror):
#   HDMI monitor = the normal desktop (taskbar on top) at its native resolution, scale HDMI_SCALE (default 2)
#   3.5" LCD (SPI-1) = live full copy of the monitor (wl-mirror), fitted with thin bars, all edges visible.
#   The kiosk opens as a normal maximized window under the taskbar.
# One-time: sudo apt install -y wl-mirror ; put the taskbar on the monitor (right-click taskbar >
# Panel Preferences > Position > Monitor: HDMI). Without a monitor the kiosk runs full screen on the LCD.
# csi/.env: SCREEN_TRANSFORM=90|270 (LCD rotation), HDMI_SCALE=2 (bigger = larger text on both screens).
setting() { grep -E "^$1=" "$APP/csi/.env" 2>/dev/null | tail -1 | cut -d= -f2; }
T=$(setting SCREEN_TRANSFORM); T=${T:-90}
S=$(setting HDMI_SCALE); S=${S:-2}
HDMI=$(wlr-randr 2>/dev/null | awk '/^HDMI-A-[0-9]/{print $1; exit}')
sleep 3   # let the desktop finish applying its own screen layout first

BROWSER=firefox
for b in chromium chromium-browser; do command -v "$b" >/dev/null && BROWSER=$b && break; done
open_kiosk() {   # $1 = extra mode flag: --kiosk (full screen) or --start-maximized (window under the taskbar)
  if [ "$BROWSER" = firefox ]; then
    if [ "$1" = --kiosk ]; then firefox --kiosk http://127.0.0.1:8090 & else firefox http://127.0.0.1:8090 & fi
  else
    "$BROWSER" --user-data-dir="$HOME/.config/hybrid-kiosk" "$1" --noerrdialogs --disable-session-crashed-bubble \
      --disable-infobars --check-for-update-interval=31536000 --app=http://127.0.0.1:8090 &
  fi
  KIOSK=$!
}

if [ -z "$HDMI" ]; then
  # LCD only: kiosk full screen on the 3.5" screen.
  wlr-randr --output SPI-1 --transform "$T" --pos 0,0 2>/dev/null
  open_kiosk --kiosk
  wait $KIOSK
  exit 0
fi

# Monitor + LCD: open the kiosk while the monitor is the only screen, so it is placed there.
wlr-randr --output "$HDMI" --on --preferred --scale "$S" --pos 0,0 --output SPI-1 --off 2>/dev/null
sleep 2
open_kiosk --start-maximized
sleep 8
# Bring the LCD back (to the right of the monitor) and fill it with a live copy of the monitor.
W=$(wlr-randr 2>/dev/null | awk -v o="$HDMI" '$1==o{f=1} f&&/current/{split($1,a,"x"); print int(a[1]/'"$S"'); exit}')
wlr-randr --output SPI-1 --on --transform "$T" --pos "${W:-1600},0" 2>/dev/null
sleep 2
if command -v wl-mirror >/dev/null; then
  ( while kill -0 $KIOSK 2>/dev/null; do wl-mirror -F --fullscreen-output SPI-1 "$HDMI" >/dev/null 2>&1; sleep 3; done ) &
fi
wait $KIOSK
